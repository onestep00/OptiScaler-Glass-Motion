#pragma once
#include "GeometryPipeline.h"
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

namespace GlassFg
{
// Native graft outcome of one pipeline job. The compiler worker decides it once
// per job (GeometryPipelineCache.cpp) and it is final for the entry. It exists
// for the coverage report and GeometryPipelineEntry::refusalOnly: the draw path
// reads nativeGraft and the variant pointers, never this value.
enum class GeometryGraftKind : std::uint8_t
{
    Pending,       // The job has not finished.
    VertexOnly,    // Explicit vertex-only capture entry; only the refusal lookup ran.
    Missing,       // No graft record for the VS.
    Refused,       // No record: the catalog refused the VS (NativeGraftRefusal).
    ClassDisabled, // Graft record whose supply class is disabled.
    Rejected,      // Graft record whose packed variant failed to build.
    CameraOnly,    // Camera-only graft: one variant serves every draw.
    Root,          // Root graft plus the camera-only array variant.
    RootNoArray,   // Root graft without an array variant.
};
// Lower-case log name: pending, vertex_only, missing, refused, class_disabled,
// rejected, camera_only, root, root_noarray.
const char* GeometryGraftKindName(GeometryGraftKind kind) noexcept;

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
    // Coverage identity, written by the compiler worker before the entry is
    // published and constant afterwards. Each holds the first eight bytes of the
    // SHA-256 of the original VS or PS container, most significant byte first.
    // %016llx therefore prints the 16-hex-digit prefix of the native catalog's
    // programs[].sha256 (all-cache-techniques.json). Zero when hashing failed.
    std::uint64_t vertexHash = 0, pixelHash = 0;
    GeometryGraftKind graftKind = GeometryGraftKind::Pending;
    std::vector<std::byte> vertexBytes, pixelBytes;
    std::vector<std::string> semantics;
    std::vector<D3D12_INPUT_ELEMENT_DESC> inputs;
    // Per-pipeline coverage for the GEOMETRY_PIPELINES report. The packed
    // capture adds to these with relaxed atomics, and only while the gate trace
    // is armed (GateArmed). gate=on and gate=reset zero them. No render path
    // reads them. They are kept last in the entry, away from the fields the
    // draw path reads.
    struct Coverage
    {
        // draws: prepare calls that carried this entry.
        // captures: those recorded with a packed variant (root graft, camera-only
        //   array or vertex history).
        // graft, array: the captures that used the root graft and the
        //   camera-only array variant.
        // arrayRejected: array or multi-instance draws of a graft entry that has
        //   neither an array nor a history variant. They keep the engine's
        //   motion.
        std::atomic<std::uint64_t> draws { 0 }, captures { 0 }, graft { 0 }, array { 0 }, arrayRejected { 0 };
    };
    mutable Coverage coverage;
    // Published for the coverage report only: the catalog refused the VS and
    // neither the material nor the packed rewrite built a variant, so every draw
    // keeps the engine's output. The entry never gets a capture variant later,
    // not even an explicit vertex-only one: pipelineCreated(..., vertexOnly) and
    // RequestGeometryVertexCapture refuse it instead of reporting it prepared.
    bool refusalOnly() const noexcept
    {
        return graftKind == GeometryGraftKind::Refused && !instrumented && !packed;
    }
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

// One published entry as the coverage report reads it. The counters are relaxed
// snapshots of GeometryPipelineEntry::coverage.
struct GeometryPipelineCoverage
{
    std::uint64_t identity = 0, vertexHash = 0, pixelHash = 0;
    std::uint64_t draws = 0, captures = 0, graft = 0, array = 0, arrayRejected = 0;
    GeometryGraftKind kind = GeometryGraftKind::Pending;
    // A vertex-history variant exists: `packed` itself when no graft is
    // usable, `packedHistory` for the array draws of a graft entry.
    bool history = false;
};

// Process-wide publish of the diagnostic opaque probe counters. The control
// status file is written outside the cache lock and there is one live cache per
// process, so the cache publishes these relaxed atomics whenever either value
// changes; the status writer only reads them. Diagnostics only - no render path
// reads them.
void PublishOpaqueProbeCounters(std::uint64_t ready, std::uint64_t rejected) noexcept;
std::uint64_t ReadOpaqueProbeReadyCount() noexcept;
std::uint64_t ReadOpaqueProbeRejectedCount() noexcept;

// Changes whenever find() of any cache in this process can return a different
// result: an entry became ready, a published entry may change (re-queue, erase)
// or a cache stopped. A caller that memoizes find() results, misses included,
// reuses one only while this value is unchanged. Never 0.
std::uint64_t GeometryPipelineLookupGeneration() noexcept;

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
    // Coverage report (diagnostics). The report thread cannot reach the cache
    // that GeometryCreation owns, so every cache registers itself for the
    // lifetime of the object. readCoverage appends one record per published
    // entry of every live cache that has not stopped (in the product, the one
    // GeometryCreation cache). It takes each cache's shared lock once.
    // resetCoverage zeroes the per-entry counters (gate=on, gate=reset).
    static void readCoverage(std::vector<GeometryPipelineCoverage>& entries);
    static void resetCoverage() noexcept;

  private:
    struct Impl;
    std::unique_ptr<Impl> implementation;
};
} // namespace GlassFg
