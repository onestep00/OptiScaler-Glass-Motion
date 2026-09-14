#pragma once
#include <d3d12.h>
#include <array>
#include <cstdint>
#include <cstdio>

namespace GlassFg
{
struct GeometryDrawView;
struct CyberpunkMeshShape;
struct GeometryPipelineEntry;
struct VertexHistoryKey;
// Process-resident source adapter. It must establish view/topology and original
// source lifetimes for this borrowed draw; addresses/counts alone do not qualify.
struct PackedMotionIdentityProvider
{
    const void* context = nullptr;
    bool (*resolve)(const void*, ID3D12GraphicsCommandList*, const GeometryDrawView&,
                    const CyberpunkMeshShape&, const GeometryPipelineEntry&, std::uint32_t span,
                    std::uint32_t ordinal, VertexHistoryKey&) noexcept = nullptr;
    explicit operator bool() const { return resolve != nullptr; }
};
struct PackedMotionFrame
{
    ID3D12Resource* resource = nullptr;
    std::uint32_t width = 0, height = 0, frame = 0;
    std::uint64_t fgFrame = UINT64_MAX;
    explicit operator bool() const { return resource && width && height && frame; }
};

struct PackedMotionCaptureStatus
{
    bool initialized = false, registered = false, healthy = false;
    std::uint32_t width = 0, height = 0;
    std::uint64_t admittedDraws = 0, capturedFrames = 0, fgFrames = 0;
    std::uint64_t missingPipeline = 0, unknownIdentity = 0, topologyRejected = 0;
    std::uint64_t mappingOverflow = 0, historyOverflow = 0, slotBusy = 0, orderingRejected = 0;
    // FG-boundary rejection split. noFgFrame counts acquisitions where the native
    // Streamline frame value was absent; noFgQueue counts unknown FG command queue.
    std::uint64_t noFgFrame = 0, noFgQueue = 0;
    // FG boundary acquisition split: no candidate frame matched the
    // submission rule, the pair was stale/backward, several candidates made
    // the batch ambiguous, or the consumer slot was already taken.
    std::uint64_t acquireNoCandidate = 0, acquireStalePair = 0, acquireAmbiguous = 0, acquireConsumerBusy = 0;
    // Per-reason split for the identity and topology rejection counters above.
    std::uint64_t unknownOwnerSpan = 0, unknownResolve = 0, unknownOwnerMismatch = 0;
    std::uint64_t unknownFieldMismatch = 0, unknownNoArrayGeneration = 0;
    std::uint64_t rasterRejected = 0, shapeRejected = 0, viewportRejected = 0;
    // Heaviest rejection chunks (engine draw chunk ids) for attribution.
    struct ChunkCount { std::uint32_t chunk = 0; std::uint64_t count = 0; };
    std::array<ChunkCount, 16> unknownChunks {}, topologyChunks {}, missingChunks {};
    // Vertex history reuse: hits keep a key across frames, inserted means the
    // key changed (no usable previous transform for that element yet).
    std::uint64_t historyHits = 0, historyInserted = 0, historyReclaimed = 0;
    std::uint64_t historyRejectedTopology = 0, historySetFull = 0, historyArenaFull = 0;
    unsigned historyLive = 0;
};

struct PackedMotionProvider
{
    PackedMotionFrame (*acquire)(ID3D12GraphicsCommandList*, std::uint32_t, std::uint32_t,
                                 std::uint64_t, bool) noexcept = nullptr;
    void (*discard)(const void*, bool destroyed) noexcept = nullptr;
    explicit operator bool() const { return acquire && discard; }
};

// One process-resident owner. Initialization is a one-time feature setup, not a
// render callback. Runtime draw admission performs bounded table lookup and
// persistently-mapped upload writes only; it never compiles, allocates or waits.
bool InitializePackedMotionCapture(ID3D12Device* device, std::uint32_t width, std::uint32_t height,
                                   FILE* log = nullptr, PackedMotionIdentityProvider identities = {}) noexcept;
PackedMotionFrame AcquirePackedMotionFrame(ID3D12GraphicsCommandList* fgCommand,
                                           std::uint32_t width, std::uint32_t height,
                                           std::uint64_t fgFrame, bool reset) noexcept;
PackedMotionCaptureStatus ReadPackedMotionCaptureStatus() noexcept;
// Zeroes the diagnostic counters without touching resources or frames.
void ResetPackedMotionCounters() noexcept;
// Pointer identity only, including a destroyed COM object's former identity.
void DiscardPackedMotionRecording(const void* command, bool destroyed) noexcept;
} // namespace GlassFg
