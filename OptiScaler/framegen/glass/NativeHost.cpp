#include "pch.h"
#include "GeometryDrawCapture.h"
#include "GeometryCreation.h"
#include "GeometryHealth.h"
#include "NativeHost.h"
#include "NativeSession.h"
#include "D3D12Observer.h"
#include "StreamlineTagBridge.h"
#include "PackedMotionCapture.h"
#include "GlassMotionIdentity.h"
#include "GlassDebugControl.h"
#include "NvngxDlssgBridge.h"
#include "NgxParameterProbe.h"
#include "GeometryHealth.h"
#include "GlassHostTiming.h"
#include "GeometryCommands.h"
#include <hooks/Streamline_Hooks.h>
#include <Util.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <memory>
#include <mutex>
#include <thread>
#include <wrl/client.h>

namespace GlassFg
{
namespace
{
// Rotation limit for the module log. Enforced at open and again during the
// session, because the step trace keeps writing while the game runs.
constexpr std::uintmax_t kLogLimit = 32ull * 1024 * 1024;
// Per-callback CPU cost, reported once per health sample. Two steady_clock
// reads and one relaxed atomic add are the whole cost on the normal path.
struct TimingScope
{
    HostTiming& timing;
    std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
    explicit TimingScope(HostTiming& value) : timing(value) {}
    ~TimingScope()
    {
        timing.add(static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - start)
                .count()));
    }
};

struct Entry
{
    NativeSession session;
    const NVSDK_NGX_Handle* handle = nullptr;
    ID3D12GraphicsCommandList* command = nullptr;
    D3D12_RESOURCE_DESC descriptions[3] {};
    // Frame generation input identities of the run this session was created
    // for. The session does not own them; they only make "the same frame
    // generation inputs" decidable when the feature instance changes.
    ID3D12Resource* motionInput = nullptr;
    ID3D12Resource* depthInput = nullptr;
    unsigned evaluations = 0;
    bool releaseLogged = false;

    bool matches(ID3D12GraphicsCommandList* candidate, const Inputs& inputs) const
    {
        (void) candidate; // Command identity is owned by the session (one per back buffer).
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
    FILE* log = nullptr;
    bool logAttempted = false, unavailable = false, stopped = false;
    uint64_t evaluations = 0, substitutions = 0, captures = 0;
    // Last frame generation result, reported by the background log pass instead
    // of by the callback that observes it.
    unsigned lastResult = 0;
    // Per-generated-frame index split of the two counters above. Index 0 is the
    // real frame, 1..n are the generated ones; a missing substitution on any of
    // them means that frame kept the engine's original motion vectors.
    uint64_t evaluationsByIndex[8] {}, substitutionsByIndex[8] {};
    // A skipped substitution is only harmless when the motion texture it would
    // have written was already corrected by an earlier evaluation of the same
    // engine frame (the host evaluates one frame once per back buffer). A skip
    // on a texture that was never corrected leaves that frame with the engine's
    // own motion vectors.
    ID3D12Resource* correctedMotion[4] {};
    unsigned correctedMotionNext = 0;
    uint64_t unsubstitutedReusedMotion = 0, unsubstitutedFreshMotion = 0;
    // Runtime extent change: the frame-generation inputs were rebuilt at a new
    // size. The draw capture is process-resident, so it is released and rebuilt
    // at a quiescent point (no active or retiring session) instead of leaving
    // the correction off for the rest of the session.
    std::uint32_t rebuildWidth = 0, rebuildHeight = 0;
    bool rebuildPending = false;
    unsigned rebuildAttempts = 0;
    Microsoft::WRL::ComPtr<ID3D12Device> rebuildDevice;

    void reap()
    {
        InternalD3D12Scope ownCalls;
        for (auto& entry : retiring)
            if (entry && !entry->evaluations)
            {
                if (!entry->session.readyToRelease())
                {
                    // Evidence for the 2026-09-15 reset: the previous build
                    // released the packed outputs here while the compose list
                    // was still executing on the FG queue.
                    if (log && !entry->releaseLogged)
                    {
                        entry->releaseLogged = true;
                        std::fprintf(log,
                                     "NATIVE_RETIRE blocked compose_inflight=%u submitted=%llu completed=%llu\n",
                                     entry->session.packedComposeInFlight() ? 1u : 0u,
                                     static_cast<unsigned long long>(entry->session.packedComposeSubmitted()),
                                     static_cast<unsigned long long>(entry->session.packedComposeCompleted()));
                        std::fflush(log);
                    }
                    continue;
                }
                if (log)
                {
                    std::fprintf(
                        log, "NATIVE_RETIRE release compose_inflight=%u submitted=%llu completed=%llu forced=%llu\n",
                        entry->session.packedComposeInFlight() ? 1u : 0u,
                        static_cast<unsigned long long>(entry->session.packedComposeSubmitted()),
                        static_cast<unsigned long long>(entry->session.packedComposeCompleted()),
                        static_cast<unsigned long long>(entry->session.packedComposeForced()));
                    std::fflush(log);
                }
                entry->session.releaseAfterGpuDrain();
                entry.reset();
            }
        if (rebuildPending && !active && !retiring[0] && !retiring[1] && rebuildDevice)
        {
            const auto previous = ReadPackedMotionCaptureStatus();
            const auto width = rebuildWidth, height = rebuildHeight;
            rebuildPending = false;
            const bool released = ReleasePackedMotionCapture();
            const bool ready = InitializePackedMotionCapture(rebuildDevice.Get(), width, height, log,
                                                             MakeGlassMotionIdentityProvider());
            ++rebuildAttempts;
            if (log)
            {
                std::fprintf(log,
                             "PACKED_CAPTURE extent_rebuild released=%u old=%ux%u new=%ux%u ready=%u attempt=%u\n",
                             released ? 1u : 0u, previous.width, previous.height, width, height, ready ? 1u : 0u,
                             rebuildAttempts);
                std::fflush(log);
            }
            if (ready)
            {
                rebuildAttempts = 0;
                rebuildDevice.Reset();
            }
            else if (rebuildAttempts < 3)
                rebuildPending = true; // Retry on the next health pass.
            else
                rebuildDevice.Reset();
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
    // Compose-fence view across every retained entry. Called with mutex held.
    std::uint64_t activeComposeSubmitted() const
    {
        std::uint64_t value = 0;
        if (active) value = std::max(value, active->session.packedComposeSubmitted());
        for (const auto& entry : retiring)
            if (entry) value = std::max(value, entry->session.packedComposeSubmitted());
        return value;
    }
    std::uint64_t activeComposeCompleted() const
    {
        std::uint64_t value = 0;
        if (active) value = std::max(value, active->session.packedComposeCompleted());
        for (const auto& entry : retiring)
            if (entry) value = std::max(value, entry->session.packedComposeCompleted());
        return value;
    }
    std::uint64_t activeComposeForced() const
    {
        std::uint64_t value = 0;
        if (active) value = std::max(value, active->session.packedComposeForced());
        for (const auto& entry : retiring)
            if (entry) value = std::max(value, entry->session.packedComposeForced());
        return value;
    }
    bool anyComposeInFlight() const
    {
        if (active && active->session.packedComposeInFlight())
            return true;
        for (const auto& entry : retiring)
            if (entry && entry->session.packedComposeInFlight())
                return true;
        return false;
    }
};
// Hooks and outstanding destruction notifications can outlive NGX shutdown.
// No static destructor releases resources from an in-flight recording.
Runtime& runtime()
{
    static auto* value = new Runtime;
    return *value;
}

// Live control requests are queued from the file poll (health thread) and
// consumed on the FG evaluation thread, which is the only place the native
// recordings may be retired while the game keeps running.
std::atomic<bool> softReloadRequested = false;
std::atomic<bool> dumpRequested = false;
// Bounded retry counter for a dump that arrived while no session was active.
std::atomic<unsigned> dumpRetries { 0 };
// Set from the loader lock by NoteProcessAttach, written into the session log
// by the first health-service tick.
std::atomic<bool> attachPending = false;
// The driver-level frame generation block carries no Streamline frame token, so
// the packed capture is keyed by this counter instead.
std::atomic<std::uint64_t> driverFrameCounter { 0 };


std::filesystem::path packedShaderPath()
{
    return Util::DllPath().parent_path() / L"Glass" / L"GlassObjectMotion.hlsl";
}

// Crash forensics: this marker exists only while the batch that carries our
// compose is in flight. After a driver reset the file is still there, which
// proves the reset happened in a batch that contained our work.
// The file is a state marker, so it is written only on the transitions: one
// create when the first compose of a run is queued and one delete when the last
// one completes. Re-creating it every frame cost 1.0ms at the median and up to
// 19.9ms at the tail on the render thread (1500-op probe, 2026-09-16), which
// breaks the 1ms frame budget on its own.
std::atomic<bool> composeMarkerActive { false };
bool writeComposeMarker(UINT64 value, bool active)
{
    const auto path = Util::DllPath().parent_path() / L"Glass" / L"glass-compose.pending";
    if (!active)
    {
        std::error_code error;
        std::filesystem::remove(path, error);
        return true;
    }
    FILE* file = _wfopen(path.c_str(), L"wb");
    if (!file)
        return false;
    std::fprintf(file, "tick=%llu producer=%llu\n", static_cast<unsigned long long>(GetTickCount64()),
                 static_cast<unsigned long long>(value));
    std::fclose(file);
    return true;
}

void raiseComposeMarker(UINT64 value)
{
    if (composeMarkerActive.load(std::memory_order_relaxed))
        return;
    // The flag is published only after the file exists, so a failed create is
    // retried on the next frame instead of silently dropping the marker.
    if (writeComposeMarker(value, true))
        composeMarkerActive.store(true, std::memory_order_release);
}

void clearComposeMarker()
{
    if (!composeMarkerActive.exchange(false, std::memory_order_acq_rel))
        return;
    writeComposeMarker(0, false);
}

// Automatic staged ramp for unattended sessions: the same order the manual
// probe/apply protocol uses, driven by elapsed time after the first FG frame.
void runAutoStage() noexcept
{
    static std::atomic<std::uint64_t> startMs = 0;
    static std::atomic<unsigned> step = ~0u;
    const auto now = GetTickCount64();
    auto started = startMs.load(std::memory_order_relaxed);
    if (!started)
    {
        startMs.compare_exchange_strong(started, now, std::memory_order_relaxed);
        started = startMs.load(std::memory_order_relaxed);
    }
    const auto seconds = (now - started) / 1000;
    // Escalation ladder for the boundary pass. The FG input swap is never
    // enabled automatically: a reset must be attributable to one compute size.
    const unsigned target = seconds < 10 ? 0u : seconds < 40 ? 1u : 2u;
    if (step.load(std::memory_order_relaxed) == target)
        return;
    step.store(target, std::memory_order_relaxed);
    auto value = ReadControls();
    value.packedDispatch = true;
    value.packedRows = target == 0 ? 1u : target == 1 ? 240u : 1440u;
    value.packedSubstitute = false;
    WriteControls(value);
    if (auto& r = runtime(); r.log)
    {
        std::fprintf(r.log, "AUTO_STAGE step=%u rows=%u substitute=%u elapsed_s=%llu\n", target, value.packedRows,
                     value.packedSubstitute ? 1u : 0u, static_cast<unsigned long long>(seconds));
        std::fflush(r.log);
    }
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
    value.barrier = [](void*, ID3D12GraphicsCommandList*, UINT, const D3D12_RESOURCE_BARRIER*) {};
    value.beforeSubmit = [](void* p, ID3D12CommandQueue* q, UINT count, ID3D12CommandList* const* lists)
    {
        auto& r = *static_cast<Runtime*>(p);
        TimingScope timing(SubmissionTiming());
        InternalD3D12Scope ownCalls;
        {
            TimingScope capture(CaptureTiming());
            NotifyGeometryCaptureBeforeSubmit(q, count, lists);
        }
        TimingScope compose(ComposeTiming());
        // The one rule this hook follows after the 2026-09-16 08:38 freeze: it
        // never adds a wait to an engine queue. A captured producer queue can
        // be destroyed by a session rebuild, and a wait whose value that queue
        // only reaches later stops the GPU until the engine watchdog kills the
        // process. Measured in that session: the compose fence stayed at zero
        // for 140 seconds while the frame generation queue waited on it. The
        // compose is queued on the frame generation queue itself, in submission
        // order ahead of the batch that consumes it, which is the ordering
        // every earlier verified session used.
        r.each([&](Entry& e)
        {
            if (!q || !e.command)
                return;
            for (UINT i = 0; i < count; ++i)
            {
                auto* candidate = static_cast<ID3D12GraphicsCommandList*>(lists[i]);
                const bool known = e.session.handlesFgCommand(candidate);
                // The batch that carries the frame generation list is matched by
                // the session identity set. The most recent evaluation identity
                // is accepted as well, because the engine can submit a list
                // recorded in an earlier evaluation of the same frame.
                const bool current = candidate == r.activeCommand.load(std::memory_order_acquire);
                if (!known && !current)
                    continue;
                {
                    static std::atomic<unsigned> unmatched { 0 };
                    if (r.log != nullptr && !known && current && unmatched.fetch_add(1, std::memory_order_relaxed) < 4)
                    {
                        std::fprintf(r.log, "TRACE_PRERACE known=0 current=1 command=%p\n",
                                     static_cast<void*>(candidate));
                        std::fflush(r.log);
                    }
                }
                {
                    ID3D12Fence* fence = nullptr;
                    std::uint64_t value = 0;
                    void* producerQueue = nullptr;
                    const bool waited = e.session.takeProducerWait(fence, value, producerQueue);
                    bool queued = false;
                    {
                        // Driver submission of the compose: attributed
                        // separately from the rest of the compose scope so a
                        // block can be told apart from a descheduled thread.
                        TimingScope queue(ComposeQueueTiming());
                        queued = e.session.executePending(q);
                    }
                    // The marker now describes the queued compose on the GPU, not
                    // the CPU window, so it is written only when one was queued.
                    if (queued)
                        raiseComposeMarker(waited ? value : 0);
                    if (r.log != nullptr)
                    {
                        // Bounded attribution: the producer queue and its fence
                        // are read for the record only. They are never submitted
                        // to and never waited on, so a stale handle cannot stall
                        // the engine's own submission.
                        static std::atomic<unsigned> attribution { 0 };
                        if (attribution.fetch_add(1, std::memory_order_relaxed) < 8)
                        {
                            std::fprintf(r.log,
                                         "TRACE_WAIT_UNUSED waited=%u producer=%llu completed=%llu producerQueue=%p "
                                         "queue=%p submitted=%u\n",
                                         waited ? 1u : 0u, static_cast<unsigned long long>(value),
                                         static_cast<unsigned long long>(fence != nullptr ? fence->GetCompletedValue() : 0),
                                         producerQueue, static_cast<void*>(q), queued ? 1u : 0u);
                            std::fflush(r.log);
                        }
                        else if (ReadControls().trace && TraceWanted())
                        {
                            std::fprintf(r.log, "TRACE_WAIT_SKIP producer=%llu queue=%p submitted=%u\n",
                                         static_cast<unsigned long long>(value), static_cast<void*>(q),
                                         queued ? 1u : 0u);
                            std::fflush(r.log);
                        }
                    }
                    break;
                }
            }
        });
    };
    value.submit = [](void* p, ID3D12CommandQueue* q, UINT count, ID3D12CommandList* const* lists)
    {
        auto& r = *static_cast<Runtime*>(p);
        InternalD3D12Scope ownSignals;
        NotifyGeometryCaptureSubmit(q, count, lists);
        // One-time, off-thread: the packed object-motion HLSL compile measured
        // 153ms when it ran inside the frame generation callback, which is the
        // exact moment the engine rebuilds its lists (focus regain).
        WarmPackedShaderOnce();
        r.each([&](Entry& e) { e.session.afterSubmit(q, count, lists); });
        // Crash forensics: the marker describes the GPU state, not the CPU
        // submission window, so it is cleared only once the deferred compose of
        // every live entry has actually completed. A reset that frees the packed
        // outputs while the compose is still executing therefore leaves it.
        // The frame generation batch can carry any of the lists the session
        // knows (one per back buffer), so the match is by membership.
        {
            bool matched = false;
            if (auto* fg = r.activeCommand.load(std::memory_order_acquire))
                for (UINT i = 0; i < count && !matched; ++i)
                    matched = lists[i] == fg;
            if (!matched && r.active)
                for (UINT i = 0; i < count && !matched; ++i)
                    matched = r.active->session.handlesFgCommand(static_cast<ID3D12GraphicsCommandList*>(lists[i]));
            if (matched)
                {
                    bool inFlight = false;
                    r.each([&](Entry& e) { inFlight |= e.session.packedComposeInFlight(); });
                    if (!inFlight)
                        clearComposeMarker();
                }
        }
        // Attribute a driver reset to the exact submitted batch that carried
        // our substituted inputs.
        {
            bool matched = false;
            if (auto* fg = r.activeCommand.load(std::memory_order_acquire))
                for (UINT i = 0; i < count && !matched; ++i)
                    matched = lists[i] == fg;
            if (!matched && r.active)
                for (UINT i = 0; i < count && !matched; ++i)
                    matched = r.active->session.handlesFgCommand(static_cast<ID3D12GraphicsCommandList*>(lists[i]));
            if (matched)
                {
                    if (r.active)
                        r.active->session.dumpSubmitted(q);
                    if (r.log && ReadControls().trace && TraceWanted())
                    {
                        std::fprintf(r.log, "TRACE_SUBMIT fg=1 lists=%u queue=%p\n", count, q);
                        std::fflush(r.log);
                    }
                }
        }
        r.reap();
        // The live channel and the periodic log must not depend on the
        // OptiScaler overlay being rendered: this hook runs every frame.
        RefreshGeometryHealthIfNeeded(GetTickCount64());
    };
    value.signal = [](void* p, ID3D12CommandQueue* q, ID3D12Fence* fence, UINT64 number)
    {
        NotifyGeometryCaptureSignal(q, fence, number);
        static_cast<Runtime*>(p)->each([&](Entry& e) { e.session.onSignal(q, fence, number); });
    };
    value.wait = [](void* p, ID3D12CommandQueue* q, ID3D12Fence* fence, UINT64 number)
    {
        NotifyGeometryCaptureWait(q, fence, number);
        static_cast<Runtime*>(p)->each([&](Entry& e) { e.session.onWait(q, fence, number); });
    };
    return value;
}

// The state observer is a Detours transaction that suspends every thread in the
// process. Installing it inside the first frame generation evaluation put the
// whole transaction on the engine render thread at the moment the game regains
// focus (observer_ms=134.9 in the 05:46 session). Every command list of the
// same device exposes the same vtable targets, and the install path proves that
// by comparing a second list and both queue types before it attaches, so the
// transaction runs once at device creation instead and the render thread only
// re-validates the identity of the list it was handed.
std::atomic<double> observerPreinstallMs { 0.0 };

bool preinstallNativeObserver(ID3D12Device* device) noexcept
{
    try
    {
        const auto start = std::chrono::steady_clock::now();
        const bool installed = PreinstallD3D12Observer(device, makeCallbacks());
        observerPreinstallMs.store(
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count(),
            std::memory_order_relaxed);
        return installed;
    }
    catch (...)
    {
        return false;
    }
}

std::shared_ptr<Entry> acquire(Runtime& r, ID3D12GraphicsCommandList* command, const NVSDK_NGX_Handle* handle,
                               const Inputs& inputs, Controls controls)
{
    TimingScope timing(EvaluationTiming());
    r.reap();
    if (r.stopped || r.unavailable || !command || !handle || !inputs.valid())
        return {};
    if (r.active)
    {
        const bool accepting = r.active->session.accepting();
        const auto observedMask = ObservedComputeMethods();
        // The engine alternates between frame generation command lists (one per
        // back buffer) for the same inputs, so a list the session does not know
        // is added instead of retiring the session. The replacement list can
        // land on the same address as a destroyed one, so the missing identity
        // is checked as well as a changed pointer.
        if (accepting && !r.active->session.handlesFgCommand(command) &&
            r.active->session.adoptFgCommand(command, observedMask, observedMask))
        {
            r.active->command = command;
            r.activeCommand.store(command, std::memory_order_release);
        }
        const bool sameInputs =
            r.active->session.handlesFgCommand(command) && r.active->matches(command, inputs);
        if (accepting && sameInputs && r.active->handle != handle)
        {
            // The feature instance pointer is not part of the correction state:
            // the packed outputs are keyed by extent and format, and every frame
            // reads its own inputs. Streamline was observed alternating between
            // two instances for the same frame generation inputs on the same
            // command list, and treating that as a different session rebuilt the
            // packed outputs and cancelled the compose on every frame.
            static std::atomic<unsigned> adopted { 0 };
            if (r.log != nullptr && adopted.fetch_add(1, std::memory_order_relaxed) < 8)
            {
                std::fprintf(r.log, "NATIVE_HANDLE adopt=1 old=%p new=%p sameMotion=%u sameDepth=%u\n",
                             static_cast<const void*>(r.active->handle), static_cast<const void*>(handle),
                             inputs.motion == r.active->motionInput ? 1u : 0u,
                             inputs.depth == r.active->depthInput ? 1u : 0u);
                std::fflush(r.log);
            }
            r.active->handle = handle;
        }
        if (!accepting || !sameInputs || r.active->handle != handle)
        {
            // Bounded attribution for the per-frame session churn: the four
            // conditions were indistinguishable in the log, so a recreated
            // session could not be traced to its cause.
            static std::atomic<unsigned> churn { 0 };
            if (r.log != nullptr && churn.fetch_add(1, std::memory_order_relaxed) < 8)
            {
                std::fprintf(r.log, "NATIVE_CHURN accepting=%u inputs=%u handle=%u active=%p incoming=%p\n",
                             accepting ? 1u : 0u, sameInputs ? 1u : 0u,
                             r.active->handle == handle ? 1u : 0u, static_cast<const void*>(r.active->handle),
                             static_cast<const void*>(handle));
                // Which field of the incoming run differs: without it the second
                // evaluation per frame cannot be identified at all.
                const auto live = [](ID3D12Resource* value) { return value ? value->GetDesc() : D3D12_RESOURCE_DESC {}; };
                const auto motion = live(inputs.motion), depth = live(inputs.depth);
                std::fprintf(r.log,
                             "NATIVE_CHURN_IN incoming motion=%llux%u f=%u depth=%llux%u f=%u "
                             "active motion=%llux%u f=%u depth=%llux%u f=%u command=%u sameMotion=%u sameDepth=%u\n",
                             static_cast<unsigned long long>(motion.Width), motion.Height,
                             static_cast<unsigned>(motion.Format), static_cast<unsigned long long>(depth.Width),
                             depth.Height, static_cast<unsigned>(depth.Format),
                             static_cast<unsigned long long>(r.active->descriptions[0].Width),
                             r.active->descriptions[0].Height, static_cast<unsigned>(r.active->descriptions[0].Format),
                             static_cast<unsigned long long>(r.active->descriptions[2].Width),
                             r.active->descriptions[2].Height, static_cast<unsigned>(r.active->descriptions[2].Format),
                             r.active->command == command ? 1u : 0u,
                             inputs.motion == r.active->motionInput ? 1u : 0u,
                             inputs.depth == r.active->depthInput ? 1u : 0u);
                std::fflush(r.log);
            }
            if (!r.retire())
                return {};
        }
    }
    if (r.active)
        return r.active;
    if (!controls.active() || inputs.index != 1)
        return {};
    for (const auto& entry : r.retiring)
        if (entry)
            return {}; // Drain old-size recordings before reuse.
    InternalD3D12Scope ownCalls;
    if (!r.logAttempted)
    {
        r.logAttempted = true;
        const auto path = Util::DllPath().parent_path() / L"OptiScaler.Glass.log";
        // The step trace is per frame, so the file grows without bound if it is
        // only ever appended to. Rotate a bounded file once and start fresh.
        std::error_code sizeError;
        if (std::filesystem::exists(path, sizeError) && !sizeError &&
            std::filesystem::file_size(path, sizeError) > kLogLimit && !sizeError)
        {
            const auto previous = Util::DllPath().parent_path() / L"OptiScaler.Glass.previous.log";
            std::error_code rotateError;
            std::filesystem::remove(previous, rotateError);
            std::filesystem::rename(path, previous, rotateError);
        }
        r.log = _wfopen(path.c_str(), L"a");
        if (!r.log)
            r.unavailable = true;
    }
    if (r.unavailable)
        return {};
    // Session creation runs on the engine's render thread when the frame
    // generation lists are rebuilt, which is the focus-regain path. The three
    // phases are timed apart so a remaining hitch can be attributed instead of
    // guessed: observer install, packed capture construction, session GPU state.
    const auto observerStart = std::chrono::steady_clock::now();
    const bool observerInstalled = InstallD3D12Observer(command, makeCallbacks());
    const auto observerMs =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - observerStart).count();
    if (!observerInstalled)
    {
        r.unavailable = true;
        std::fprintf(r.log, "NATIVE_HOST ready=0 reason=observer_coverage mismatch_slot=%d\n",
                     MismatchD3D12ObserverSlot(command));
        std::fflush(r.log);
        return {};
    }
    r.submissionObserved.store(true, std::memory_order_release);
    auto entry = std::make_shared<Entry>();
    entry->handle = handle;
    entry->command = command;
    entry->motionInput = inputs.motion;
    entry->depthInput = inputs.depth;
    entry->descriptions[0] = inputs.motion->GetDesc();
    entry->descriptions[1] = inputs.color->GetDesc();
    entry->descriptions[2] = inputs.depth->GetDesc();
    Microsoft::WRL::ComPtr<ID3D12Device> device;
    if (FAILED(command->GetDevice(IID_PPV_ARGS(&device))))
        return {};
    const auto width = static_cast<std::uint32_t>(entry->descriptions[0].Width);
    const auto height = entry->descriptions[0].Height;
    const auto captureStart = std::chrono::steady_clock::now();
    const bool captureReady =
        InitializePackedMotionCapture(device.Get(), width, height, r.log, MakeGlassMotionIdentityProvider());
    const auto captureMs =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - captureStart).count();
    if (!captureReady)
    {
        // A different extent means the frame-generation inputs were rebuilt at
        // a new size (resolution or DLSS quality change). The capture is sized
        // at runtime from those inputs, so it has to be rebuilt; defer that to
        // the health thread, which can prove no session still references it.
        const auto current = ReadPackedMotionCaptureStatus();
        if (current.initialized && (current.width != width || current.height != height))
        {
            r.rebuildWidth = width;
            r.rebuildHeight = height;
            r.rebuildPending = true;
            r.rebuildAttempts = 0;
            r.rebuildDevice = device;
            if (r.log)
            {
                std::fprintf(r.log, "PACKED_CAPTURE extent_change requested=%ux%u current=%ux%u deferred=1\n",
                             width, height, current.width, current.height);
                std::fflush(r.log);
            }
        }
        return {};
    }
    const auto shaders = Util::DllPath().parent_path() / L"Glass";
    const auto sessionStart = std::chrono::steady_clock::now();
    if (!entry->session.initializePacked(device.Get(), entry->descriptions,
                                         (shaders / L"GlassObjectMotion.hlsl").c_str(), r.log,
                                         { AcquirePackedMotionFrame, DiscardPackedMotionRecording }))
    {
        r.unavailable = true;
        return {};
    }
    const auto sessionMs =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - sessionStart).count();
    const auto mask = ObservedComputeMethods();
    if (!entry->session.bindFgCommand(command, mask, mask))
    {
        entry->session.releaseAfterGpuDrain(); // Initialization recorded nothing.
        r.unavailable = true;
        return {};
    }
    r.active = entry;
    r.activeCommand.store(command, std::memory_order_release);
    std::fprintf(r.log,
                 "NATIVE_HOST ready=1 handle=%p command=%p methods=%x width=%llu height=%u observer_ms=%.1f "
                 "capture_ms=%.1f session_ms=%.1f preinstall_ms=%.1f\n",
                 handle, command, mask, entry->descriptions[0].Width, entry->descriptions[0].Height, observerMs,
                 captureMs, sessionMs, observerPreinstallMs.load(std::memory_order_relaxed));
    std::fflush(r.log);
    return entry;
}
} // namespace

bool PreinstallNativeObserver(ID3D12Device* device) noexcept
{
    return preinstallNativeObserver(device);
}

bool NativeCaptureSubmissionReady() noexcept
{
    return runtime().submissionObserved.load(std::memory_order_acquire);
}

void RequestNativeSoftReload() noexcept
{
    softReloadRequested.store(true, std::memory_order_release);
}

void RequestPackedDump() noexcept
{
    dumpRequested.store(true, std::memory_order_release);
}

bool CorrectStreamlineFrame(ID3D12GraphicsCommandList* command, const void* featureKey,
                            const StreamlineFrame& frame) noexcept
{
    try
    {
        auto& r = runtime();
        auto controls = ReadControls();
        if (!controls.active() || !command || !featureKey)
            return false;

        // Bounded diagnostics: a silent no-op path has to be visible without
        // letting the log grow without bound.
        static std::atomic<unsigned> calls { 0 };
        const auto call = calls.fetch_add(1, std::memory_order_relaxed);
        const bool report = call < 3 || call % 600 == 0;

        Inputs inputs;
        inputs.motion = frame.motion;
        inputs.depth = frame.depth;
        inputs.color = frame.color;
        inputs.index = frame.index;
        inputs.count = frame.count;
        inputs.reset = frame.reset;
        inputs.scaleX = frame.scaleX;
        inputs.scaleY = frame.scaleY;
        inputs.jitterX = frame.jitterX;
        inputs.jitterY = frame.jitterY;
        inputs.clipToPrevious = frame.clipToPrevious;
        inputs.frame = frame.frame;
        const bool valid = inputs.valid();

        if (report)
        {
            std::lock_guard lock(r.mutex);
            if (!r.logAttempted)
            {
                r.logAttempted = true;
                r.log = _wfopen((Util::DllPath().parent_path() / L"OptiScaler.Glass.log").c_str(), L"a");
            }
            if (r.log)
            {
                std::fprintf(r.log,
                             "SL_FRAME call=%u motion=%p depth=%p color=%p scale=%.4f,%.4f frame=%llu valid=%u\n",
                             call + 1, static_cast<void*>(frame.motion), static_cast<void*>(frame.depth),
                             static_cast<void*>(frame.color), frame.scaleX, frame.scaleY,
                             static_cast<unsigned long long>(frame.frame), valid ? 1u : 0u);
                std::fflush(r.log);
            }
        }
        if (!valid)
            return false;

        // Delivery reuses the engine-input write-back path: the composed motion
        // and depth are copied into the game's own textures, so the frame
        // generation provider keeps its resources and the unlocker is untouched.
        controls.packedSubstitute = false;
        controls.packedWriteBack = true;
        std::lock_guard lock(r.mutex);
        auto entry = acquire(r, command, reinterpret_cast<const NVSDK_NGX_Handle*>(featureKey), inputs, controls);
        if (!entry)
            return false;
        const D3D12_RESOURCE_STATES states[3] { frame.motionState, D3D12_RESOURCE_STATE_COMMON, frame.depthState };
        entry->session.prepare(command, inputs, states, controls, true);
        StreamlineFrameCounter().fetch_add(1, std::memory_order_relaxed);
        return true;
    }
    catch (...)
    {
        return false;
    }
}

void NoteStreamlineFeature(unsigned id) noexcept
{
    try
    {
        static std::atomic<unsigned> seen[32] {};
        static std::atomic<unsigned> logged { 0 };
        const auto slot = id % 32u;
        const auto bit = 1u << (id % 31u);
        if ((seen[slot].fetch_or(bit, std::memory_order_relaxed) & bit) != 0)
            return;
        if (logged.fetch_add(1, std::memory_order_relaxed) >= 8)
            return;
        auto& r = runtime();
        std::lock_guard lock(r.mutex);
        if (!r.logAttempted)
        {
            r.log = _wfopen((Util::DllPath().parent_path() / L"OptiScaler.Glass.log").c_str(), L"a");
            // Only a successful open may close the door: a failed one has to
            // stay retryable, otherwise the whole session stops logging.
            r.logAttempted = r.log != nullptr;
        }
        if (r.log)
        {
            std::fprintf(r.log, "SL_FEATURE id=%u\n", id);
            std::fflush(r.log);
        }
    }
    catch (...)
    {
    }
}

void NoteNgxFeature(unsigned feature, unsigned handleId, const char* provider) noexcept
{
    try
    {
        struct Pair
        {
            unsigned feature = 0;
            unsigned handle = 0;
        };
        static Pair seen[16] {};
        static std::atomic<unsigned> seenCount { 0 };
        static std::atomic<unsigned> logged { 0 };
        const auto count = (std::min)(seenCount.load(std::memory_order_relaxed), 16u);
        for (unsigned i = 0; i < count; ++i)
            if (seen[i].feature == feature && seen[i].handle == handleId)
                return;
        if (logged.fetch_add(1, std::memory_order_relaxed) >= 12)
            return;
        if (count < 16)
        {
            seen[count] = { feature, handleId };
            seenCount.store(count + 1, std::memory_order_relaxed);
        }
        auto& r = runtime();
        std::lock_guard lock(r.mutex);
        if (!r.logAttempted)
        {
            r.logAttempted = true;
            r.log = _wfopen((Util::DllPath().parent_path() / L"OptiScaler.Glass.log").c_str(), L"a");
        }
        if (r.log)
        {
            std::fprintf(r.log, "NGX_FEATURE feature=%u handleId=%u fgConstant=%u provider=%s\n", feature, handleId,
                         static_cast<unsigned>(NVSDK_NGX_Feature_FrameGeneration), provider ? provider : "-");
            std::fflush(r.log);
        }
    }
    catch (...)
    {
    }
}

void NoteNgxCreate(unsigned feature, unsigned handleId, const char* route) noexcept
{
    try
    {
        struct Pair
        {
            unsigned feature = 0;
            unsigned handle = 0;
        };
        static Pair seen[16] {};
        static std::atomic<unsigned> seenCount { 0 };
        static std::atomic<unsigned> logged { 0 };
        const auto count = (std::min)(seenCount.load(std::memory_order_relaxed), 16u);
        for (unsigned i = 0; i < count; ++i)
            if (seen[i].feature == feature && seen[i].handle == handleId)
                return;
        if (logged.fetch_add(1, std::memory_order_relaxed) >= 16)
            return;
        if (count < 16)
        {
            seen[count] = { feature, handleId };
            seenCount.store(count + 1, std::memory_order_relaxed);
        }
        auto& r = runtime();
        std::lock_guard lock(r.mutex);
        if (!r.logAttempted)
        {
            r.logAttempted = true;
            r.log = _wfopen((Util::DllPath().parent_path() / L"OptiScaler.Glass.log").c_str(), L"a");
        }
        if (r.log)
        {
            std::fprintf(r.log, "NGX_CREATE feature=%u handleId=%u route=%s\n", feature, handleId, route ? route : "-");
            std::fflush(r.log);
        }
    }
    catch (...)
    {
    }
}

void NoteNvngxLoad(const wchar_t* name, bool redirect) noexcept
{
    try
    {
        static std::atomic<unsigned> logged { 0 };
        if (logged.fetch_add(1, std::memory_order_relaxed) >= 16)
            return;
        auto& r = runtime();
        std::lock_guard lock(r.mutex);
        if (!r.logAttempted)
        {
            r.logAttempted = true;
            r.log = _wfopen((Util::DllPath().parent_path() / L"OptiScaler.Glass.log").c_str(), L"a");
        }
        if (r.log)
        {
            std::fprintf(r.log, "GLASS_NVNGX_LOAD name=%ls redirect=%u\n", name ? name : L"-", redirect ? 1u : 0u);
            std::fflush(r.log);
        }
    }
    catch (...)
    {
    }
}

void NoteHookStage(const wchar_t* name, unsigned stage) noexcept
{
    try
    {
        static std::atomic<unsigned> logged { 0 };
        if (logged.fetch_add(1, std::memory_order_relaxed) >= 8)
            return;
        auto& r = runtime();
        std::lock_guard lock(r.mutex);
        if (!r.logAttempted)
        {
            r.log = _wfopen((Util::DllPath().parent_path() / L"OptiScaler.Glass.log").c_str(), L"a");
            r.logAttempted = r.log != nullptr;
        }
        if (r.log)
        {
            std::fprintf(r.log, "GLASS_NGX_HOOK module=%ls stage=%u\n", name ? name : L"-", stage);
            std::fflush(r.log);
        }
    }
    catch (...)
    {
    }
}

LONG WINAPI glassUnhandledFilter(EXCEPTION_POINTERS* info) noexcept
{
    try
    {
        auto& r = runtime();
        std::lock_guard lock(r.mutex);
        if (!r.logAttempted)
        {
            r.logAttempted = true;
            r.log = _wfopen((Util::DllPath().parent_path() / L"OptiScaler.Glass.log").c_str(), L"a");
        }
        if (r.log)
        {
            const auto* record = info != nullptr ? info->ExceptionRecord : nullptr;
            std::fprintf(r.log, "GLASS_EXCEPTION code=0x%08lX address=%p\n",
                         record != nullptr ? static_cast<unsigned long>(record->ExceptionCode) : 0ul,
                         record != nullptr ? static_cast<void*>(record->ExceptionAddress) : nullptr);
            std::fflush(r.log);
        }
    }
    catch (...)
    {
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

LONG CALLBACK glassVectoredHandler(EXCEPTION_POINTERS* info) noexcept
{
    try
    {
        const auto* record = info != nullptr ? info->ExceptionRecord : nullptr;
        if (record == nullptr)
            return EXCEPTION_CONTINUE_SEARCH;
        const auto code = static_cast<unsigned long>(record->ExceptionCode);
        // Only fault-class exceptions: first-chance C++/DRM noise is not useful
        // and must not fill the log.
        if (code != 0xC0000005ul && code != 0xC000001Dul && code != 0xC0000094ul && code != 0xC0000096ul &&
            code != 0xC00000FDul && code != 0xC0000409ul && code != 0x80000003ul)
            return EXCEPTION_CONTINUE_SEARCH;
        static std::atomic<unsigned> faults { 0 };
        if (faults.fetch_add(1, std::memory_order_relaxed) >= 8)
            return EXCEPTION_CONTINUE_SEARCH;
        wchar_t moduleName[MAX_PATH] {};
        MEMORY_BASIC_INFORMATION memory {};
        const auto* address = static_cast<const unsigned char*>(record->ExceptionAddress);
        const auto* base = static_cast<const unsigned char*>(nullptr);
        if (record->ExceptionAddress != nullptr &&
            VirtualQuery(record->ExceptionAddress, &memory, sizeof(memory)) == sizeof(memory) &&
            memory.AllocationBase != nullptr)
        {
            GetModuleFileNameW(static_cast<HMODULE>(memory.AllocationBase), moduleName, MAX_PATH);
            base = static_cast<const unsigned char*>(memory.AllocationBase);
        }
        auto& r = runtime();
        std::lock_guard lock(r.mutex);
        if (!r.logAttempted)
        {
            r.log = _wfopen((Util::DllPath().parent_path() / L"OptiScaler.Glass.log").c_str(), L"a");
            r.logAttempted = r.log != nullptr;
        }
        if (r.log)
        {
            std::fprintf(r.log, "GLASS_FAULT code=0x%08lX address=%p base=%p offset=0x%llX module=%ls\n", code,
                         static_cast<const void*>(address), static_cast<const void*>(base),
                         static_cast<unsigned long long>(address != nullptr && base != nullptr ? address - base : 0),
                         moduleName[0] != 0 ? moduleName : L"-");
            std::fflush(r.log);
        }
    }
    catch (...)
    {
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

void InstallProcessDiagnostics() noexcept
{
    // The engine installs its own unhandled filter later, which replaced this
    // one; a vectored handler is not replaced and still sees the fatal fault.
    AddVectoredExceptionHandler(1, &glassVectoredHandler);
    SetUnhandledExceptionFilter(&glassUnhandledFilter);
}

void NoteProcessAttach() noexcept
{
    // No file work from the loader lock: the module path resolved there lands
    // outside the overlay the rest of the session logs into, and RootBuilder
    // deletes that copy when the game exits. The marker is written by the first
    // health-service tick instead, into the same log as every other line.
    attachPending.store(true, std::memory_order_release);
}

void NoteProcessDetach() noexcept
{
    try
    {
        auto& r = runtime();
        // Never open a handle from the loader lock: only report through a log
        // that a running session already opened.
        if (!r.log)
            return;
        std::lock_guard lock(r.mutex);
        if (r.log)
        {
            std::fprintf(r.log, "GLASS_PROCESS detach=1\n");
            std::fflush(r.log);
        }
    }
    catch (...)
    {
    }
}


void ServiceNativeDiagnostics() noexcept
{
    // Outside the runtime lock on purpose: installing a detour suspends the
    // process briefly, and a suspended thread must not be holding a lock this
    // call would need.
    InstallLoadedNgxHooks();
    auto& r = runtime();
    std::lock_guard lock(r.mutex);
    if (attachPending.exchange(false, std::memory_order_acq_rel))
    {
        if (!r.logAttempted)
        {
            r.log = _wfopen((Util::DllPath().parent_path() / L"OptiScaler.Glass.log").c_str(), L"a");
            r.logAttempted = r.log != nullptr;
        }
        if (r.log)
        {
            std::fprintf(r.log, "GLASS_PROCESS attach=1 pid=%lu\n", GetCurrentProcessId());
            std::fflush(r.log);
        }
    }
    r.each([&](Entry& e) { e.session.serviceDump(); });
}

NativeHostStatus ReadNativeHostStatus() noexcept
{
    auto& r = runtime();
    std::lock_guard lock(r.mutex);
    NativeHostStatus status;
    status.evaluations = r.evaluations;
    status.substitutions = r.substitutions;
    status.captures = r.captures;
    for (unsigned i = 0; i < 8; ++i)
    {
        status.evaluationsByIndex[i] = r.evaluationsByIndex[i];
        status.substitutionsByIndex[i] = r.substitutionsByIndex[i];
    }
    status.unsubstitutedReusedMotion = r.unsubstitutedReusedMotion;
    status.unsubstitutedFreshMotion = r.unsubstitutedFreshMotion;
    status.active = r.active ? 1u : 0u;
    status.retiring = (r.retiring[0] ? 1u : 0u) + (r.retiring[1] ? 1u : 0u);
    status.stopped = r.stopped ? 1u : 0u;
    status.unavailable = r.unavailable ? 1u : 0u;
    return status;
}

void ReportNativeHostLog() noexcept
{
    try
    {
        // Snapshot under the runtime lock, format outside it: the render thread
        // must never wait for the log. The old call site held the lock across the
        // whole report, which is why a busy file system stalled the render thread.
        const auto status = ReadNativeHostStatus();
        auto& r = runtime();
        unsigned lastResult = 0;
        FILE* log = nullptr;
        {
            std::lock_guard lock(r.mutex);
            lastResult = r.lastResult;
            log = r.log;
        }
        if (log == nullptr)
            return;
        std::fprintf(log, "NATIVE_HOST evaluations=%llu substitutions=%llu captures=%llu result=%x\n",
                     static_cast<unsigned long long>(status.evaluations),
                     static_cast<unsigned long long>(status.substitutions),
                     static_cast<unsigned long long>(status.captures), lastResult);
        std::fprintf(log, "NATIVE_HOST_BY_INDEX");
        for (unsigned i = 0; i < 8; ++i)
            if (status.evaluationsByIndex[i] != 0)
                std::fprintf(log, " %u:%llu/%llu", i,
                             static_cast<unsigned long long>(status.substitutionsByIndex[i]),
                             static_cast<unsigned long long>(status.evaluationsByIndex[i]));
        std::fprintf(log, " (substituted/evaluated) skipped_reused=%llu skipped_uncorrected=%llu\n",
                     static_cast<unsigned long long>(status.unsubstitutedReusedMotion),
                     static_cast<unsigned long long>(status.unsubstitutedFreshMotion));
        ReportGeometryHost(log);
        std::fflush(log);
    }
    catch (...)
    {
    }
}

void WarmPackedShaderOnce() noexcept
{
    try
    {
        static std::once_flag once;
        std::call_once(once, []
        {
            std::thread([] {
                try
                {
                    auto& r = runtime();
                    FILE* log = nullptr;
                    {
                        std::lock_guard lock(r.mutex);
                        log = r.log;
                    }
                    const bool warmed = PackedMotionGpu::warmShaderCode(packedShaderPath().c_str(), log);
                    if (warmed && log != nullptr)
                    {
                        std::fprintf(log, "OBJECT_SHADER warmed=1\n");
                        std::fflush(log);
                    }
                }
                catch (...)
                {
                }
            }).detach();
        });
    }
    catch (...)
    {
    }
}

NVSDK_NGX_Result EvaluateNativeFG(ID3D12GraphicsCommandList* command, const NVSDK_NGX_Handle* handle,
                                  NVSDK_NGX_Parameter* parameters, PFN_NVSDK_NGX_ProgressCallback callback,
                                  NativeEvaluate original)
{
    if (!original)
        return NVSDK_NGX_Result_FAIL_FeatureNotFound;
    auto& r = runtime();
    // Hard log cap, applied from this thread so the handle is never closed while
    // a hook on another thread writes to it.
    if (r.log != nullptr && r.evaluations != 0 && (r.evaluations % 900) == 0)
    {
        const auto path = Util::DllPath().parent_path() / L"OptiScaler.Glass.log";
        std::error_code sizeError;
        const auto size = std::filesystem::file_size(path, sizeError);
        if (!sizeError && size > kLogLimit)
        {
            std::fflush(r.log);
            std::fclose(r.log);
            const auto previous = Util::DllPath().parent_path() / L"OptiScaler.Glass.previous.log";
            std::error_code rotateError;
            std::filesystem::remove(previous, rotateError);
            std::filesystem::rename(path, previous, rotateError);
            r.log = _wfopen(path.c_str(), L"a");
            if (r.log != nullptr)
            {
                std::fprintf(r.log, "GLASS_LOG rotated=1 size_bytes=%llu\n",
                             static_cast<unsigned long long>(size));
                std::fflush(r.log);
            }
        }
    }
    // File-driven live controls. Both run on this thread because it owns the
    // native recordings; the request file itself is polled elsewhere.
    if (softReloadRequested.exchange(false, std::memory_order_acq_rel))
    {
        std::lock_guard lock(r.mutex);
        if (r.retire())
        {
            r.reap();
            ResetPackedMotionCounters();
            if (r.log)
            {
                std::fprintf(r.log, "NATIVE_HOST soft_reload=1 retired=1\n");
                std::fflush(r.log);
            }
        }
        else
            softReloadRequested.store(true, std::memory_order_release); // Retry after a drain.
    }
    if (dumpRequested.exchange(false, std::memory_order_acq_rel))
    {
        std::lock_guard lock(r.mutex);
        // The session is created and retired inside each evaluation, so the
        // health thread rarely sees an active entry. A retiring entry still owns
        // its packed outputs until the compose fence completes, and the dump
        // service below runs for every entry, so target either.
        Entry* target = r.active.get();
        if (!target)
            for (auto& entry : r.retiring)
                if (entry)
                    target = entry.get();
        if (target)
        {
            dumpRetries.store(0, std::memory_order_relaxed);
            target->session.requestDump();
        }
        else
        {
            // The session is created later in this same evaluation call, so a
            // request that lands between frames has to survive until one is
            // active instead of being dropped.
            const auto attempt = dumpRetries.fetch_add(1, std::memory_order_relaxed);
            if (attempt < 240)
                dumpRequested.store(true, std::memory_order_release);
            if (r.log && (attempt < 3 || attempt == 240u))
            {
                std::fprintf(r.log, "PACKED_DUMP %s\n", attempt < 240 ? "requeued" : "skipped reason=no_active_entry");
                std::fflush(r.log);
            }
        }
    }
    if (TakeShaderReloadRequest())
    {
        std::lock_guard lock(r.mutex);
        const auto shader = packedShaderPath();
        bool reloaded = false;
        if (r.active && r.log)
            reloaded = r.active->session.reloadPackedShader(shader.c_str(), r.log);
        if (r.log)
        {
            std::fprintf(r.log, "NATIVE_HOST shader_reload=%u path=%ls\n", reloaded ? 1u : 0u, shader.c_str());
            std::fflush(r.log);
        }
    }
    static std::atomic<std::uint64_t> tagHits = 0, tagMisses = 0;
    Inputs inputs;
    std::shared_ptr<Entry> entry;
    PreparedInputs prepared;
    auto controls = ReadControls();
    if (controls.autoStage)
    {
        runAutoStage();
        controls = ReadControls();
    }
    if (Inputs::read(parameters, inputs))
    {
        if (inputs.motionKey != nullptr && std::strcmp(inputs.motionKey, "DLSSG.MVecs") != 0)
        {
            // The driver-level block (Streamline's frame generation plugin
            // through _nvngx.dll) names only the two textures. The motion-vector
            // scale lives in the Streamline constants this module already
            // tracks, so take it from there instead of assuming 1.
            const auto scale = StreamlineHooks::GlassMvecScale();
            if (std::isfinite(scale.x) && std::isfinite(scale.y) && scale.x > 0.f && scale.y > 0.f)
            {
                inputs.scaleX = scale.x;
                inputs.scaleY = scale.y;
            }
        }
        std::lock_guard lock(r.mutex);
        entry = acquire(r, command, handle, inputs, controls);
        if (!entry)
            PublishRuntimeStatus(r.stopped                                    ? RuntimeStatus::Stopped
                                 : r.unavailable                              ? RuntimeStatus::Unavailable
                                 : r.active || r.retiring[0] || r.retiring[1] ? RuntimeStatus::Retiring
                                                                              : RuntimeStatus::Waiting);
        if (entry)
        {
            static std::atomic<unsigned> flowLogged { 0 };
            if (r.log && flowLogged.fetch_add(1, std::memory_order_relaxed) < 4)
            {
                std::fprintf(r.log, "GLASS_FLOW host_evaluate index=%u driverKeys=%u\n", inputs.index,
                             inputs.motionKey != nullptr && std::strcmp(inputs.motionKey, "DLSSG.MVecs") != 0 ? 1u
                                                                                                             : 0u);
                std::fflush(r.log);
            }
            ++entry->evaluations;
            if (inputs.index < 8)
                ++r.evaluationsByIndex[inputs.index];
            D3D12_RESOURCE_STATES states[3] {};
            ID3D12Resource* resources[] = { inputs.motion, inputs.color, inputs.depth };
            InternalD3D12Scope ownCalls;
            StreamlineInputFrame frame;
            if (ReadStreamlineStates(handle, inputs.index, inputs.count, resources, states, &frame))
            {
                inputs.frame = frame.frame;
                if (r.log && inputs.index == 1 && !(++tagHits % 300))
                    std::fprintf(r.log, "NATIVE_TAG ok hits=%llu frame=%llu\n",
                                 static_cast<unsigned long long>(tagHits.load()),
                                 static_cast<unsigned long long>(frame.frame));
                prepared = entry->session.prepare(command, inputs, states, controls);
            }
            else
            {
                if (r.log && inputs.index == 1 && !(++tagMisses % 300))
                    std::fprintf(r.log, "NATIVE_TAG miss count=%llu\n",
                                 static_cast<unsigned long long>(tagMisses.load()));
                // Driver-level block: Streamline's frame generation plugin hands
                // the two textures straight to the NGX core, so no tag state and
                // no Streamline frame token exist. The DLSS-G input convention
                // delivers both in COPY_DEST. This is the block that reads the
                // parameter table (the tagged engine evaluations read nothing),
                // so the correction has to be prepared here as well.
                if (inputs.motionKey != nullptr)
                {
                    states[0] = D3D12_RESOURCE_STATE_COPY_DEST;
                    states[2] = D3D12_RESOURCE_STATE_COPY_DEST;
                    if (std::strcmp(inputs.motionKey, "DLSSG.MVecs") != 0)
                    {
                        // The packed capture numbers its frames with the engine's
                        // render frame (the draw packets), so the driver path has to
                        // use that same counter. A private sequence would never match
                        // and the capture would report acquire_no_candidate.
                        const auto commandsFrame = GetGeometryCommandStats().lastFrame;
                        const auto healthFrame = ReadGeometryHealth().frame;
                        const auto engineFrame = commandsFrame != 0 ? commandsFrame : healthFrame;
                        inputs.frame = engineFrame != 0 ? engineFrame : ++driverFrameCounter;
                        static std::atomic<unsigned> driverPrepared { 0 };
                        if (r.log && driverPrepared.fetch_add(1, std::memory_order_relaxed) < 4)
                        {
                            std::fprintf(r.log, "GLASS_FLOW driver_prepare frame=%llu\n",
                                         static_cast<unsigned long long>(inputs.frame));
                            std::fprintf(r.log, "GLASS_FLOW session_log=%p host_log=%p initialized=%u\n",
                                         static_cast<void*>(entry->session.logFileHandle()), static_cast<void*>(r.log),
                                         entry->session.initializedForDiagnostics() ? 1u : 0u);
                            std::fflush(r.log);
                        }
                    }
                    else
                    {
                        // The table carries both naming conventions and the
                        // provider reads its own: the parameter trace shows
                        // MotionVectors/Depth on exactly these token-less
                        // evaluations. Replacing DLSSG.MVecs only would leave the
                        // key the provider reads pointing at the engine texture.
                        inputs.motionKey = "MotionVectors";
                        inputs.depthKey = "Depth";
                    }
                    prepared = entry->session.prepare(command, inputs, states, controls, true);
                }
                else
                    entry->session.bypass(inputs.index);
            }
        }
    }
    else
    {
        std::lock_guard lock(r.mutex);
        // Bounded diagnostic: which keys the provider did not supply. Without it
        // a rejected frame leaves no trace at all.
        static std::atomic<unsigned> readMisses { 0 };
        if (readMisses.fetch_add(1, std::memory_order_relaxed) < 3)
        {
            if (!r.logAttempted)
            {
                r.logAttempted = true;
                r.log = _wfopen((Util::DllPath().parent_path() / L"OptiScaler.Glass.log").c_str(), L"a");
            }
            // The driver-level evaluate does not use the game's parameter names,
            // so the probe also reports which candidate keys are present at all.
            const char* keys[] { "DLSSG.MVecs", "DLSSG.HUDLess", "DLSSG.Depth", "DLSSG.MultiFrameIndex",
                                 "DLSSG.MultiFrameCount", "DLSSG.Reset", "DLSSG.ClipToPrevClip" };
            const char* probes[] { "MVecs", "MotionVectors", "DLSSG.MotionVectors", "Depth", "DLSSG.DepthInverted",
                                   "HudlessColor", "DLSSG.Color", "DLSSG.HudlessColor", "Width", "Height",
                                   "DLSS.Feature.Create.Flags", "DLSSG.OpticalFlowEnabled", "DLSSG.CameraNear",
                                   "DLSSG.CameraFar", "DLSSG.JitterOffset", "DLSSG.MVecScale" };
            if (r.log)
            {
                std::fprintf(r.log, "GLASS_READFAIL missing=");
                for (const auto* key : keys)
                {
                    void* value = nullptr;
                    if (parameters == nullptr || parameters->Get(key, &value) != 1)
                        std::fprintf(r.log, " %s", key);
                }
                std::fprintf(r.log, "\n");
                std::fprintf(r.log, "GLASS_READPROBE present=");
                for (const auto* key : probes)
                {
                    void* value = nullptr;
                    if (parameters != nullptr && parameters->Get(key, &value) == 1)
                        std::fprintf(r.log, " %s", key);
                }
                std::fprintf(r.log, "\n");
                std::fflush(r.log);
            }
        }
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
        // Live diagnostic: hand the provider a forwarding wrapper so the keys it
        // actually reads (and whether the motion/depth pointers it gets are the
        // substituted textures) are recorded instead of assumed. Only while the
        // trace switch is on; the normal path passes the original pointer.
        if (controls.trace || controls.packedSupply)
        {
            auto& probe = NgxParameterProbeInstance();
            probe.bind(parameters, r.log, inputs.index, inputs.count, inputs.frame, prepared.motion, prepared.depth,
                       inputs.motion, inputs.depth);
            if (controls.packedSupply)
                probe.supply(prepared.motion, prepared.depth, inputs.motionKey, inputs.depthKey,
                             prepared.layerMvecs, prepared.layerOpacity);
            else
                probe.supply(nullptr, nullptr, nullptr, nullptr);
            static std::atomic<unsigned> probeBinds { 0 };
            if (r.log != nullptr && probeBinds.fetch_add(1, std::memory_order_relaxed) < 6)
            {
                wchar_t providerPath[MAX_PATH] {};
                HMODULE owner = nullptr;
                if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                           GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                       reinterpret_cast<LPCWSTR>(original), &owner) &&
                    owner != nullptr)
                    GetModuleFileNameW(owner, providerPath, MAX_PATH);
                std::fprintf(r.log, "GLASS_PARAM_BIND provider=%ls function=%p applied=%u supply=%u index=%u frame=%llu\n",
                             providerPath, reinterpret_cast<void*>(original), applied ? 1u : 0u,
                             controls.packedSupply ? 1u : 0u, inputs.index,
                             static_cast<unsigned long long>(inputs.frame));
                std::fflush(r.log);
            }
            result = original(command, handle, &probe, callback);
            static std::atomic<unsigned> probeCalls { 0 };
            if (controls.trace && probeCalls.fetch_add(1, std::memory_order_relaxed) < 60)
                probe.finish(r.log);
            if (controls.trace && r.evaluations % 600 == 0)
                probe.report(r.log);
        }
        else
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
        r.lastResult = static_cast<unsigned>(result);
        r.substitutions += applied;
        if (applied && inputs.index < 8)
            ++r.substitutionsByIndex[inputs.index];
        if (applied)
        {
            r.correctedMotion[r.correctedMotionNext++ & 3] = inputs.motion;
        }
        else
        {
            bool reused = false;
            for (auto* pointer : r.correctedMotion)
                if (pointer != nullptr && pointer == inputs.motion)
                {
                    reused = true;
                    break;
                }
            if (reused)
                ++r.unsubstitutedReusedMotion;
            else
                ++r.unsubstitutedFreshMotion;
        }
        if (applied && result == NVSDK_NGX_Result_Success)
        {
            const auto now = GetTickCount64();
            GeometryTelemetry::counts[GeometryFgReplacements].fetch_add(1, std::memory_order_relaxed);
            GeometryTelemetry::changedMs[GeometryFgReplacements].store(now, std::memory_order_relaxed);
            GeometryTelemetry::fgMs.store(now, std::memory_order_relaxed);
        }
        PublishRuntimeStatus(applied && result == NVSDK_NGX_Result_Success ? RuntimeStatus::Correcting
                                                                           : RuntimeStatus::Waiting);
        if (result != NVSDK_NGX_Result_Success || (prepared.motion && !applied))
            entry->session.nativeFailure();
        if (auto timing = entry->session.pollTiming())
            PublishGpuMilliseconds(timing->milliseconds);
        // Live panel state: the swap counters plus the compose fence pair that
        // proves our own GPU work is not outliving the session.
        PublishLiveStatus(LiveStatusEvaluations, r.evaluations);
        PublishLiveStatus(LiveStatusSubstitutions, r.substitutions);
        PublishLiveStatus(LiveStatusComposeSubmitted, r.activeComposeSubmitted());
        PublishLiveStatus(LiveStatusComposeCompleted, r.activeComposeCompleted());
        PublishLiveStatus(LiveStatusComposeForced, r.activeComposeForced());
        PublishLiveStatus(LiveStatusComposeInFlight, r.anyComposeInFlight() ? 1u : 0u);
        PublishLiveStatus(LiveStatusUnavailable, r.unavailable ? 1u : 0u);
        PublishLiveStatus(LiveStatusRetiring, (r.retiring[0] ? 1u : 0u) + (r.retiring[1] ? 1u : 0u));
        // The periodic report is formatted by the host's background thread
        // (ReportNativeHostLog). It used to run here, inside the frame generation
        // callback, which is the engine's render thread: ~30 formatted lines plus
        // a flush while holding the runtime mutex.
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
    if (r.log)
    {
        std::fprintf(r.log, "NATIVE_HOST created_native_fg=1\n");
        std::fflush(r.log);
    }
    // Only the host's successful native FG creation reopens admission after
    // shutdown. Retiring recordings still prevent allocation until drained.
    r.stopped = false;
    r.reap();
}
} // namespace GlassFg
