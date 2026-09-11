#include "pch.h"
#include "GeometryPipelineCache.h"
#include "MaterialCaptureBlend.h"
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <variant>
#include <cstring>
#include <stdexcept>

namespace GlassFg
{
namespace
{
thread_local bool compilingGeometry = false;
bool candidate(const D3D12_GRAPHICS_PIPELINE_STATE_DESC& d, bool vertexOnly)
{
    D3D12_BLEND_DESC ignored {};
    const auto keep = [](const D3D12_DEPTH_STENCILOP_DESC& s)
    {
        return s.StencilFailOp == D3D12_STENCIL_OP_KEEP && s.StencilDepthFailOp == D3D12_STENCIL_OP_KEEP &&
               s.StencilPassOp == D3D12_STENCIL_OP_KEEP;
    };
    return d.pRootSignature && d.VS.pShaderBytecode && d.PS.pShaderBytecode && d.VS.BytecodeLength &&
           d.PS.BytecodeLength && d.VS.BytecodeLength <= 2 * 1024 * 1024 && d.PS.BytecodeLength <= 2 * 1024 * 1024 &&
           d.NumRenderTargets && d.NumRenderTargets <= D3D12_SIMULTANEOUS_RENDER_TARGET_COUNT &&
           d.SampleDesc.Count == 1 && !d.GS.BytecodeLength && !d.HS.BytecodeLength && !d.DS.BytecodeLength &&
           !d.StreamOutput.NumEntries && !d.StreamOutput.NumStrides &&
           d.PrimitiveTopologyType == D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE && d.InputLayout.NumElements <= 32 &&
           (!d.InputLayout.NumElements || d.InputLayout.pInputElementDescs) &&
           (vertexOnly || ((!d.DepthStencilState.DepthEnable || d.DepthStencilState.DepthWriteMask == D3D12_DEPTH_WRITE_MASK_ZERO) &&
           (!d.DepthStencilState.StencilEnable || !d.DepthStencilState.StencilWriteMask ||
            (keep(d.DepthStencilState.FrontFace) && keep(d.DepthStencilState.BackFace))) &&
           tryMaterialCaptureBlend(d.BlendState, MaterialCapture::SourceColor, ignored)));
}
} // namespace

struct GeometryPipelineCache::Impl
{
    struct RootWork
    {
        Microsoft::WRL::ComPtr<ID3D12RootSignature> original;
        UINT node = 0;
        std::vector<std::byte> bytes;
        std::shared_ptr<GeometryRoot> result;
    };
    struct PipelineWork
    {
        std::shared_ptr<RootWork> root;
        std::shared_ptr<GeometryPipelineEntry> entry;
        bool ready = false;
        bool completed = false;
    };
    using Job = std::variant<std::shared_ptr<RootWork>, std::shared_ptr<PipelineWork>>;
    Microsoft::WRL::ComPtr<ID3D12Device> device;
    std::filesystem::path compilerPath;
    GeometryCacheLimits limits;
    mutable std::mutex mutex;
    std::condition_variable changed;
    std::unordered_map<ID3D12RootSignature*, std::shared_ptr<RootWork>> roots;
    std::unordered_map<ID3D12PipelineState*, std::shared_ptr<PipelineWork>> pipelines;
    std::deque<Job> jobs;
    GeometryCacheStats counters;
    std::thread worker;
    bool stopping = false;

    Impl(ID3D12Device* d, std::filesystem::path path, GeometryCacheLimits l)
        : device(d), compilerPath(std::move(path)), limits(l)
    {
        if (!d || !compilerPath.is_absolute() || !limits.roots || !limits.pipelines || limits.bytes < 4096)
            throw std::invalid_argument("Invalid geometry pipeline cache configuration");
        worker = std::thread([this] { run(); });
    }
    void run() noexcept
    {
        compilingGeometry = true;
        try
        {
            GeometryCompiler compiler(compilerPath);
            for (;;)
            {
                Job job;
                {
                    std::unique_lock lock(mutex);
                    changed.wait(lock, [&] { return stopping || !jobs.empty(); });
                    if (stopping)
                        break;
                    job = std::move(jobs.front());
                    jobs.pop_front();
                }
                std::string error;
                HRESULT status = E_FAIL;
                if (auto* item = std::get_if<std::shared_ptr<RootWork>>(&job))
                {
                    auto result = std::make_shared<GeometryRoot>();
                    auto& root = **item;
                    status = CreateGeometryRoot(device.Get(), root.original.Get(), root.node, root.bytes.data(),
                                                root.bytes.size(), *result, error, GeometryLayout::PerInstance);
                    std::lock_guard lock(mutex);
                    if (SUCCEEDED(status))
                        root.result = std::move(result);
                }
                else
                {
                    auto& work = *std::get<std::shared_ptr<PipelineWork>>(job);
                    // FIFO root creation precedes every referring pipeline job.
                    if (work.root->result)
                    {
                        auto& entry = *work.entry;
                        status = entry.vertexOnlyCapture
                            ? compiler.createVertexCapture(device.Get(), *work.root->result, entry.description,
                                                           entry.instrumented, error)
                            : compiler.create(device.Get(), *work.root->result, entry.description,
                                              entry.instrumented, error);
                        if (SUCCEEDED(status))
                        {
                            entry.root = work.root->result;
                            std::lock_guard lock(mutex);
                            work.ready = true;
                            ++counters.ready;
                        }
                    }
                    else
                        error = "Original root could not be extended";
                }
                {
                    std::lock_guard lock(mutex);
                    --counters.pending;
                    if (FAILED(status))
                    {
                        ++counters.rejected;
                        counters.lastError = error;
                    }
                    if (auto* item = std::get_if<std::shared_ptr<PipelineWork>>(&job))
                        (**item).completed = true;
                }
            }
        }
        catch (...)
        {
            std::lock_guard lock(mutex);
            counters.rejected += counters.pending;
            counters.pending = 0;
            stopping = true;
            jobs.clear();
        }
        compilingGeometry = false;
    }
    bool budget(std::size_t bytes) const
    {
        return counters.retainedBytes <= limits.bytes && bytes <= limits.bytes - counters.retainedBytes;
    }
    void stop()
    {
        {
            std::lock_guard lock(mutex);
            stopping = true;
            counters.rejected += jobs.size();
            counters.pending -= jobs.size();
            jobs.clear();
        }
        changed.notify_one();
        if (worker.joinable())
            worker.join();
    }
    ~Impl() { stop(); }
};

GeometryPipelineCache::GeometryPipelineCache(ID3D12Device* device, std::filesystem::path compiler,
                                             GeometryCacheLimits limits)
    : implementation(std::make_unique<Impl>(device, std::move(compiler), limits))
{
}
GeometryPipelineCache::~GeometryPipelineCache() = default;
bool GeometryPipelineCache::compilerThread() { return compilingGeometry; }
void GeometryPipelineCache::stop() { implementation->stop(); }

bool GeometryPipelineCache::rootCreated(ID3D12RootSignature* identity, UINT node, const void* bytes,
                                        SIZE_T size) noexcept
{
    if (compilingGeometry || !identity || !bytes || !size || size > 1024 * 1024)
        return false;
    try
    {
        auto& r = *implementation;
        std::lock_guard lock(r.mutex);
        if (r.stopping)
            return false;
        if (r.roots.contains(identity))
            return true;
        if (r.roots.size() >= r.limits.roots || !r.budget(size * 2))
            return false;
        auto work = std::make_shared<Impl::RootWork>();
        work->original = identity;
        work->node = node;
        work->bytes.resize(size);
        std::memcpy(work->bytes.data(), bytes, size);
        r.roots.emplace(identity, work);
        try
        {
            r.jobs.emplace_back(work);
        }
        catch (...)
        {
            r.roots.erase(identity);
            throw;
        }
        ++r.counters.roots;
        ++r.counters.pending;
        r.counters.retainedBytes += size * 2; // Worker source plus immutable root serialization.
        r.changed.notify_one();
        return true;
    }
    catch (...)
    {
        return false;
    }
}

bool GeometryPipelineCache::pipelineCreated(ID3D12PipelineState* identity,
                                            const D3D12_GRAPHICS_PIPELINE_STATE_DESC& desc, bool vertexOnly) noexcept
{
    if (compilingGeometry || !identity || !candidate(desc, vertexOnly))
        return false;
    try
    {
        auto& r = *implementation;
        std::lock_guard lock(r.mutex);
        if (r.stopping)
            return false;
        if (const auto existing = r.pipelines.find(identity); existing != r.pipelines.end())
        {
            auto& work = *existing->second;
            if (work.ready || !work.completed) return true;
            // A rejected material rewrite must not permanently block explicit
            // vertex-only capture. Failed entries have never been published.
            if (!vertexOnly || work.entry->vertexOnlyCapture || !work.root->result) return false;
            r.jobs.emplace_back(existing->second);
            work.entry->vertexOnlyCapture = true;
            work.entry->instrumented.Reset();
            work.completed = false;
            ++r.counters.pending;
            r.changed.notify_one();
            return true;
        }
        const auto root = r.roots.find(desc.pRootSignature);
        const std::size_t bytes = desc.VS.BytecodeLength + desc.PS.BytecodeLength +
                                  desc.InputLayout.NumElements * (sizeof(D3D12_INPUT_ELEMENT_DESC) + 128);
        if (root == r.roots.end() || r.pipelines.size() >= r.limits.pipelines || !r.budget(bytes))
            return false;
        auto work = std::make_shared<Impl::PipelineWork>();
        work->root = root->second;
        work->entry = std::make_shared<GeometryPipelineEntry>();
        auto& entry = *work->entry;
        entry.vertexOnlyCapture = vertexOnly;
        entry.original = identity;
        entry.description = desc;
        entry.vertexBytes.resize(desc.VS.BytecodeLength);
        entry.pixelBytes.resize(desc.PS.BytecodeLength);
        std::memcpy(entry.vertexBytes.data(), desc.VS.pShaderBytecode, desc.VS.BytecodeLength);
        std::memcpy(entry.pixelBytes.data(), desc.PS.pShaderBytecode, desc.PS.BytecodeLength);
        entry.description.VS = { entry.vertexBytes.data(), entry.vertexBytes.size() };
        entry.description.PS = { entry.pixelBytes.data(), entry.pixelBytes.size() };
        entry.description.CachedPSO = {};
        entry.description.StreamOutput = {};
        entry.semantics.reserve(desc.InputLayout.NumElements);
        if (desc.InputLayout.NumElements)
            entry.inputs.assign(desc.InputLayout.pInputElementDescs,
                                desc.InputLayout.pInputElementDescs + desc.InputLayout.NumElements);
        for (auto& element : entry.inputs)
        {
            if (!element.SemanticName || !element.SemanticName[0] || strnlen_s(element.SemanticName, 128) == 128)
                return false;
            entry.semantics.emplace_back(element.SemanticName);
            element.SemanticName = entry.semantics.back().c_str();
        }
        entry.description.InputLayout = { entry.inputs.data(), static_cast<UINT>(entry.inputs.size()) };
        entry.identity = r.counters.pipelines + 1;
        r.pipelines.emplace(identity, work);
        try
        {
            r.jobs.emplace_back(work);
        }
        catch (...)
        {
            r.pipelines.erase(identity);
            throw;
        }
        ++r.counters.pipelines;
        ++r.counters.pending;
        r.counters.retainedBytes += bytes;
        r.changed.notify_one();
        return true;
    }
    catch (...)
    {
        return false;
    }
}

std::shared_ptr<const GeometryPipelineEntry> GeometryPipelineCache::find(ID3D12PipelineState* identity) const
{
    const auto& r = *implementation;
    std::lock_guard lock(r.mutex);
    const auto it = r.pipelines.find(identity);
    return !r.stopping && it != r.pipelines.end() && it->second->ready ? it->second->entry : nullptr;
}
GeometryCacheStats GeometryPipelineCache::stats() const
{
    const auto& r = *implementation;
    std::lock_guard lock(r.mutex);
    return r.counters;
}
bool GeometryPipelineCache::tryCounters(GeometryCacheStats& result) const
{
    const auto& r = *implementation;
    std::unique_lock lock(r.mutex, std::try_to_lock);
    if (!lock.owns_lock())
        return false;
    result.roots = r.counters.roots;
    result.pipelines = r.counters.pipelines;
    result.ready = r.counters.ready;
    result.rejected = r.counters.rejected;
    result.pending = r.counters.pending;
    result.retainedBytes = r.counters.retainedBytes;
    return true; // No allocation or diagnostic-string copy on the UI path.
}
} // namespace GlassFg
