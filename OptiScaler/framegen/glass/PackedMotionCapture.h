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
    // Borrowed producer dependency. The packed raster runs on the graphics
    // queue; the queue that executes the compose must wait on this value
    // before the packed buffer is read.
    ID3D12Fence* producerFence = nullptr;
    std::uint64_t producerValue = 0;
    // Queue that carries the producer submission for this frame. Waiting on the
    // frame generation queue for a value that the same queue will signal later
    // is a self wait, so the host has to be able to compare the two.
    void* producerQueue = nullptr;
    // Draws of this frame that used a native graft variant (engine
    // MotionMatrix previous clip). Counted by prepare for the frame it
    // records into, so it describes exactly this capture.
    std::uint64_t graftDraws = 0;
    explicit operator bool() const { return resource && width && height && frame; }
};

struct PackedMotionCaptureStatus
{
    bool initialized = false, registered = false, healthy = false;
    std::uint32_t width = 0, height = 0;
    std::uint64_t admittedDraws = 0, capturedFrames = 0, fgFrames = 0;
    std::uint64_t missingPipeline = 0, unknownIdentity = 0, topologyRejected = 0;
    // Draws whose packed variant the cache withheld because the audited camera
    // constant pair was absent (F-01). missingPipeline counts them as well; this
    // counter isolates the cause so the size of the pair-less population is
    // readable. Those draws keep the engine's own motion and run no packed
    // raster, so they have no compose pixel counter either.
    std::uint64_t deltaMissingDraws = 0;
    std::uint64_t mappingOverflow = 0, historyOverflow = 0, slotBusy = 0, orderingRejected = 0;
    // Slots whose recorded/consumed command list disappeared without a reset and
    // that the capture reclaimed after both fences completed. Diagnostics only;
    // a growing value means the engine keeps replacing command lists.
    std::uint64_t slotReclaimed = 0;
    // Slots that were old enough to be unreachable by any reader but still
    // pinned the history retirement watermark because their discard/destroyed
    // notification never arrived. Each recovery unblocks history reclamation;
    // a large value next to a frozen historyRetiredFrame means the engine keeps
    // leaving one slot behind per frame.
    std::uint64_t slotRecovered = 0;
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
    std::array<ChunkCount, 16> unknownChunks {}, topologyChunks {}, missingChunks {}, deltaMissingChunks {};
    // Vertex history reuse: hits keep a key across frames, inserted means the
    // key changed (no usable previous transform for that element yet).
    std::uint64_t historyHits = 0, historyInserted = 0, historyReclaimed = 0;
    std::uint64_t historyRejectedTopology = 0, historySetFull = 0, historyArenaFull = 0;
    // Allocations that failed first and succeeded after the bounded reclaim pass.
    std::uint64_t historyArenaReclaimed = 0;
    // Distinct page counts the arena could not serve (largest first is not
    // guaranteed; the values identify one huge mesh versus plain exhaustion).
    std::array<std::uint32_t, 4> historyArenaFullPages {};
    unsigned historyArenaFullPageCount = 0;
    unsigned historyLive = 0;
    // Arena occupancy when the status was read. arenaUsedPages is the page
    // count the live keys hold, arenaLargestFree is the biggest block the
    // buddy allocator can still serve; the pair separates true exhaustion
    // from fragmentation without a per-request log.
    unsigned historyArenaPages = 0, historyArenaUsedPages = 0, historyArenaLargestFree = 0;
    // Reclamation window when the status was read. historyPinnedEntries counts
    // entries the two-frame N-1 rule still protects; if that count is close to
    // historyLive the arena is short of capacity, and if historyRetiredFrame
    // trails historyFrame the watermark is the reason nothing is reclaimed.
    std::uint32_t historyFrame = 0, historyRetiredFrame = 0;
    unsigned historyPinnedEntries = 0;
    // Which engine mesh families reach the packed capture. Per-chunk rejection
    // counters cannot answer "was this material family captured at all", which
    // is the question a missing object (a glass, a railing) raises.
    struct SpanCount
    {
        std::uint32_t chunk = 0, mesh = 0, vertices = 0;
        std::uint64_t count = 0;
    };
    std::array<SpanCount, 8> admittedSpans {};
    unsigned admittedSpanFamilyCount = 0;
    // Families the bounded table could not hold; non-zero means the printed
    // list is incomplete, never that the list is wrong.
    std::uint64_t spanFamilyEvictions = 0;
    // Elements the capture admitted in the frame the family table describes.
    std::uint64_t frameSpanCount = 0;
    // Chunks whose elements resolved an identity but found no arena block.
    std::array<ChunkCount, 16> overflowChunks {};
};

struct PackedMotionProvider
{
    PackedMotionFrame (*acquire)(ID3D12GraphicsCommandList*, std::uint32_t, std::uint32_t,
                                 std::uint64_t, bool) noexcept = nullptr;
    // Second consumer (the DLSS-NR seam): exactly the requested engine frame
    // whose producer submission already exists. No nearest-frame fallback.
    // No frame generation command or token is involved, so the frame generation
    // submit-batch rule does not apply here.
    PackedMotionFrame (*acquireSecondConsumer)(std::uint32_t, std::uint32_t, std::uint64_t) noexcept = nullptr;
    void (*discard)(const void*, bool destroyed) noexcept = nullptr;
    explicit operator bool() const { return acquire && acquireSecondConsumer && discard; }
};

// One process-resident owner. Initialization is a one-time feature setup, not a
// render callback. Runtime draw admission performs bounded table lookup and
// persistently-mapped upload writes only; it never compiles, allocates or waits.
bool InitializePackedMotionCapture(ID3D12Device* device, std::uint32_t width, std::uint32_t height,
                                   FILE* log = nullptr, PackedMotionIdentityProvider identities = {}) noexcept;
// Drops the process-resident capture so a different extent can be built. The
// caller must be the health thread and must have released every session and
// every frame that still references the capture. Returns true when a capture
// was unregistered (the capture stays alive until the draw path stops reading
// the owner slot, so this must not race an active draw callback).
bool ReleasePackedMotionCapture() noexcept;
PackedMotionFrame AcquirePackedMotionFrame(ID3D12GraphicsCommandList* fgCommand,
                                           std::uint32_t width, std::uint32_t height,
                                           std::uint64_t fgFrame, bool reset) noexcept;
// Second consumer selection: exactly engineFrame, already submitted and not
// consumed by FG. Missing, invalid or ambiguous frame identity returns empty.
PackedMotionFrame AcquirePackedMotionFrameForSecondConsumer(std::uint32_t width, std::uint32_t height,
                                                            std::uint64_t engineFrame) noexcept;
PackedMotionCaptureStatus ReadPackedMotionCaptureStatus() noexcept;
// Zeroes the diagnostic counters without touching resources or frames.
void ResetPackedMotionCounters() noexcept;
// Pointer identity only, including a destroyed COM object's former identity.
void DiscardPackedMotionRecording(const void* command, bool destroyed) noexcept;
} // namespace GlassFg
