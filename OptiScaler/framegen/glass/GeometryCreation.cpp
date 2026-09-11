#include "pch.h"
#include "GeometryCreation.h"
#include "DetourThreads.h"
#include <hooks/Hook_Utils.h>
#include <atomic>
#include <mutex>

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
    std::atomic<std::uint64_t> roots = 0, graphics = 0, streams = 0;
    Capture(ID3D12Device* d, const std::filesystem::path& path, GeometryCacheLimits limits)
        : device(d), cache(d, path, limits)
    {
        d->QueryInterface(IID_PPV_ARGS(&device2));
    }
};
struct Control
{
    std::mutex lifecycle;
    std::atomic<std::shared_ptr<Capture>> active;
    void* graphicsTarget = nullptr;
    void* streamTarget = nullptr;
    bool installed = false;
};
std::atomic<Control*> publishedControl = nullptr;
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
                capture->cache.pipelineCreated(pipeline.Get(), *desc);
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
            ++capture->streams;
    }
    // Stream variants are counted, never silently labeled supported or parsed
    // as the older graphics descriptor. They retain the original pipeline.
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
        r.active.store(std::move(capture), std::memory_order_release);
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
    if (auto capture = r.active.exchange(nullptr, std::memory_order_acq_rel))
        capture->cache.stop();
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
        capture->cache.rootCreated(root.Get(), node, bytes, size);
}
std::shared_ptr<const GeometryPipelineEntry> FindGeometryPipeline(ID3D12PipelineState* original) noexcept
{
    try
    {
        auto* state = publishedControl.load(std::memory_order_acquire);
        if (!state)
            return {};
        auto capture = state->active.load(std::memory_order_acquire);
        return capture ? capture->cache.find(original) : nullptr;
    }
    catch (...)
    {
        return {};
    }
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
        result.cache = capture->cache.stats();
    }
    return result;
}
} // namespace GlassFg
