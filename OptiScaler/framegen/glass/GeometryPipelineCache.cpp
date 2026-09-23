#include "pch.h"
#include "GeometryPipelineCache.h"
#include "DxilVertexHistory.h"
#include "GeometryGateTrace.h"
#include "GlassControls.h"
#include "MaterialCaptureBlend.h"
#include <condition_variable>
#include <deque>
#include <mutex>
#include <shared_mutex>
#include <thread>
#include <unordered_map>
#include <variant>
#include <cstring>
#include <stdexcept>

namespace GlassFg
{
namespace
{
std::atomic<std::uint64_t> publishedOpaqueProbeReady { 0 };
std::atomic<std::uint64_t> publishedOpaqueProbeRejected { 0 };
} // namespace

void PublishOpaqueProbeCounters(std::uint64_t ready, std::uint64_t rejected) noexcept
{
    publishedOpaqueProbeReady.store(ready, std::memory_order_relaxed);
    publishedOpaqueProbeRejected.store(rejected, std::memory_order_relaxed);
}
std::uint64_t ReadOpaqueProbeReadyCount() noexcept
{
    return publishedOpaqueProbeReady.load(std::memory_order_relaxed);
}
std::uint64_t ReadOpaqueProbeRejectedCount() noexcept
{
    return publishedOpaqueProbeRejected.load(std::memory_order_relaxed);
}

namespace
{
thread_local bool compilingGeometry = false;
// Memoized lookups per thread. find() runs for every indexed draw that carries
// a candidate pipeline, and the map path takes the shared lock that the
// compiler worker holds exclusively while it rewrites a pipeline. A hit here
// returns without touching the lock.
//
// The table is direct mapped and holds several slots because the render thread
// walks hundreds of distinct pipelines per engine frame; the live stage split
// showed the lookup still costing its full unlocked-map price with a single
// slot because consecutive calls almost never repeat the same pipeline. Only
// ready entries are memoized, so the value can never flip from null to non-null
// behind the memo; the generation counter invalidates every slot whenever an
// entry can change (re-queue, erase, stop) and the epoch makes sure a rebuilt
// session never reuses a previous cache's entry even if the allocator hands
// back the same address.
// Overridable so the offline benchmark can compile the same translation unit
// with one slot (the previous behaviour) and with the shipped table size.
#ifndef GLASS_PIPELINE_FIND_MEMO_SLOTS
#define GLASS_PIPELINE_FIND_MEMO_SLOTS 64
#endif
constexpr unsigned PipelineFindMemoSlots = GLASS_PIPELINE_FIND_MEMO_SLOTS;
static_assert(PipelineFindMemoSlots >= 1 && (PipelineFindMemoSlots & (PipelineFindMemoSlots - 1)) == 0,
              "the lookup memo is direct mapped on a power-of-two slot count");
struct PipelineFindMemo
{
    std::uint64_t epoch = 0;
    std::uint64_t generation = 0;
    ID3D12PipelineState* key = nullptr;
    std::shared_ptr<const GeometryPipelineEntry> value;
};
thread_local PipelineFindMemo pipelineFindMemo[PipelineFindMemoSlots];
// Knuth's multiplicative hash on the block-aligned pointer: the low bits of a
// D3D12 pipeline state are mostly zero, so the slot comes from the high bits.
inline unsigned PipelineFindMemoSlot(ID3D12PipelineState* identity) noexcept
{
    const auto mixed = (static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(identity)) >> 4) * 2654435761ull;
    return static_cast<unsigned>((mixed >> 26) & (PipelineFindMemoSlots - 1));
}
std::atomic<std::uint64_t> nextPipelineCacheEpoch { 1 };
// Diagnostic opaque probe registration cap. The probe adds pipelines that the
// product gate refuses, and every added entry costs a rewritten VS/PS pair, a
// PSO and one compiler job on the single worker thread. 256 keeps the
// diagnostic addition at roughly an eighth of the cache's own 2048-pipeline
// budget, which bounds the extra PSO memory and compile time while still
// covering a scene's opaque material families in one session. Overridable so
// the offline fixture can exercise the cap without compiling 257 shader pairs.
#ifndef GLASS_OPAQUE_PROBE_PIPELINE_LIMIT
#define GLASS_OPAQUE_PROBE_PIPELINE_LIMIT 256
#endif
constexpr std::uint64_t OpaqueProbePipelineLimit = GLASS_OPAQUE_PROBE_PIPELINE_LIMIT;
// Blend state is the only creation-time signal that separates a surface the
// engine draws with alpha blending from ordinary opaque content. It does not
// decide admission; it labels the census so a refused blended pipeline can be
// told apart from a refused opaque one.
bool blendedTarget(const D3D12_GRAPHICS_PIPELINE_STATE_DESC& d) noexcept
{
    if (d.BlendState.AlphaToCoverageEnable)
        return true;
    for (UINT target = 0; target < d.NumRenderTargets && target < 8; ++target)
    {
        const auto& value = d.BlendState.RenderTarget[d.BlendState.IndependentBlendEnable ? target : 0];
        if (value.RenderTargetWriteMask && value.BlendEnable && !value.LogicOpEnable)
            return true;
    }
    return false;
}

// Returns GateCandidateRoot when the pipeline is admitted, otherwise the first
// check that refused it. The order matches the single boolean expression this
// replaced so the reason is the same one that used to short-circuit.
//
// opaqueProbe relaxes exactly the two refusals the diagnostic flag owns: the
// depth-write mask and the missing blend. Every other refusal - including the
// stencil write, which also changes the depth buffer later passes read - stays
// in force, so the probe cannot admit a pipeline the rewrite path was not
// audited for beyond those two.
unsigned candidateReason(const D3D12_GRAPHICS_PIPELINE_STATE_DESC& d, bool vertexOnly, bool opaqueProbe)
{
    D3D12_BLEND_DESC ignored {};
    const auto keep = [](const D3D12_DEPTH_STENCILOP_DESC& s)
    {
        return s.StencilFailOp == D3D12_STENCIL_OP_KEEP && s.StencilDepthFailOp == D3D12_STENCIL_OP_KEEP &&
               s.StencilPassOp == D3D12_STENCIL_OP_KEEP;
    };
    if (!d.pRootSignature)
        return GateCandidateRoot;
    if (!d.VS.pShaderBytecode || !d.VS.BytecodeLength)
        return GateCandidateVertex;
    if (!d.PS.pShaderBytecode || !d.PS.BytecodeLength)
        return GateCandidatePixel;
    if (d.VS.BytecodeLength > 2 * 1024 * 1024)
        return GateCandidateVertexBytes;
    if (d.PS.BytecodeLength > 2 * 1024 * 1024)
        return GateCandidatePixelBytes;
    if (!d.NumRenderTargets || d.NumRenderTargets > D3D12_SIMULTANEOUS_RENDER_TARGET_COUNT)
        return GateCandidateTargets;
    if (d.SampleDesc.Count != 1)
        return GateCandidateSamples;
    if (d.GS.BytecodeLength || d.HS.BytecodeLength || d.DS.BytecodeLength)
        return GateCandidateShaderStages;
    if (d.StreamOutput.NumEntries || d.StreamOutput.NumStrides)
        return GateCandidateStreamOutput;
    if (d.PrimitiveTopologyType != D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE)
        return GateCandidateTopology;
    if (d.InputLayout.NumElements > 32 || (d.InputLayout.NumElements && !d.InputLayout.pInputElementDescs))
        return GateCandidateInputLayout;
    if (vertexOnly)
        return GateCandidateRoot;
    if (!opaqueProbe && d.DepthStencilState.DepthEnable &&
        d.DepthStencilState.DepthWriteMask != D3D12_DEPTH_WRITE_MASK_ZERO)
        return GateCandidateDepthWrite;
    if (d.DepthStencilState.StencilEnable && d.DepthStencilState.StencilWriteMask &&
        !(keep(d.DepthStencilState.FrontFace) && keep(d.DepthStencilState.BackFace)))
        return GateCandidateStencilWrite;
    if (!opaqueProbe &&
        !(tryMaterialCaptureBlend(d.BlendState, MaterialCapture::SourceColor, ignored, true) ||
          (d.BlendState.RenderTarget[0].BlendEnable && !d.BlendState.RenderTarget[0].LogicOpEnable &&
           d.BlendState.RenderTarget[0].RenderTargetWriteMask)))
        return GateCandidateBlend;
    return GateCandidateRoot;
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
    // Readers (every DrawIndexedInstanced that asks whether this pipeline is a
    // candidate) used to take an exclusive lock shared with the compiler
    // thread, which parks the render thread behind pipeline creation. The
    // writer side is unchanged; only the read path is shared now.
    mutable std::shared_mutex mutex;
    // The worker's wait uses the same shared_mutex as the readers, so the
    // condition variable has to accept an arbitrary lock type.
    std::condition_variable_any changed;
    std::unordered_map<ID3D12RootSignature*, std::shared_ptr<RootWork>> roots;
    std::unordered_map<ID3D12PipelineState*, std::shared_ptr<PipelineWork>> pipelines;
    std::deque<Job> jobs;
    GeometryCacheStats counters;
    std::thread worker;
    bool stopping = false;
    const std::uint64_t epoch = nextPipelineCacheEpoch.fetch_add(1, std::memory_order_relaxed);
    // Bumped under the unique lock whenever a published entry can change.
    std::atomic<std::uint64_t> generations { 0 };

    void invalidateLookups() noexcept { generations.fetch_add(1, std::memory_order_release); }

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
                        bool packedPairMissing = false;
                        if (entry.vertexOnlyCapture)
                            status = compiler.createVertexCapture(device.Get(), *work.root->result, entry.description,
                                                                  entry.instrumented, error);
                        else
                        {
                            std::string packedError;
                            // Audited Cyberpunk transparent-VS camera block: the
                            // raw NDC jitter pair lives in row 51 of the
                            // 848-byte b1/space0 constant buffer. Packed capture
                            // stores the current frame's words in every record
                            // tag and the linked pixel shader adds their
                            // frame-to-frame UV delta, so the delivered motion
                            // no longer depends on the declared compose jitter.
                            // Shaders without this binding are recompiled
                            // without the pair instead of being dropped (R6).
                            constexpr VertexConstantPair packedCameraCapture { 0, 1, 848, 51 };
                            const auto materialStatus = compiler.create(device.Get(), *work.root->result,
                                                                         entry.description, entry.instrumented, error);
                            const auto packedStatus = compiler.createPackedMotion(device.Get(), *work.root->result,
                                                                                   entry.description, entry.packed,
                                                                                   packedError,
                                                                                   &packedCameraCapture,
                                                                                   &packedPairMissing);
                            // F-01: without the audited constant pair the packed pixel
                            // stage adds a zero delta, so the recorded motion is the raw
                            // jittered position difference instead of the engine's motion
                            // convention. Withhold that variant from delivery: prepare()
                            // then leaves the draw on the game's own pipeline and the
                            // engine's motion value survives byte for byte. The compiler
                            // still returns the pair-less PSO for the offline fixtures.
                            if (SUCCEEDED(packedStatus) && packedPairMissing)
                                entry.packed.Reset();
                            {
                                std::lock_guard lock(mutex);
                                if (SUCCEEDED(packedStatus) && !packedPairMissing)
                                    ++counters.packedReady;
                                else
                                {
                                    ++counters.packedRejected;
                                    counters.lastPackedError =
                                        packedPairMissing
                                            ? "capture constant pair missing: packed variant withheld (no frame-to-frame jitter delta)"
                                            : packedError;
                                    if (packedPairMissing)
                                        ++counters.packedDeltaMissing;
                                }
                            }
                            status = SUCCEEDED(materialStatus) || (SUCCEEDED(packedStatus) && !packedPairMissing)
                                         ? S_OK
                                         : packedStatus;
                            if (FAILED(status) && !packedError.empty())
                                error = packedError;
                        }
                        if (SUCCEEDED(status))
                        {
                            entry.root = work.root->result;
                            entry.deltaMissing = packedPairMissing;
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
            invalidateLookups();
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
            invalidateLookups();
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
    // Same bisect switch as pipelineCreated: with compilation disabled the
    // module creates no extended root and no rewritten pipeline at all.
    if (!GeometryPipelineCompilationEnabled())
        return false;
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
    const bool transparentLooking = blendedTarget(desc);
    const auto reject = [&](unsigned reason)
    {
        GateNoteCreatedPipeline(reason, transparentLooking);
        GateRecordCreatedPipeline(identity, reason, transparentLooking);
        return false;
    };
    // Bisect switch: observe without ever creating a rewritten pipeline.
    if (!GeometryPipelineCompilationEnabled())
        return reject(GateCandidateCompilationOff);
    // Pipelines created by the compiler thread (and the module's own rewrites)
    // are not game content and must not enter the census.
    if (compilingGeometry || !identity)
        return false;
    // The probe re-runs the same predicate with and without its own relaxation:
    // a pipeline the strict predicate already admitted is unaffected, so the
    // probe counters only describe the pipelines this flag alone registered.
    const bool opaqueProbe = OpaqueProbeEnabled();
    const auto strictReason = candidateReason(desc, vertexOnly, false);
    const auto reason = opaqueProbe ? candidateReason(desc, vertexOnly, true) : strictReason;
    const bool probeOnly = opaqueProbe && strictReason != GateCandidateRoot && reason == GateCandidateRoot;
    if (reason != GateCandidateRoot)
        return reject(reason);
    GateRecordCreatedPipeline(identity, GateCandidateRoot, transparentLooking);
    try
    {
        auto& r = *implementation;
        std::lock_guard lock(r.mutex);
        if (r.stopping)
            return reject(GateCandidateLimits);
        if (const auto existing = r.pipelines.find(identity); existing != r.pipelines.end())
        {
            auto& work = *existing->second;
            if (work.ready || !work.completed)
            {
                GateNoteCreatedPipeline(GateCandidateRoot, transparentLooking);
                return true;
            }
            // A rejected material rewrite must not permanently block explicit
            // vertex-only capture. Failed entries have never been published.
            if (!vertexOnly || work.entry->vertexOnlyCapture || !work.root->result)
                return reject(GateCandidateLimits);
            // The entry is still published here: a reader that memoized it must
            // fall back to the locked lookup before the rewrite starts.
            r.invalidateLookups();
            r.jobs.emplace_back(existing->second);
            work.entry->vertexOnlyCapture = true;
            work.entry->instrumented.Reset();
            work.completed = false;
            ++r.counters.pending;
            r.changed.notify_one();
            GateNoteCreatedPipeline(GateCandidateRoot, transparentLooking);
            return true;
        }
        const auto root = r.roots.find(desc.pRootSignature);
        const std::size_t bytes = desc.VS.BytecodeLength + desc.PS.BytecodeLength +
                                  desc.InputLayout.NumElements * (sizeof(D3D12_INPUT_ELEMENT_DESC) + 128);
        if (root == r.roots.end())
            return reject(GateCandidateRootUnknown);
        if (probeOnly && r.counters.packedOpaqueProbeReady >= OpaqueProbePipelineLimit)
        {
            // Over the diagnostic cap: the pipeline stays on the engine's own
            // motion and the census keeps the reason the product gate would
            // have recorded, so an aborted probe session is still comparable
            // with a probe-off session.
            ++r.counters.packedOpaqueProbeRejected;
            PublishOpaqueProbeCounters(r.counters.packedOpaqueProbeReady, r.counters.packedOpaqueProbeRejected);
            return reject(strictReason);
        }
        if (r.pipelines.size() >= r.limits.pipelines || !r.budget(bytes))
            return reject(GateCandidateLimits);
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
            r.invalidateLookups();
            throw;
        }
        ++r.counters.pipelines;
        ++r.counters.pending;
        if (probeOnly)
        {
            ++r.counters.packedOpaqueProbeReady;
            PublishOpaqueProbeCounters(r.counters.packedOpaqueProbeReady, r.counters.packedOpaqueProbeRejected);
        }
        r.counters.retainedBytes += bytes;
        r.changed.notify_one();
        GateNoteCreatedPipeline(GateCandidateRoot, transparentLooking);
        return true;
    }
    catch (...)
    {
        return reject(GateCandidateLimits);
    }
}

std::shared_ptr<const GeometryPipelineEntry> GeometryPipelineCache::find(ID3D12PipelineState* identity) const
{
    const auto& r = *implementation;
    if (!identity)
        return nullptr;
    auto& memo = pipelineFindMemo[PipelineFindMemoSlot(identity)];
    if (memo.epoch == r.epoch && memo.key == identity && memo.value &&
        memo.generation == r.generations.load(std::memory_order_acquire))
        return memo.value;
    std::shared_lock lock(r.mutex);
    const auto it = r.pipelines.find(identity);
    auto result = !r.stopping && it != r.pipelines.end() && it->second->ready ? it->second->entry : nullptr;
    if (result)
    {
        memo.epoch = r.epoch;
        memo.generation = r.generations.load(std::memory_order_relaxed);
        memo.key = identity;
        memo.value = result;
    }
    else if (memo.epoch == r.epoch && memo.key == identity)
    {
        memo.key = nullptr;
        memo.value.reset();
    }
    return result;
}
GeometryCacheStats GeometryPipelineCache::stats() const
{
    const auto& r = *implementation;
    std::shared_lock lock(r.mutex);
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
    result.packedOpaqueProbeReady = r.counters.packedOpaqueProbeReady;
    result.packedOpaqueProbeRejected = r.counters.packedOpaqueProbeRejected;
    return true; // No allocation or diagnostic-string copy on the UI path.
}
} // namespace GlassFg
