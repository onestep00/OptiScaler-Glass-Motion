#pragma once
#include "GeometryPipeline.h"
#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

namespace GlassFg
{
struct GeometryPipelineEntry
{
    std::shared_ptr<const GeometryRoot> root;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> original, instrumented, packed;
    // Camera-only graft variant for array/grouped or multi-instance draws of a
    // graft pipeline: the grafted VS's previous clip is the native previous
    // view-projection applied to each element's own current world position
    // (the engine's own convention for array elements, which have no per-element
    // previous transform). Compiled with `packed` when the graft has one; for a
    // camera-only graft (no root variant) it is the same pipeline as `packed`.
    Microsoft::WRL::ComPtr<ID3D12PipelineState> packedArray;
    // Vertex-history packed variant for array/grouped or multi-instance draws of
    // a graft pipeline without packedArray. Compiled only when
    // VertexHistoryFallback was on at compile time; otherwise such draws keep
    // the engine's motion.
    Microsoft::WRL::ComPtr<ID3D12PipelineState> packedHistory;
    D3D12_GRAPHICS_PIPELINE_STATE_DESC description {};
    std::uint64_t identity = 0;
    bool vertexOnlyCapture = false;
    // The packed variant was compiled without the audited camera constant pair,
    // so its record cannot carry the frame-to-frame jitter delta. The cache
    // withholds `packed` for such an entry and the draw keeps the engine's own
    // motion; this flag separates the cause from any other missing pipeline
    // (F-01).
    bool deltaMissing = false;
    // `packed` takes the previous clip from the engine's MotionMatrix supply
    // through a grafted VS, or, for a camera-only graft, from the previous
    // view-projection on the VS's own world position. The engine evaluates the
    // MotionMatrix supply once per draw proxy, so the packed capture admits a
    // root variant only for a single-instance draw with a non-array identity.
    bool nativeGraft = false;
    std::vector<std::byte> vertexBytes, pixelBytes;
    std::vector<std::string> semantics;
    std::vector<D3D12_INPUT_ELEMENT_DESC> inputs;
};

struct GeometryCacheLimits
{
    std::size_t roots = 128, pipelines = 2048, bytes = 128 * 1024 * 1024;
};
struct GeometryCacheStats
{
    std::uint64_t roots = 0, pipelines = 0, ready = 0, rejected = 0, pending = 0, retainedBytes = 0;
    std::uint64_t packedReady = 0, packedRejected = 0;
    // Pipelines whose packed variant was withheld because the audited camera
    // constant pair was absent (subset of packedRejected, F-01).
    std::uint64_t packedDeltaMissing = 0;
    // Diagnostic opaque probe (GlassFG/OpaqueProbe, default off). ready counts
    // the pipelines this flag alone admitted into the cache; rejected counts the
    // probe-eligible pipelines that were left out afterwards (registration cap).
    std::uint64_t packedOpaqueProbeReady = 0, packedOpaqueProbeRejected = 0;
    std::string lastError, lastPackedError;
};

// Process-wide publish of the diagnostic opaque probe counters. The control
// status file is written outside the cache lock and there is one live cache per
// process, so the cache publishes these relaxed atomics whenever either value
// changes; the status writer only reads them. Diagnostics only - no render path
// reads them.
void PublishOpaqueProbeCounters(std::uint64_t ready, std::uint64_t rejected) noexcept;
std::uint64_t ReadOpaqueProbeReadyCount() noexcept;
std::uint64_t ReadOpaqueProbeRejectedCount() noexcept;


// Captures successful public creation calls, then builds the paired shaders on
// one worker. find() never compiles, waits for a result, or calls a D3D12 method.
// A caller recording a returned entry must retain its shared_ptr until both GPU
// completion and recording discard. Cache membership alone is not GPU ownership.
class GeometryPipelineCache
{
  public:
    GeometryPipelineCache(ID3D12Device* device, std::filesystem::path compiler, GeometryCacheLimits limits = {});
    ~GeometryPipelineCache();
    GeometryPipelineCache(const GeometryPipelineCache&) = delete;
    GeometryPipelineCache& operator=(const GeometryPipelineCache&) = delete;

    bool rootCreated(ID3D12RootSignature* root, UINT nodeMask, const void* bytes, SIZE_T size) noexcept;
    bool pipelineCreated(ID3D12PipelineState* pipeline, const D3D12_GRAPHICS_PIPELINE_STATE_DESC& desc,
                         bool vertexOnly = false) noexcept;
    std::shared_ptr<const GeometryPipelineEntry> find(ID3D12PipelineState* original) const;
    GeometryCacheStats stats() const;
    bool tryCounters(GeometryCacheStats& result) const;
    // May join the compiler thread. Do not call from a render/API callback.
    void stop();
    static bool compilerThread();

  private:
    struct Impl;
    std::unique_ptr<Impl> implementation;
};
} // namespace GlassFg
