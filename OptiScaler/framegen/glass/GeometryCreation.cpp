#include "pch.h"
#include "GeometryCreation.h"
#include "GeometryObservationCache.h"
#include "GeometryPipelineStream.h"
#include "ExperimentPipelineService.h"
#include "DetourThreads.h"
#include <hooks/Hook_Utils.h>
#include <atomic>
#include <mutex>
#include <vector>

namespace GlassFg
{
namespace
{
using Microsoft::WRL::ComPtr;
using GraphicsCreate = rewrite_signature<decltype(&ID3D12Device::CreateGraphicsPipelineState)>::type;
using StreamCreate = rewrite_signature<decltype(&ID3D12Device2::CreatePipelineState)>::type;
GraphicsCreate originalGraphics = nullptr;
StreamCreate originalStream = nullptr;
thread_local unsigned creationDepth = 0;
struct Capture
{
    ComPtr<ID3D12Device> device;
    ComPtr<ID3D12Device2> device2;
    GeometryPipelineCache cache;
    GeometryObservationCache observations;
    std::atomic<std::uint64_t> roots = 0, graphics = 0, streams = 0;
    std::atomic<std::uint64_t> streamGraphics = 0, streamNonGraphics = 0, streamRejected = 0;
    Capture(ID3D12Device* d, const std::filesystem::path& path, GeometryCacheLimits limits)
        : device(d), cache(d, path, limits), observations(2048, 32 * 1024 * 1024, true)
    {
        d->QueryInterface(IID_PPV_ARGS(&device2));
    }
};
struct Control
{
    std::mutex lifecycle;
    std::atomic<std::shared_ptr<Capture>> active;
    // Hot lookup target. Loading the owning atomic<shared_ptr> takes the
    // implementation's spin lock, and find() runs once per indexed draw that
    // carries an object list. The cache address is published separately, and a
    // stopped capture is retired instead of destroyed so a reader that already
    // loaded the raw pointer can still finish its lookup.
    std::atomic<GeometryPipelineCache*> activeCache = nullptr;
    std::vector<std::shared_ptr<Capture>> retired;
    void* graphicsTarget = nullptr;
    void* streamTarget = nullptr;
    bool installed = false;
};
std::atomic<Control*> publishedControl = nullptr;
GeometryPipelineCache* activeGeometryCache() noexcept
{
    auto* state = publishedControl.load(std::memory_order_acquire);
    return state ? state->activeCache.load(std::memory_order_acquire) : nullptr;
}
Control& control()
{
    // Forwarding callbacks outlive the active cache. No destructor joins a
    // worker or releases driver objects under the process loader lock.
    static auto* state = []
    {
        auto* value = new Control;
        publishedControl.store(value, std::memory_order_release);
        return value;
    }();
    return *state;
}
struct CallScope
{
    bool outer = false;
    CallScope() : outer(creationDepth++ == 0 && !GeometryPipelineCache::compilerThread()) {}
    ~CallScope() { --creationDepth; }
};
HRESULT WINAPI graphics(ID3D12Device* device, const D3D12_GRAPHICS_PIPELINE_STATE_DESC* desc, REFIID iid, void** output)
{
    CallScope scope;
    const auto status = originalGraphics(device, desc, iid, output);
    if (scope.outer && SUCCEEDED(status) && desc && output && *output)
    {
        auto capture = control().active.load(std::memory_order_acquire);
        if (capture && capture->device.Get() == device)
        {
            ++capture->graphics;
            ComPtr<ID3D12PipelineState> pipeline;
            if (SUCCEEDED(static_cast<IUnknown*>(*output)->QueryInterface(IID_PPV_ARGS(&pipeline))))
            {
                capture->cache.pipelineCreated(pipeline.Get(), *desc);
                capture->observations.observe(pipeline.Get(), *desc);
            }
        }
    }
    return status;
}
HRESULT WINAPI stream(ID3D12Device2* device, const D3D12_PIPELINE_STATE_STREAM_DESC* desc, REFIID iid, void** output)
{
    CallScope scope;
    const auto status = originalStream(device, desc, iid, output);
    if (scope.outer && SUCCEEDED(status) && output && *output)
    {
        auto capture = control().active.load(std::memory_order_acquire);
        if (capture && capture->device2.Get() == device)
        {
            ++capture->streams;
            const auto parsed = desc ? ParseGeometryPipelineStream(*desc) : GeometryPipelineStreamResult {};
            if (parsed.kind == GeometryPipelineStreamKind::Graphics)
            {
                ++capture->streamGraphics;
                ComPtr<ID3D12PipelineState> pipeline;
                if (SUCCEEDED(static_cast<IUnknown*>(*output)->QueryInterface(IID_PPV_ARGS(&pipeline))))
                {
                    capture->cache.pipelineCreated(pipeline.Get(), parsed.graphics);
                    capture->observations.observe(pipeline.Get(), parsed.graphics);
                }
            }
            else if (parsed.kind == GeometryPipelineStreamKind::NonGraphics)
                ++capture->streamNonGraphics;
            else
                ++capture->streamRejected;
        }
    }
    // Observation happens only after the runtime accepts the original stream.
    // The application's descriptor and returned pipeline are never changed.
    return status;
}
bool install(Control& r, Capture& capture)
{
    auto table = *reinterpret_cast<void***>(capture.device.Get());
    void* graphicsTarget = table[10];
    void* streamTarget = capture.device2 ? (*reinterpret_cast<void***>(capture.device2.Get()))[47] : nullptr;
    if (r.installed)
        return r.graphicsTarget == graphicsTarget && r.streamTarget == streamTarget;
    if (!graphicsTarget || graphicsTarget == streamTarget)
        return false;
    HMODULE resident = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
                            reinterpret_cast<LPCWSTR>(&StartGeometryCreation), &resident))
        return false;
    DetourThreads threads;
    if (!threads.gather() || DetourTransactionBegin() != NO_ERROR)
        return false;
    originalGraphics = reinterpret_cast<GraphicsCreate>(graphicsTarget);
    originalStream = reinterpret_cast<StreamCreate>(streamTarget);
    bool okay =
        DetourAttach(reinterpret_cast<PVOID*>(&originalGraphics), reinterpret_cast<PVOID>(&graphics)) == NO_ERROR;
    if (streamTarget)
        okay = DetourAttach(reinterpret_cast<PVOID*>(&originalStream), reinterpret_cast<PVOID>(&stream)) == NO_ERROR &&
               okay;
    if (!okay || !threads.enlist())
    {
        DetourTransactionAbort();
        return false;
    }
    if (DetourTransactionCommit() != NO_ERROR)
        return false;
    r.graphicsTarget = graphicsTarget;
    r.streamTarget = streamTarget;
    r.installed = true;
    return true;
}
} // namespace

bool StartGeometryCreation(ID3D12Device* device, const std::filesystem::path& compiler,
                           GeometryCacheLimits limits) noexcept
{
    if (!device || !compiler.is_absolute())
        return false;
    try
    {
        auto& r = control();
        std::lock_guard lock(r.lifecycle);
        if (auto existing = r.active.load(std::memory_order_acquire))
            return existing->device.Get() == device;
        D3D12_FEATURE_DATA_D3D12_OPTIONS options {};
        if (FAILED(device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS, &options, sizeof(options))) ||
            !options.ROVsSupported)
            return false;
        auto capture = std::make_shared<Capture>(device, compiler, limits);
        if (!install(r, *capture))
            return false;
        r.activeCache.store(&capture->cache, std::memory_order_release);
        r.active.store(std::move(capture), std::memory_order_release);
        experimentVertexCaptureRequest.store(RequestGeometryVertexCapture, std::memory_order_release);
        return true;
    }
    catch (...)
    {
        return false;
    }
}
void StopGeometryCreation()
{
    auto& r = control();
    std::lock_guard lock(r.lifecycle);
    r.activeCache.store(nullptr, std::memory_order_release);
    if (auto capture = r.active.exchange(nullptr, std::memory_order_acq_rel))
    {
        capture->cache.stop();
        r.retired.push_back(std::move(capture));
    }
}
void ObserveGeometryRoot(ID3D12Device* device, UINT node, const void* bytes, SIZE_T size, IUnknown* created) noexcept
{
    if (!created || GeometryPipelineCache::compilerThread())
        return;
    auto* state = publishedControl.load(std::memory_order_acquire);
    if (!state)
        return;
    auto capture = state->active.load(std::memory_order_acquire);
    if (!capture || capture->device.Get() != device)
        return;
    ++capture->roots;
    ComPtr<ID3D12RootSignature> root;
    if (SUCCEEDED(created->QueryInterface(IID_PPV_ARGS(&root))))
    {
        capture->cache.rootCreated(root.Get(), node, bytes, size);
        capture->observations.rootCreated(root.Get(), bytes, size, node);
    }
}
std::shared_ptr<const GeometryPipelineEntry> FindGeometryPipeline(ID3D12PipelineState* original) noexcept
{
    try
    {
        auto* cache = activeGeometryCache();
        return cache ? cache->find(original) : nullptr;
    }
    catch (...)
    {
        return {};
    }
}
std::shared_ptr<const GeometryPipelineEntry> FindObservedGeometryPipeline(ID3D12PipelineState* original) noexcept
{
    try
    {
        auto* cache = activeGeometryCache();
        if (!cache) return {};
        auto prepared = cache->find(original);
        if (prepared) return prepared;
        auto* state = publishedControl.load(std::memory_order_acquire);
        auto capture = state ? state->active.load(std::memory_order_acquire) : nullptr;
        return capture ? capture->observations.find(original) : nullptr;
    }
    catch (...) { return {}; }
}
bool RequestGeometryVertexCapture(ID3D12PipelineState* original) noexcept
{
    try
    {
        auto* state = publishedControl.load(std::memory_order_acquire);
        if (!state || !original) return false;
        auto capture = state->active.load(std::memory_order_acquire);
        if (!capture) return false;
        // A published entry is already prepared unless it is refusal-only: that
        // one never gets a capture variant, so the request fails.
        if (const auto prepared = capture->cache.find(original))
            return !prepared->refusalOnly();
        const auto observed = capture->observations.find(original);
        return observed && capture->cache.pipelineCreated(original, observed->description, true);
    }
    catch (...) { return false; }
}
GeometryCreationStats GetGeometryCreationStats()
{
    GeometryCreationStats result;
    auto* state = publishedControl.load(std::memory_order_acquire);
    if (!state)
        return result;
    if (auto capture = state->active.load(std::memory_order_acquire))
    {
        result.active = true;
        result.roots = capture->roots.load();
        result.graphics = capture->graphics.load();
        result.streams = capture->streams.load();
        result.streamGraphics = capture->streamGraphics.load();
        result.streamNonGraphics = capture->streamNonGraphics.load();
        result.streamRejected = capture->streamRejected.load();
        result.cache = capture->cache.stats();
        result.observation = capture->observations.stats();
    }
    return result;
}
bool TryGeometryCreationCounters(GeometryCreationStats& result)
{
    auto* state = publishedControl.load(std::memory_order_acquire);
    if (!state)
        return true;
    if (auto capture = state->active.load(std::memory_order_acquire))
    {
        result.active = true;
        result.roots = capture->roots.load();
        result.graphics = capture->graphics.load();
        result.streams = capture->streams.load();
        result.streamGraphics = capture->streamGraphics.load();
        result.streamNonGraphics = capture->streamNonGraphics.load();
        result.streamRejected = capture->streamRejected.load();
        result.observation = capture->observations.stats();
        return capture->cache.tryCounters(result.cache);
    }
    return true;
}
} // namespace GlassFg
