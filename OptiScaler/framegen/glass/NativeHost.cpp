#include "pch.h"
#include "GeometryDrawCapture.h"
#include "GeometryCreation.h"
#include "NativeHost.h"
#include "NativeSession.h"
#include "D3D12Observer.h"
#include "CyberpunkSurfacePass.h"
#include "StreamlineTagBridge.h"
#include <Util.h>
#include <memory>
#include <mutex>
#include <wrl/client.h>

namespace GlassFg
{
namespace
{
struct Entry
{
    NativeSession session;
    const NVSDK_NGX_Handle* handle = nullptr;
    ID3D12GraphicsCommandList* command = nullptr;
    D3D12_RESOURCE_DESC descriptions[3] {};
    unsigned evaluations = 0;

    bool matches(ID3D12GraphicsCommandList* candidate, const Inputs& inputs) const
    {
        if (candidate != command)
            return false;
        ID3D12Resource* resources[] = { inputs.motion, inputs.color, inputs.depth };
        for (unsigned i = 0; i < 3; ++i)
        {
            const auto desc = resources[i]->GetDesc();
            const auto& old = descriptions[i];
            if (desc.Width != old.Width || desc.Height != old.Height || desc.Format != old.Format ||
                desc.Dimension != old.Dimension || desc.SampleDesc.Count != old.SampleDesc.Count ||
                desc.MipLevels != old.MipLevels || desc.DepthOrArraySize != old.DepthOrArraySize)
                return false;
        }
        return true;
    }
};
struct Runtime
{
    std::recursive_mutex mutex;
    std::shared_ptr<Entry> active;
    std::atomic<ID3D12GraphicsCommandList*> activeCommand = nullptr;
    std::atomic<bool> submissionObserved = false;
    std::array<std::shared_ptr<Entry>, 2> retiring;
    CyberpunkSurfacePass selector;
    FILE* log = nullptr;
    bool selectorAttempted = false, unavailable = false, stopped = false;
    uint64_t evaluations = 0, substitutions = 0, captures = 0;

    void reap()
    {
        InternalD3D12Scope ownCalls;
        for (auto& entry : retiring)
            if (entry && !entry->evaluations && entry->session.readyToRelease())
            {
                entry->session.releaseAfterGpuDrain();
                entry.reset();
            }
    }
    bool retire()
    {
        reap();
        if (!active)
            return true;
        activeCommand.store(nullptr, std::memory_order_release);
        active->session.stop();
        for (auto& slot : retiring)
            if (!slot)
            {
                slot = std::move(active);
                return true;
            }
        return false; // Bounded VRAM retention; no wait or unsafe early free.
    }
    template <class Function> void each(Function function)
    {
        if (active)
            function(*active);
        for (auto& entry : retiring)
            if (entry)
                function(*entry);
    }
};
// Hooks and outstanding destruction notifications can outlive NGX shutdown.
// No static destructor releases resources from an in-flight recording.
Runtime& runtime()
{
    static auto* value = new Runtime;
    return *value;
}

D3D12Callbacks makeCallbacks()
{
    D3D12Callbacks value;
    value.context = &runtime();
    value.enter = [](void* p) { static_cast<Runtime*>(p)->mutex.lock(); };
    value.leave = [](void* p) { static_cast<Runtime*>(p)->mutex.unlock(); };
    value.stateTracked = [](void* p, ID3D12GraphicsCommandList* c)
    { return static_cast<Runtime*>(p)->activeCommand.load(std::memory_order_acquire) == c; };
    value.reset = [](void* p, ID3D12GraphicsCommandList* c, bool okay, ID3D12PipelineState* initial)
    {
        auto& r = *static_cast<Runtime*>(p);
        InternalD3D12Scope ownCalls;
        r.each([&](Entry& e) { e.session.onReset(c, okay, initial); });
        r.reap();
    };
    value.mutation = [](void* p, ID3D12GraphicsCommandList* c)
    { static_cast<Runtime*>(p)->each([&](Entry& e) { e.session.onStateMutation(c); }); };
    value.barrier = [](void* p, ID3D12GraphicsCommandList* c, UINT count, const D3D12_RESOURCE_BARRIER* barriers)
    {
        auto& r = *static_cast<Runtime*>(p);
        if (!r.active || r.stopped || !barriers || count > 4096 || !ReadControls().active() ||
            c->GetType() != D3D12_COMMAND_LIST_TYPE_DIRECT)
            return;
        auto& e = *r.active;
        for (UINT i = 0; i < count; ++i)
            if (r.selector.matches(barriers[i], static_cast<unsigned>(e.descriptions[0].Width),
                                   e.descriptions[0].Height))
            {
                InternalD3D12Scope ownCalls;
                if (e.session.captureIdentifiedSurface(c, barriers[i].Transition.pResource,
                                                       barriers[i].Transition.StateAfter))
                    ++r.captures;
            }
    };
    value.beforeSubmit = [](void*, ID3D12CommandQueue* q, UINT count, ID3D12CommandList* const* lists)
    {
        InternalD3D12Scope ownCalls;
        NotifyGeometryCaptureBeforeSubmit(q, count, lists);
    };
    value.submit = [](void* p, ID3D12CommandQueue* q, UINT count, ID3D12CommandList* const* lists)
    {
        auto& r = *static_cast<Runtime*>(p);
        InternalD3D12Scope ownSignals;
        NotifyGeometryCaptureSubmit(q, count, lists);
        r.each([&](Entry& e) { e.session.afterSubmit(q, count, lists); });
        r.reap();
    };
    value.signal = [](void* p, ID3D12CommandQueue* q, ID3D12Fence* fence, UINT64 number)
    { static_cast<Runtime*>(p)->each([&](Entry& e) { e.session.onSignal(q, fence, number); }); };
    value.wait = [](void* p, ID3D12CommandQueue* q, ID3D12Fence* fence, UINT64 number)
    { static_cast<Runtime*>(p)->each([&](Entry& e) { e.session.onWait(q, fence, number); }); };
    return value;
}

std::shared_ptr<Entry> acquire(Runtime& r, ID3D12GraphicsCommandList* command, const NVSDK_NGX_Handle* handle,
                               const Inputs& inputs, Controls controls)
{
    r.reap();
    if (r.stopped || r.unavailable || !command || !handle || !inputs.valid())
        return {};
    if (r.active &&
        (!r.active->session.accepting() || r.active->handle != handle || !r.active->matches(command, inputs)))
        if (!r.retire())
            return {};
    if (r.active)
        return r.active;
    if (!controls.active() || inputs.index != 1)
        return {};
    for (const auto& entry : r.retiring)
        if (entry)
            return {}; // Drain old-size recordings before reuse.
    InternalD3D12Scope ownCalls;
    if (!r.selectorAttempted)
    {
        r.selectorAttempted = true;
        const auto path = Util::DllPath().parent_path() / L"OptiScaler.Glass.log";
        r.log = _wfopen(path.c_str(), L"a");
        if (!r.log || !r.selector.initialize(GetModuleHandleW(nullptr), r.log))
            r.unavailable = true;
    }
    if (r.unavailable)
        return {};
    if (!InstallD3D12Observer(command, makeCallbacks()))
    {
        r.unavailable = true;
        std::fprintf(r.log, "NATIVE_HOST ready=0 reason=observer_coverage\n");
        std::fflush(r.log);
        return {};
    }
    r.submissionObserved.store(true, std::memory_order_release);
    auto entry = std::make_shared<Entry>();
    entry->handle = handle;
    entry->command = command;
    entry->descriptions[0] = inputs.motion->GetDesc();
    entry->descriptions[1] = inputs.color->GetDesc();
    entry->descriptions[2] = inputs.depth->GetDesc();
    Microsoft::WRL::ComPtr<ID3D12Device> device;
    if (FAILED(command->GetDevice(IID_PPV_ARGS(&device))))
        return {};
    const auto shaders = Util::DllPath().parent_path() / L"Glass";
    if (!entry->session.initialize(device.Get(), entry->descriptions, (shaders / L"GlassSurface.hlsl").c_str(),
                                   (shaders / L"GlassRegion.hlsl").c_str(), r.log))
    {
        r.unavailable = true;
        return {};
    }
    const auto mask = ObservedComputeMethods();
    if (!entry->session.bindFgCommand(command, mask, mask))
    {
        entry->session.releaseAfterGpuDrain(); // Initialization recorded nothing.
        r.unavailable = true;
        return {};
    }
    r.active = entry;
    r.activeCommand.store(command, std::memory_order_release);
    std::fprintf(r.log, "NATIVE_HOST ready=1 handle=%p command=%p methods=%x width=%llu height=%u\n", handle, command,
                 mask, entry->descriptions[0].Width, entry->descriptions[0].Height);
    std::fflush(r.log);
    return entry;
}
} // namespace

bool NativeCaptureSubmissionReady() noexcept
{
    return runtime().submissionObserved.load(std::memory_order_acquire);
}

NVSDK_NGX_Result EvaluateNativeFG(ID3D12GraphicsCommandList* command, const NVSDK_NGX_Handle* handle,
                                  NVSDK_NGX_Parameter* parameters, PFN_NVSDK_NGX_ProgressCallback callback,
                                  NativeEvaluate original)
{
    if (!original)
        return NVSDK_NGX_Result_FAIL_FeatureNotFound;
    auto& r = runtime();
    Inputs inputs;
    std::shared_ptr<Entry> entry;
    PreparedInputs prepared;
    const auto controls = ReadControls();
    if (Inputs::read(parameters, inputs))
    {
        std::lock_guard lock(r.mutex);
        entry = acquire(r, command, handle, inputs, controls);
        if (!entry)
            PublishRuntimeStatus(r.stopped                                    ? RuntimeStatus::Stopped
                                 : r.unavailable                              ? RuntimeStatus::Unavailable
                                 : r.active || r.retiring[0] || r.retiring[1] ? RuntimeStatus::Retiring
                                                                              : RuntimeStatus::Waiting);
        if (entry)
        {
            ++entry->evaluations;
            D3D12_RESOURCE_STATES states[3] {};
            ID3D12Resource* resources[] = { inputs.motion, inputs.color, inputs.depth };
            InternalD3D12Scope ownCalls;
            if (ReadStreamlineStates(handle, inputs.index, inputs.count, resources, states))
                prepared = entry->session.prepare(command, inputs, states, controls);
            else
                entry->session.bypass(inputs.index);
        }
    }
    else
    {
        std::lock_guard lock(r.mutex);
        if (r.active && r.active->handle == handle)
            r.active->session.bypass(1);
        PublishRuntimeStatus(RuntimeStatus::Waiting);
    }
    NVSDK_NGX_Result result;
    bool applied = false;
    // Native NGX can use other threads. Do not hold the global observer mutex
    // across its evaluation; the entry is retained until this call completes.
    try
    {
        ScopedInputs<NVSDK_NGX_Parameter> substitute(parameters, prepared);
        applied = substitute.applied();
        result = original(command, handle, parameters, callback);
    }
    catch (...)
    {
        if (entry)
        {
            std::lock_guard lock(r.mutex);
            entry->session.nativeFailure();
            --entry->evaluations;
        }
        throw;
    }
    if (entry)
    {
        std::lock_guard lock(r.mutex);
        --entry->evaluations;
        ++r.evaluations;
        r.substitutions += applied;
        PublishRuntimeStatus(applied && result == NVSDK_NGX_Result_Success ? RuntimeStatus::Correcting
                                                                           : RuntimeStatus::Waiting);
        if (result != NVSDK_NGX_Result_Success || (prepared.motion && !applied))
            entry->session.nativeFailure();
        if (auto timing = entry->session.pollTiming())
            PublishGpuMilliseconds(timing->milliseconds);
        if (r.log && (r.evaluations <= 3 || r.evaluations % 300 == 0))
        {
            std::fprintf(r.log, "NATIVE_HOST evaluations=%llu substitutions=%llu captures=%llu result=%x\n",
                         r.evaluations, r.substitutions, r.captures, static_cast<unsigned>(result));
            ReportGeometryHost(r.log);
            std::fflush(r.log);
        }
        r.reap();
    }
    return result;
}

void RetireNativeFG(const NVSDK_NGX_Handle* handle)
{
    auto& r = runtime();
    std::lock_guard lock(r.mutex);
    if (r.active && r.active->handle == handle)
        r.retire();
    r.reap();
}
void StopNativeFG()
{
    auto& r = runtime();
    std::lock_guard lock(r.mutex);
    if (r.log)
    {
        std::fprintf(r.log, "NATIVE_HOST stop=1 evaluations=%llu substitutions=%llu captures=%llu\n",
                     r.evaluations, r.substitutions, r.captures);
        std::fflush(r.log);
    }
    r.stopped = true;
    r.retire();
    r.reap();
}
void CreatedNativeFG()
{
    auto& r = runtime();
    std::lock_guard lock(r.mutex);
    // Only the host's successful native FG creation reopens admission after
    // shutdown. Retiring recordings still prevent allocation until drained.
    r.stopped = false;
    r.reap();
}
} // namespace GlassFg
