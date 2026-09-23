#include "pch.h"
#include "PackedMotionCapture.h"
#include "DxilVertexHistory.h"
#include "CyberpunkDraws.h"
#include "GeometryCommands.h"
#include "GeometryDrawCapture.h"
#include "GeometryHealth.h"
#include "GlassControls.h"
#include "MotionFramePair.h"
#include "PackedMotionMappings.h"
#include "PackedMotionSelection.h"
#include "GeometryGateTrace.h"
#include <d3dcompiler.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <limits>
#include <mutex>
#include <new>
#include <thread>

namespace GlassFg
{
namespace
{
using Microsoft::WRL::ComPtr;
constexpr unsigned FrameCount = 3, RecordingCount = 64, FgCommandCount = 4;
// Distance at which a capture slot that still holds a stale recording or FG
// consumer command is treated as unreachable and its bookkeeping dropped, so it
// can no longer pin the history retirement watermark. Well beyond FrameCount
// and the four-frame FG hold-open window below.
constexpr std::uint32_t StaleSlotFrames = 8;
constexpr unsigned MappingCapacity = 16384, ConstantCapacity = 4096;
// Vertex-history arena. 4096 pages x 128 vertices was regularly exhausted in
// live scenes (a single large mesh asks for a contiguous power-of-two block),
// so the arena was doubled once. The live bar scene still fills it: the module
// reported arena_full=361,289 failed reservations with arena_used 8,192/8,192
// pages and ~2,500 live histories at 3.3 pages each, and every failed
// reservation is a surface whose motion vector stays at the engine's value.
// The arena is therefore doubled again. The GPU cost is 2 x HistoryCapacity x
// 32 bytes (128 MB total) and the backing store is a power-of-two buddy
// allocator, so the page count and the vertex capacity have to move together.
constexpr unsigned HistoryPages = 16384, HistoryPageVertices = 128;
constexpr unsigned HistoryCapacity = HistoryPages * HistoryPageVertices;
static_assert(HistoryCapacity == (1u << 21), "history capacity must stay a power of two");
// The draw batch's frame field is a render-context tick that stays zero in some
// configurations; a zero frame number made the capture reject every draw and the
// frame generation side then had no candidate at all. Fall back to the engine
// render frame the command observer tracks, which is the same counter the
// correction uses, and finally to a private monotone counter.
std::uint32_t resolvedDrawFrame(const GeometryDrawView& draw)
{
    // Only this draw's verified packet frame identifies its history slot.
    // A process-wide last draw can belong to another recording; an incrementing
    // fallback invents temporal identity and makes unrelated draws consecutive.
    return draw.frame == UINT32_MAX ? 0 : draw.frame;
}

void checked(HRESULT value, const char* message)
{
    if (FAILED(value))
        throw std::runtime_error(message);
}

ComPtr<ID3D12Resource> buffer(ID3D12Device* device, UINT64 bytes, D3D12_HEAP_TYPE heapType,
                              D3D12_RESOURCE_FLAGS flags = D3D12_RESOURCE_FLAG_NONE,
                              D3D12_RESOURCE_STATES initial = D3D12_RESOURCE_STATE_COMMON)
{
    D3D12_HEAP_PROPERTIES heap {};
    heap.Type = heapType;
    D3D12_RESOURCE_DESC desc {};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Width = bytes;
    desc.Height = 1;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    desc.Flags = flags;
    ComPtr<ID3D12Resource> result;
    const auto state = heapType == D3D12_HEAP_TYPE_UPLOAD ? D3D12_RESOURCE_STATE_GENERIC_READ : initial;
    checked(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, state,
                                             nullptr, IID_PPV_ARGS(&result)), "Packed buffer creation failed");
    return result;
}

// Same live owner, independent of the volatile per-update ticket. The provider
// supplies the registration lifetime in the key generation instead.
bool sameOwner(const GeometryDrawIdentity& a, const GeometryDrawIdentity& b)
{
    return a.proxy == b.proxy && a.mesh == b.mesh && a.slot == b.slot;
}

// Diagnostic lines a capture-mutex holder formats under the mutex and writes
// once the mutex is released: the holder declares it before its lock, so the
// lock is destroyed first. A slow log write, or another thread inside the FILE
// lock, then never stalls a recording thread that waits for the mutex. Fixed
// storage; a line that does not fit is dropped.
struct DeferredLog
{
    FILE* file;
    bool flush = false;
    std::size_t used = 0;
    char text[1024];
    explicit DeferredLog(FILE* target) noexcept : file(target) {}
    DeferredLog(const DeferredLog&) = delete;
    DeferredLog& operator=(const DeferredLog&) = delete;
    ~DeferredLog()
    {
        if (file == nullptr || used == 0)
            return;
        std::fwrite(text, 1, used, file);
        if (flush)
            std::fflush(file);
    }
    void add(const char* format, ...) noexcept
    {
        if (file == nullptr)
            return;
        va_list arguments;
        va_start(arguments, format);
        const int written = std::vsnprintf(text + used, sizeof(text) - used, format, arguments);
        va_end(arguments);
        if (written > 0 && std::size_t(written) < sizeof(text) - used)
            used += std::size_t(written);
    }
};

// Blocking-acquisition diagnostic, measured only while the trace control is on
// (off by default): prepare calls that waited for the capture mutex, their
// total wait and the longest wait since the last report, in QPC ticks. Only a
// contended acquisition with trace on reads the clock.
std::atomic<std::uint64_t> lockWaits { 0 }, lockWaitTicks { 0 }, lockWaitMaxTicks { 0 };
void noteLockWait(std::uint64_t ticks) noexcept
{
    lockWaits.fetch_add(1, std::memory_order_relaxed);
    lockWaitTicks.fetch_add(ticks, std::memory_order_relaxed);
    auto longest = lockWaitMaxTicks.load(std::memory_order_relaxed);
    while (ticks > longest &&
           lockWaitMaxTicks.compare_exchange_weak(longest, ticks, std::memory_order_relaxed) == false)
    {
    }
}

class Capture final : public GeometryDrawCaptureOwner
{
    struct Recording { ID3D12GraphicsCommandList* command = nullptr; std::uint64_t epoch = 0; };
    // Dump id table row (PackedMotionCapture.h): a boundary ID and the constant
    // index of the draw that admitted the element.
    struct DumpId { std::uint16_t id, constant; };
    // Draw variant of a dump id table row, by constant index. Stale: a
    // single-instance root draw that took the camera-only variant under the
    // stale MotionMatrix rule (stalemotion=camera).
    enum DumpVariant : std::uint8_t { DumpVariantHistory, DumpVariantRoot, DumpVariantArray, DumpVariantStale };
    static const char* dumpVariantName(std::uint8_t variant) noexcept
    {
        return variant == DumpVariantRoot    ? "root"
               : variant == DumpVariantArray ? "array"
               : variant == DumpVariantStale ? "stale"
                                             : "history";
    }
    struct Frame
    {
        std::uint32_t number = 0, mappingUsed = 0, constantsUsed = 0;
        // Advances on every (re)assignment of the slot. A draw that admits in
        // several batches (prepare) releases the mutex between them and checks
        // this before its next batch.
        std::uint32_t serial = 0;
        // Dump id table (diagnostics). idsRecording is set when the slot is
        // assigned while a dump request has the table armed, so a recording slot
        // holds every element its frame admitted. Kept beside the counters every
        // admitted element already touches. The rows are ids[0, idCount); the
        // constant index of a row names its draw, whose entry and variant are
        // pipelines[] and idVariants[] at that index.
        bool idsRecording = false;
        std::uint32_t idCount = 0;
        GeometryInstance* mappings = nullptr;
        std::byte* constants = nullptr;
        ComPtr<ID3D12Resource> mapping, constantBuffer, capture;
        // GPU addresses of the three buffers above, which live as long as the
        // capture; prepare binds them for every admitted draw.
        D3D12_GPU_VIRTUAL_ADDRESS mappingAddress = 0, constantAddress = 0, captureAddress = 0;
        ComPtr<ID3D12CommandAllocator> clearAllocator;
        ComPtr<ID3D12GraphicsCommandList> clearCommand;
        std::array<Recording, RecordingCount> recordings {};
        std::array<std::shared_ptr<const GeometryPipelineEntry>, ConstantCapacity> pipelines;
        ID3D12GraphicsCommandList* consumerCommand = nullptr;
        ComPtr<ID3D12CommandQueue> producerQueue, consumerQueue, orderedQueue;
        ComPtr<ID3D12Fence> syncFence;
        std::uint64_t producerValue = 0, consumerValue = 0, syncValue = 0;
        // Monotonic submission sequence, used to correlate a producer frame with
        // the FG command that was submitted after it.
        std::uint64_t submitOrder = 0;
        // Submission batch index (one per ExecuteCommandLists call). Items in
        // the same batch keep the same value, so a frame recorded on the same
        // command list as the FG call is still admitted.
        std::uint64_t submitBatch = 0;
        // Draws recorded into this frame that used a native graft variant.
        std::uint64_t graftDraws = 0;
        // Dump id table rows and per-draw variants (idsRecording above).
        std::array<DumpId, MappingCapacity> ids;
        std::array<std::uint8_t, ConstantCapacity> idVariants;
        bool clearSubmitted = false, syncPending = false;
    };
    struct FgCommand
    {
        ID3D12GraphicsCommandList* command = nullptr;
        ComPtr<ID3D12CommandQueue> queue;
        std::uint64_t submitOrder = 0;
        std::uint64_t submitBatch = 0;
    };
    // Engine-owned Signal/Wait correlation. The orderedQueue condition must not
    // depend on this capture's private fence, which nothing in the engine waits on.
    struct ObservedSignal
    {
        ComPtr<ID3D12Fence> fence;
        ComPtr<ID3D12CommandQueue> queue;
        std::uint64_t value = 0, order = 0;
    };

    ComPtr<ID3D12Device> device;
    ComPtr<ID3D12RootSignature> clearRoot;
    ComPtr<ID3D12PipelineState> clearPipeline;
    ComPtr<ID3D12Resource> history[2];
    D3D12_GPU_VIRTUAL_ADDRESS historyAddress[2] {};
    ComPtr<ID3D12CommandAllocator> historyClearAllocator;
    ComPtr<ID3D12GraphicsCommandList> historyClearCommand;
    ComPtr<ID3D12CommandQueue> historyQueue;
    ComPtr<ID3D12Fence> producerFence, consumerFence;
    std::array<Frame, FrameCount> frames;
    PackedMotionMappings<4096, 4, HistoryPages, HistoryPageVertices> objectMappings;
    PackedMotionIdentityProvider identitySource;
    std::array<FgCommand, FgCommandCount> fgCommands;
    std::array<ObservedSignal, 256> observedSignals {};
    unsigned nextObservedSignal = 0;
    std::uint64_t submitSequence = 0;
    std::uint64_t submitBatchSequence = 0;
    // Diagnostic only: module log and the last frame that produced a replay
    // trace line, so the line appears once per captured frame.
    FILE* log = nullptr;
    std::atomic<std::uint32_t> lastReplayFrame { 0 };
    // Bounded attribution for the admission gates. A scene that never captures
    // a surface has to leave enough detail to name the gate without turning the
    // per-draw path into a logger. prepare writes these lines outside the
    // capture mutex, so the budgets are atomic.
    static constexpr unsigned GateDetailLimit = 240, GateAdmitLimit = 64;
    std::atomic<unsigned> gateDetailLines { 0 }, gateAdmitLines { 0 };
    // Bounded log for the stale-slot recovery in beginMappings (diagnostics).
    unsigned staleSlotReports = 0;
    // Motion probe (motionprobe=<hex>, probeMotion). Every probed draw updates
    // the per-proxy track and the totals; the first ProbeDrawsPerWindow draws of
    // each two-second window also print their MOTION_PROBE lines. The track
    // holds the INSTANCE_TRANSFORM a proxy's draws used in the last two render
    // frames it was seen, so a draw's MotionMatrix rows can be compared with
    // the transform the proxy was drawn with one frame earlier. Fixed storage;
    // probeMutex is taken only by probed draws.
    struct ProbeTrack
    {
        std::uint64_t proxy = 0;
        std::uint32_t frame = 0, earlierFrame = 0;
        std::array<std::uint32_t, 12> instance {}, earlier {};
    };
    static constexpr unsigned ProbeTrackCount = 32, ProbeDrawsPerWindow = 8;
    static constexpr std::uint64_t ProbeWindowMs = 2000;
    std::mutex probeMutex;
    std::array<ProbeTrack, ProbeTrackCount> probeTracks {};
    unsigned probeTrackCursor = 0;
    std::atomic<std::uint64_t> probeWindow { 0 };
    std::atomic<unsigned> probeLines { 0 };
    enum ProbeTotal : unsigned
    {
        ProbeDraws,
        ProbePrinted,
        ProbeRowsNone,
        ProbeRowsCurrent,
        ProbeRowsPrevious,
        ProbeRowsOther,
        ProbeInstanceOther,
        ProbeSupplied,
        ProbeStale,
        ProbeEarlierMatch,
        ProbeEarlierMiss,
        ProbeTotalCount
    };
    std::array<std::atomic<std::uint64_t>, ProbeTotalCount> probeTotals {};
    // The capture mutex. It guards the frame slots and their upload memory, the
    // object mappings, the submission and fence bookkeeping, the dump id table,
    // `counters`, `overflowChunks` and the span family table. Every recording
    // thread's prepare shares it, so prepare holds it only for one element
    // batch of a draw: slot and mapping updates, the reservation and the upload
    // writes. Identity resolution, the draw checks and all logging run outside.
    // A busy mutex is waited for, never a reason to drop a draw; the longest
    // holders are listed at status().
    mutable std::mutex mutex;
    PackedMotionCaptureStatus counters;
    std::array<PackedMotionCaptureStatus::ChunkCount, 16> overflowChunks {};
    // Refusals that prepare counts before it would take the capture mutex:
    // pipeline, raster, shape, viewport and identity. A refused draw holds this
    // lock for one chunk table update and never waits for the capture mutex.
    struct Rejections
    {
        std::uint64_t missingPipeline = 0, deltaMissingDraws = 0;
        std::uint64_t topologyRejected = 0, rasterRejected = 0, shapeRejected = 0, viewportRejected = 0;
        std::uint64_t unknownIdentity = 0, unknownOwnerSpan = 0, unknownResolve = 0, unknownOwnerMismatch = 0;
        std::uint64_t unknownFieldMismatch = 0, unknownNoArrayGeneration = 0;
        std::array<PackedMotionCaptureStatus::ChunkCount, 16> unknownChunks {}, topologyChunks {}, missingChunks {};
        // Pair-less packed variants the cache withheld: the draw keeps the
        // engine's own motion, and this table names the engine chunks that lost
        // coverage because of it (F-01).
        std::array<PackedMotionCaptureStatus::ChunkCount, 16> deltaMissingChunks {};
    };
    std::mutex rejectionMutex;
    Rejections rejections;
    // Recorded draws (finish). Atomic, so finish takes no lock.
    std::atomic<std::uint64_t> admittedDraws { 0 };
    // Bounded family table for the packed capture. A four-probe hash window
    // keeps the per-span cost at a few comparisons; a family that cannot be
    // placed is counted, never silently merged into another mesh.
    struct SpanFamily
    {
        std::uint32_t chunk = 0, mesh = 0, vertices = 0;
        std::uint64_t count = 0;
    };
    static constexpr unsigned SpanFamilyCount = 256, SpanFamilyProbes = 8;
    std::array<SpanFamily, SpanFamilyCount> spanFamilies {};
    std::uint64_t spanFamilyEvictions = 0;
    std::uint64_t frameSpanCount = 0;
    std::uint32_t configuredWidth = 0, configuredHeight = 0;
    std::uint64_t nextProducer = 0, nextConsumer = 0;
    MotionFramePair framePair;
    // Sticky capture failure. Written under the mutex; prepare also reads it
    // before it takes the mutex.
    std::atomic<bool> failed { false };
    bool historyClearSubmitted = false;
    // Dump id table state (PackedMotionCapture.h), under mutex. idsArmed means a
    // dump request asked for a table and no frame has been frozen for it yet.
    // Slots assigned meanwhile record. idsArmedFrames ends the recording when the
    // compose stops asking (session retired, dump aborted). idsDeferred counts
    // the composes this request has held back. A complete table normally
    // needs one or two engine frames. Under multi-frame generation each
    // engine frame is composed up to four times, so 32 composes cover about
    // eight engine frames.
    static constexpr unsigned DumpIdMaxDeferred = 32, DumpIdArmedFrames = 64;
    bool idsArmed = false;
    unsigned idsArmedFrames = 0, idsDeferred = 0;
    struct DumpIdPipeline
    {
        std::uint64_t identity = 0, vertexHash = 0, pixelHash = 0;
        GeometryGraftKind kind = GeometryGraftKind::Pending;
        std::uint8_t variant = DumpVariantHistory;
    };
    // The frozen table of the frame being dumped. It holds plain copies, so the
    // slot can be reused before the dump writer runs.
    struct DumpIdSnapshot
    {
        std::uint32_t frame = 0, count = 0, pipelines = 0, deferred = 0;
        bool valid = false, complete = false;
        std::array<DumpId, MappingCapacity> ids;
        std::array<DumpIdPipeline, ConstantCapacity> pipeline;
    } idSnapshot;

    // Adds `count` refusals of one chunk, exactly as that many single calls
    // would: the first places the chunk (or replaces the smallest entry) and the
    // rest find it.
    static void noteChunk(std::array<PackedMotionCaptureStatus::ChunkCount, 16>& list, std::uint32_t chunk,
                          std::uint64_t count = 1) noexcept
    {
        if (!chunk || !count) return;
        auto* smallest = &list[0];
        for (auto& entry : list)
        {
            if (entry.chunk == chunk)
            {
                entry.count += count;
                return;
            }
            if (entry.count < smallest->count)
                smallest = &entry;
        }
        smallest->chunk = chunk;
        smallest->count = count;
    }

    // Counts `elements` admitted elements of one family, exactly as that many
    // single calls would: the first places the family (or is counted as an
    // eviction) and the rest find the same entry, because nothing else writes
    // the table in between.
    void noteSpanFamily(std::uint32_t chunk, std::uint32_t mesh, std::uint32_t vertices,
                        std::uint32_t elements) noexcept
    {
        if (!chunk || !mesh) return;
        const auto base = unsigned(((std::uint64_t(mesh) * 0x9E3779B97F4A7C15ull) ^
                                    (std::uint64_t(chunk) << 7)) & (SpanFamilyCount - 1));
        SpanFamily* free = nullptr;
        for (unsigned i = 0; i < SpanFamilyProbes; ++i)
        {
            auto& entry = spanFamilies[(base + i) & (SpanFamilyCount - 1)];
            if (entry.count)
            {
                if (entry.chunk == chunk && entry.mesh == mesh)
                {
                    entry.count += elements;
                    return;
                }
                continue;
            }
            if (!free) free = &entry;
        }
        if (!free)
        {
            spanFamilyEvictions += elements;
            return;
        }
        *free = { chunk, mesh, vertices, elements };
    }

    bool completed(ID3D12Fence* fence, std::uint64_t value)
    {
        if (!value) return true;
        const auto done = fence->GetCompletedValue();
        if (done == std::numeric_limits<std::uint64_t>::max()) failed = true;
        return done != std::numeric_limits<std::uint64_t>::max() && done >= value;
    }
    bool reusable(Frame& frame)
    {
        if (!frame.number) return true;
        for (const auto& recording : frame.recordings)
            if (recording.command) return false;
        // A frame whose draws were captured is the candidate the frame generation
        // evaluation consumes. Recycling it as soon as the draw recordings were
        // discarded left the FG side with no candidate at all
        // (acquire_no_candidate with every slot empty). Hold it until the FG call
        // consumes it, with a bounded margin so the pool cannot be pinned.
        if (frame.producerValue && !frame.consumerCommand)
        {
            const auto newest = newestFrameNumber();
            if (newest == 0 || frame.number + 4 >= newest)
                return false;
        }
        return !frame.consumerCommand && completed(producerFence.Get(), frame.producerValue) &&
               completed(consumerFence.Get(), frame.consumerValue);
    }
    // Both fences past this slot's last submitted work: nothing can still be
    // reading or writing its capture, so its stale bookkeeping is droppable.
    bool fencesComplete(const Frame& frame)
    {
        return completed(producerFence.Get(), frame.producerValue) &&
               completed(consumerFence.Get(), frame.consumerValue);
    }
    std::uint32_t newestFrameNumber() const
    {
        std::uint32_t newest = 0;
        for (const auto& value : frames)
            if (value.number > newest)
                newest = value.number;
        return newest;
    }
    bool beginMappings(std::uint32_t number, DeferredLog& deferred)
    {
        if (objectMappings.frame() == number) return true;
        if (!number || number < objectMappings.frame()) return false;
        auto completedThrough = number - 1;
        // Every owned recording remains in these slots until both recording
        // discard and GPU completion. Gaps with no owned work need no retirement.
        for (auto& value : frames)
        {
            if (!value.number || value.number > completedThrough || reusable(value))
                continue;
            // A slot whose discard/destroyed notification never arrives keeps a
            // non-null recording (or a stale FG consumer command) forever, and
            // the line below would then pin the history retirement watermark at
            // that frame: every history entry at or above it stays
            // unreclaimable, the vertex arena fills to capacity, each later
            // reservation fails and the transparent-object correction silently
            // stops covering the scene. Measured live 2026-09-16: retired frozen
            // at 71612 while frame reached 79161, arena 16384/16384 pages used
            // with largestFreePages 0, covered pixels down from 6 % to 0.09 %,
            // and the correction stopped for the rest of the session.
            // The pool is FrameCount slots deep and the FG hold-open window is
            // four frames, so a slot this far behind with both fences complete
            // cannot be read by any in-flight work. Dropping its stale
            // bookkeeping is the same recovery frame() already performs.
            if (number - value.number >= StaleSlotFrames && fencesComplete(value))
            {
                value.recordings = {};
                value.consumerCommand = nullptr;
                ++counters.slotRecovered;
                if (log != nullptr && staleSlotReports < 8)
                {
                    ++staleSlotReports;
                    deferred.add("PACKED_SLOT_RECOVER frame=%u stale=%u gap=%u\n", number, value.number,
                                 number - value.number);
                    deferred.flush = true;
                }
                continue;
            }
            completedThrough = value.number - 1;
        }
        // Diagnostic (jitterLog only): record-tag validity for the frame that
        // just finished. A key that kept its history entry reuses its arena
        // slot, so the tag the next capture reads there is (frame-1, generation)
        // and the previous transform is admitted; a key inserted this frame
        // finds a stale or foreign tag and falls back to the current values.
        // The counts are cumulative, so the per-frame value is the difference
        // between consecutive lines, and `spans` is this frame's admitted
        // element count. The line joins with the compose line of the same frame
        // number, which carries the delivered jitter pair.
        if (log != nullptr && ReadControls().jitterLog)
        {
            static std::atomic<unsigned> markerLines { 0 };
            const auto& stats = objectMappings.historyStats();
            deferred.add("PACKED_CAPTURE_FRAME frame=%u next=%u hits=%llu inserted=%llu spans=%llu evictions=%llu "
                         "live=%u\n",
                         objectMappings.historyFrame(), number, static_cast<unsigned long long>(stats.hits),
                         static_cast<unsigned long long>(stats.inserted),
                         static_cast<unsigned long long>(frameSpanCount),
                         static_cast<unsigned long long>(spanFamilyEvictions), objectMappings.liveHistories());
            if ((markerLines.fetch_add(1, std::memory_order_relaxed) & 31u) == 31u)
                deferred.flush = true;
        }
        if (failed || !objectMappings.beginFrame(number, completedThrough)) return false;
        // The family table describes the frame that is being captured now; a
        // cumulative table would be dominated by earlier scenes and would hide
        // the object the current camera actually looks at.
        spanFamilies = {};
        spanFamilyEvictions = 0;
        frameSpanCount = 0;
        return true;
    }
    // A newly assigned slot records its dump ids when the table is armed at
    // its first draw (PackedMotionCapture.h). The armed state ends on its own
    // if the compose stopped asking for a table.
    void assignIds(Frame& value)
    {
        value.idCount = 0;
        if (idsArmed && ++idsArmedFrames > DumpIdArmedFrames)
            idsArmed = false;
        value.idsRecording = idsArmed;
    }
    Frame* frame(std::uint32_t number)
    {
        for (auto& value : frames)
            if (value.number == number) return &value;
        for (auto& value : frames)
            if (reusable(value))
            {
                value.number = number;
                ++value.serial;
                for (unsigned i = 0; i < value.constantsUsed; ++i) value.pipelines[i].reset();
                value.mappingUsed = value.constantsUsed = 0;
                value.recordings = {};
                value.consumerCommand = nullptr;
                value.producerQueue.Reset(); value.consumerQueue.Reset(); value.orderedQueue.Reset();
                value.syncFence.Reset();
                value.producerValue = value.consumerValue = value.syncValue = value.graftDraws = 0;
                value.clearSubmitted = value.syncPending = false;
                assignIds(value);
                return &value;
            }
        // Stale-slot reclaim. A recorded render list that is never reset again,
        // or an FG list that was replaced without a destroyed notification,
        // otherwise pins its slot forever and the three-slot pool drains. Both
        // fences completing means nothing can still be reading or writing that
        // capture, so dropping the stale bookkeeping cannot race in-flight work.
        for (auto& value : frames)
        {
            if (!value.number || value.number >= number) continue;
            if (!completed(producerFence.Get(), value.producerValue) ||
                !completed(consumerFence.Get(), value.consumerValue))
                continue;
            ++counters.slotReclaimed;
            value.number = number;
            ++value.serial;
            for (unsigned i = 0; i < value.constantsUsed; ++i) value.pipelines[i].reset();
            value.mappingUsed = value.constantsUsed = 0;
            value.recordings = {};
            value.consumerCommand = nullptr;
            value.producerQueue.Reset(); value.consumerQueue.Reset(); value.orderedQueue.Reset();
            value.syncFence.Reset();
            value.producerValue = value.consumerValue = value.syncValue = value.graftDraws = 0;
            value.clearSubmitted = value.syncPending = false;
            assignIds(value);
            return &value;
        }
        ++counters.slotBusy;
        return nullptr;
    }
    static bool contains(const Frame& frame, ID3D12CommandList* command)
    {
        for (const auto& value : frame.recordings)
            if (value.command == command) return true;
        return false;
    }
    bool record(Frame& value, ID3D12GraphicsCommandList* command, std::uint64_t epoch)
    {
        for (const auto& entry : value.recordings)
            if (entry.command == command) return entry.epoch == epoch;
        for (const auto& other : frames)
            if (&other != &value && contains(other, command)) return false;
        for (auto& entry : value.recordings)
            if (!entry.command)
            {
                entry = { command, epoch };
                return true;
            }
        return false;
    }
    void compileClear()
    {
        static constexpr char shader[] = R"(
RWByteAddressBuffer Target : register(u0);
cbuffer Constants : register(b0) { uint Words; uint GroupsX; };
[numthreads(256,1,1)] void Clear(uint3 id : SV_DispatchThreadID)
{
    uint index = (id.y * GroupsX * 256) + id.x;
    if (index < Words) Target.Store(index * 4, 0);
})";
        ComPtr<ID3DBlob> code, errors;
        checked(D3DCompile(shader, sizeof(shader) - 1, "packed-clear.hlsl", nullptr, nullptr, "Clear", "cs_5_0",
                           D3DCOMPILE_OPTIMIZATION_LEVEL3 | D3DCOMPILE_ENABLE_STRICTNESS, 0, &code, &errors),
                "Packed clear shader compilation failed");
        D3D12_ROOT_PARAMETER parameters[2] {};
        parameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
        parameters[0].Descriptor = { 0, 0 };
        parameters[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        parameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        parameters[1].Constants = { 0, 0, 2 };
        parameters[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        D3D12_ROOT_SIGNATURE_DESC rootDesc { 2, parameters, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_NONE };
        ComPtr<ID3DBlob> serialized;
        checked(D3D12SerializeRootSignature(&rootDesc, D3D_ROOT_SIGNATURE_VERSION_1, &serialized, &errors),
                "Packed clear root serialization failed");
        checked(device->CreateRootSignature(0, serialized->GetBufferPointer(), serialized->GetBufferSize(),
                                            IID_PPV_ARGS(&clearRoot)), "Packed clear root creation failed");
        D3D12_COMPUTE_PIPELINE_STATE_DESC desc {};
        desc.pRootSignature = clearRoot.Get();
        desc.CS = { code->GetBufferPointer(), code->GetBufferSize() };
        checked(device->CreateComputePipelineState(&desc, IID_PPV_ARGS(&clearPipeline)),
                "Packed clear pipeline creation failed");
    }
    void recordClear(ID3D12Resource* target, UINT64 words, ComPtr<ID3D12CommandAllocator>& allocator,
                     ComPtr<ID3D12GraphicsCommandList>& command)
    {
        checked(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)),
                "Packed clear allocator creation failed");
        checked(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), clearPipeline.Get(),
                                          IID_PPV_ARGS(&command)), "Packed clear command creation failed");
        const UINT64 groups = (words + 255) / 256;
        const UINT groupsX = UINT((std::min)(groups, UINT64(65535)));
        const UINT groupsY = UINT((groups + groupsX - 1) / groupsX);
        if (!groupsX || groupsY > 65535) throw std::runtime_error("Packed clear dispatch exceeds D3D12 limits");
        const UINT values[] { UINT(words), groupsX };
        command->SetComputeRootSignature(clearRoot.Get());
        command->SetComputeRootUnorderedAccessView(0, target->GetGPUVirtualAddress());
        command->SetComputeRoot32BitConstants(1, 2, values, 0);
        command->Dispatch(groupsX, groupsY, 1);
        D3D12_RESOURCE_BARRIER barrier {};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
        barrier.UAV.pResource = target;
        command->ResourceBarrier(1, &barrier);
        checked(command->Close(), "Packed clear command close failed");
    }
    void recordHistoryClear()
    {
        checked(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&historyClearAllocator)),
                "History clear allocator creation failed");
        checked(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, historyClearAllocator.Get(),
                                          clearPipeline.Get(), IID_PPV_ARGS(&historyClearCommand)),
                "History clear command creation failed");
        // 16 Mi words needs 65,536 thread groups, which is one past the
        // 65,535 limit of a single dimension, so the clear is dispatched in
        // two dimensions exactly like the packed capture clear.
        const UINT words = HistoryCapacity * 8;
        const UINT64 groups = (UINT64(words) + 255) / 256;
        const UINT groupsX = UINT((std::min)(groups, UINT64(65535)));
        const UINT groupsY = UINT((groups + groupsX - 1) / groupsX);
        if (!groupsX || groupsY > 65535)
            throw std::runtime_error("History clear dispatch exceeds D3D12 limits");
        for (auto& target : history)
        {
            const UINT values[] { words, groupsX };
            historyClearCommand->SetComputeRootSignature(clearRoot.Get());
            historyClearCommand->SetComputeRootUnorderedAccessView(0, target->GetGPUVirtualAddress());
            historyClearCommand->SetComputeRoot32BitConstants(1, 2, values, 0);
            historyClearCommand->Dispatch(groupsX, groupsY, 1);
            D3D12_RESOURCE_BARRIER barrier {};
            barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
            barrier.UAV.pResource = target.Get();
            historyClearCommand->ResourceBarrier(1, &barrier);
        }
        checked(historyClearCommand->Close(), "History clear command close failed");
    }

  public:
    Capture(ID3D12Device* value, std::uint32_t width, std::uint32_t height, PackedMotionIdentityProvider source,
            FILE* logFile = nullptr)
        : device(value), identitySource(source)
    {
        if (!identitySource) throw std::invalid_argument("Verified motion identity source unavailable");
        if (!value || !width || !height || width > 32768 || height > 32768 ||
            std::uint64_t(width) * height > UINT32_MAX / 8)
            throw std::invalid_argument("Invalid packed capture extent");
        configuredWidth = width; configuredHeight = height;
        log = logFile;
        compileClear();
        history[0] = buffer(device.Get(), UINT64(HistoryCapacity) * 32, D3D12_HEAP_TYPE_DEFAULT,
                            D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
        history[1] = buffer(device.Get(), UINT64(HistoryCapacity) * 32, D3D12_HEAP_TYPE_DEFAULT,
                            D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
        historyAddress[0] = history[0]->GetGPUVirtualAddress();
        historyAddress[1] = history[1]->GetGPUVirtualAddress();
        checked(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&producerFence)),
                "Packed producer fence creation failed");
        checked(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&consumerFence)),
                "Packed consumer fence creation failed");
        const UINT64 pixels = UINT64(width) * height;
        for (auto& frame : frames)
        {
            frame.mapping = buffer(device.Get(), UINT64(MappingCapacity) * sizeof(GeometryInstance),
                                   D3D12_HEAP_TYPE_UPLOAD);
            frame.constantBuffer = buffer(device.Get(), UINT64(ConstantCapacity) * 256, D3D12_HEAP_TYPE_UPLOAD);
            // The compose reads this raster as a UAV on the frame generation
            // queue while the capture writes it on the graphics queue. The
            // simultaneous-access flag would legalise that overlap, but this
            // driver rejects the flag for every combination
            // (D3D12CreateDevice probe: E_INVALIDARG), so the overlap has to be
            // removed by submitting the compose on the producer queue instead.
            frame.capture = buffer(device.Get(), pixels * 8, D3D12_HEAP_TYPE_DEFAULT,
                                   D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
                                   D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            D3D12_RANGE noRead { 0, 0 };
            checked(frame.mapping->Map(0, &noRead, reinterpret_cast<void**>(&frame.mappings)),
                    "Packed mapping map failed");
            checked(frame.constantBuffer->Map(0, &noRead, reinterpret_cast<void**>(&frame.constants)),
                    "Packed constants map failed");
            frame.mappingAddress = frame.mapping->GetGPUVirtualAddress();
            frame.constantAddress = frame.constantBuffer->GetGPUVirtualAddress();
            frame.captureAddress = frame.capture->GetGPUVirtualAddress();
            recordClear(frame.capture.Get(), pixels * 2, frame.clearAllocator, frame.clearCommand);
        }
        recordHistoryClear();
        counters.initialized = counters.healthy = true;
        counters.width = width; counters.height = height;
    }

  private:
    // One element of a draw. prepare resolves it outside the capture mutex and
    // admits it under the mutex.
    struct Element
    {
        VertexHistoryKey key;
        // Mapping slot in the draw's range: span.first + ordinal.
        std::uint32_t index;
        // Set under the mutex; empty unless the element was admitted.
        PackedMotionAllocation allocation;
    };
    // Elements a draw admits per hold of the capture mutex. The largest draw in
    // the live gate logs (2026-09-24) had 35 elements, so a draw normally takes
    // the mutex once; a larger one takes it once per batch, which bounds a hold
    // at 64 table lookups and 64 upload records.
    static constexpr unsigned ElementBatch = 64;
    // Identity refusals of one draw, in GEOMETRY_PACKED_SPLIT order. Counted
    // without a lock and published once per draw.
    enum SpanRefusal : unsigned
    {
        SpanOwner,
        SpanResolve,
        SpanOwnerMismatch,
        SpanFieldMismatch,
        SpanNoArrayGeneration,
        SpanRefusalCount
    };
    static constexpr GeometryPipelineEntry::CoverageGate SpanRefusalGate[SpanRefusalCount] {
        GeometryPipelineEntry::GateSpanOwner, GeometryPipelineEntry::GateSpanResolve,
        GeometryPipelineEntry::GateSpanMismatch, GeometryPipelineEntry::GateSpanField,
        GeometryPipelineEntry::GateSpanArray };

    // Claims one line of a bounded detail budget. The plain load keeps an
    // exhausted budget from counting further on every draw.
    static bool claimLine(std::atomic<unsigned>& used, unsigned limit) noexcept
    {
        return used.load(std::memory_order_relaxed) < limit && used.fetch_add(1, std::memory_order_relaxed) < limit;
    }
    // One packed 3x4 transform (CyberpunkDraws.h CyberpunkMotionSample) as
    // "m00,m01,m02,T0;m10,m11,m12,T1;m20,m21,m22,T2": floats as %.9g, the
    // integer translation words in 1/131072 m. "-" when it was not read.
    static void formatTransform(char* out, std::size_t size, const std::array<std::uint32_t, 12>& value,
                                bool read) noexcept
    {
        if (!read)
        {
            std::snprintf(out, size, "-");
            return;
        }
        const auto f = [&](unsigned i) { return double(std::bit_cast<float>(value[i])); };
        const auto t = [&](unsigned i) { return int(std::int32_t(value[i])); };
        std::snprintf(out, size, "%.9g,%.9g,%.9g,%d;%.9g,%.9g,%.9g,%d;%.9g,%.9g,%.9g,%d", f(0), f(1), f(2), t(3), f(4),
                      f(5), f(6), t(7), f(8), f(9), f(10), t(11));
    }
    // Motion probe of one draw of the selected pipeline (motionprobe=<hex>,
    // GlassControls.h), diagnostics only. It reads what the engine's
    // MotionMatrix supply gave the draw (ReadCyberpunkMotionSample): the rows
    // 24..26 its flush uploaded to b7, the draw's INSTANCE_TRANSFORM and the
    // owner proxy's transform and history record. The rows are classified
    // against the proxy's transform and history pose, and against the transform
    // the proxy was drawn with one render frame earlier. Guarded reads, fixed
    // storage and stack buffers only. Called outside the capture mutex.
    void probeMotion(const GeometryDrawView& draw, const GeometryIndexedArguments& args,
                     const GeometryPipelineEntry& pipeline, const GeometryBatchSpan* span, const char* variant,
                     bool stale) noexcept
    {
        const auto bump = [this](ProbeTotal total)
        { probeTotals[total].fetch_add(1, std::memory_order_relaxed); };
        const std::uint64_t proxy = span ? (span->identity ? span->identity.proxy : span->parent.proxy) : 0;
        CyberpunkMotionSample sample;
        const bool sampled = ReadCyberpunkMotionSample(proxy, span ? span->first : 0, sample);
        bump(ProbeDraws);
        // rows: cur = the proxy's current transform (no previous pose supplied),
        // prev = its history pose, other = neither. inst: the draw's
        // INSTANCE_TRANSFORM against the proxy's transform.
        const char* rows = !sample.rowsRead ? "none"
                           : sample.currentRead && sample.rows == sample.current ? "cur"
                           : sample.previousRead && sample.rows == sample.previous ? "prev"
                                                                                  : "other";
        const char* instance = !sample.instanceRead                                    ? "none"
                               : sample.currentRead && sample.instance == sample.current ? "cur"
                                                                                        : "other";
        bump(!sample.rowsRead           ? ProbeRowsNone
             : rows[0] == 'c'           ? ProbeRowsCurrent
             : rows[0] == 'p'           ? ProbeRowsPrevious
                                        : ProbeRowsOther);
        if (sample.instanceRead && instance[0] == 'o')
            bump(ProbeInstanceOther);
        if (sample.historyRead && sample.history.supplied())
            bump(ProbeSupplied);
        if (stale)
            bump(ProbeStale);
        // earlier: 1 when the rows equal the INSTANCE_TRANSFORM this proxy was
        // drawn with in the previous render frame, 0 when they differ, -1
        // without a sample of that frame.
        int earlier = -1;
        if (sampled && proxy && sample.instanceRead)
        {
            std::lock_guard lock(probeMutex);
            ProbeTrack* track = nullptr;
            for (auto& entry : probeTracks)
                if (entry.proxy == proxy)
                {
                    track = &entry;
                    break;
                }
            if (!track)
            {
                track = &probeTracks[probeTrackCursor++ % ProbeTrackCount];
                *track = {};
                track->proxy = proxy;
            }
            if (track->frame != sample.frame)
            {
                track->earlierFrame = track->frame;
                track->earlier = track->instance;
                track->frame = sample.frame;
                track->instance = sample.instance;
            }
            if (sample.rowsRead && track->earlierFrame && track->earlierFrame + 1 == sample.frame)
                earlier = track->earlier == sample.rows ? 1 : 0;
        }
        if (earlier >= 0)
            bump(earlier ? ProbeEarlierMatch : ProbeEarlierMiss);
        if (!log)
            return;
        // The first probed draw of a new window writes the totals since arming.
        const auto window = GetTickCount64() / ProbeWindowMs;
        auto seen = probeWindow.load(std::memory_order_relaxed);
        const bool summary =
            seen != window && probeWindow.compare_exchange_strong(seen, window, std::memory_order_relaxed);
        if (summary)
            probeLines.store(0, std::memory_order_relaxed);
        const bool detail = claimLine(probeLines, ProbeDrawsPerWindow);
        if (!summary && !detail)
            return;
        char line[2048];
        _lock_file(log);
        if (summary)
        {
            const auto total = [this](ProbeTotal value)
            { return static_cast<unsigned long long>(probeTotals[value].load(std::memory_order_relaxed)); };
            const auto digits = MotionProbeDigits();
            std::snprintf(line, sizeof(line),
                          "MOTION_PROBE_SUM prefix=%0*llx draws=%llu printed=%llu rows_none=%llu rows_cur=%llu "
                          "rows_prev=%llu rows_other=%llu inst_other=%llu supplied=%llu stale=%llu "
                          "earlier_match=%llu earlier_miss=%llu stalemotion=%s\n",
                          int(digits ? digits : 1),
                          static_cast<unsigned long long>(
                              digits ? MotionProbePrefixValue().load(std::memory_order_relaxed) >> (64 - 4 * digits)
                                     : 0),
                          total(ProbeDraws), total(ProbePrinted), total(ProbeRowsNone), total(ProbeRowsCurrent),
                          total(ProbeRowsPrevious), total(ProbeRowsOther), total(ProbeInstanceOther),
                          total(ProbeSupplied), total(ProbeStale), total(ProbeEarlierMatch), total(ProbeEarlierMiss),
                          StaleMotionCameraEnabled() ? "camera" : "off");
            std::fputs(line, log);
        }
        if (detail)
        {
            bump(ProbePrinted);
            // Rows minus INSTANCE_TRANSFORM: translation in mm and the largest
            // rotation/scale entry difference. Both zero when the rows carry no
            // motion for this draw.
            double dt[3] {};
            double dr = 0.0;
            const bool compared = sample.rowsRead && sample.instanceRead;
            if (compared)
                for (unsigned row = 0; row < 3; ++row)
                {
                    dt[row] = double(std::int64_t(std::int32_t(sample.rows[row * 4 + 3])) -
                                     std::int64_t(std::int32_t(sample.instance[row * 4 + 3]))) *
                              (1000.0 / 131072.0);
                    for (unsigned column = 0; column < 3; ++column)
                        dr = (std::max)(dr, std::fabs(double(std::bit_cast<float>(sample.rows[row * 4 + column])) -
                                                      double(std::bit_cast<float>(sample.instance[row * 4 + column]))));
                }
            char state[8] = "-";
            if (sample.historyRead && sample.history.record)
                std::snprintf(state, sizeof(state), "%u", unsigned(sample.history.state));
            std::snprintf(line, sizeof(line),
                          "MOTION_PROBE frame=%u vs=%016llx ps=%016llx pipeline=%llu chunk=%u instances=%u spans=%zu "
                          "proxy=%llx variant=%s rows=%s inst=%s record=%d state=%s weight=%u flags=0x%02x "
                          "own=%d supplied=%d earlier=%d dt_mm=%.3f,%.3f,%.3f dr=%.3g\n",
                          sample.frame, static_cast<unsigned long long>(pipeline.vertexHash),
                          static_cast<unsigned long long>(pipeline.pixelHash),
                          static_cast<unsigned long long>(pipeline.identity), draw.chunk, args.instances,
                          draw.objects.size(), static_cast<unsigned long long>(proxy), variant, rows, instance,
                          sample.historyRead ? (sample.history.record ? 1 : 0) : -1, state,
                          unsigned(sample.history.weight), unsigned(sample.history.flags),
                          int(sample.ownHistory), sample.historyRead && sample.history.supplied() ? 1 : 0, earlier,
                          dt[0], dt[1], dt[2], compared ? dr : -1.0);
            std::fputs(line, log);
            char rowsText[256], instanceText[256], currentText[256], previousText[256];
            formatTransform(rowsText, sizeof(rowsText), sample.rows, sample.rowsRead);
            formatTransform(instanceText, sizeof(instanceText), sample.instance, sample.instanceRead);
            formatTransform(currentText, sizeof(currentText), sample.current, sample.currentRead);
            formatTransform(previousText, sizeof(previousText), sample.previous, sample.previousRead);
            std::snprintf(line, sizeof(line), "MOTION_PROBE_M frame=%u proxy=%llx rows=%s inst=%s cur=%s prev=%s\n",
                          sample.frame, static_cast<unsigned long long>(proxy), rowsText, instanceText, currentText,
                          previousText);
            std::fputs(line, log);
        }
        std::fflush(log);
        _unlock_file(log);
    }
    // Per-pipeline entry of a draw-level gate; CoverageGateCount for the gates
    // that describe the capture or the draw rather than the pipeline (failed
    // capture, missing command, frame id or instances).
    static unsigned pipelineGate(unsigned stage) noexcept
    {
        switch (stage)
        {
        case GatePrepareNotPacked: return GeometryPipelineEntry::GateNotPacked;
        case GatePrepareRoot: return GeometryPipelineEntry::GateRoot;
        case GatePrepareMapping: return GeometryPipelineEntry::GateMapping;
        case GatePrepareRaster: return GeometryPipelineEntry::GateRaster;
        case GatePrepareShape: return GeometryPipelineEntry::GateShape;
        case GatePrepareViewport: return GeometryPipelineEntry::GateViewport;
        case GatePrepareFrameSlot: return GeometryPipelineEntry::GateFrameSlot;
        case GatePrepareOrdering: return GeometryPipelineEntry::GateOrdering;
        case GatePrepareNoElement: return GeometryPipelineEntry::GateNoElement;
        default: return GeometryPipelineEntry::CoverageGateCount;
        }
    }
    // A draw-level refusal on the stage counter and, for a known pipeline, on
    // its GEOMETRY_PIPELINE split. Only while the gate trace is armed.
    static void refuse(bool gate, const GeometryPipelineEntry* pipeline, unsigned stage) noexcept
    {
        if (!gate) return;
        GateNote(stage);
        const auto entry = pipelineGate(stage);
        if (pipeline && entry != GeometryPipelineEntry::CoverageGateCount)
            pipeline->coverage.gates[entry].fetch_add(1, std::memory_order_relaxed);
    }
    void rejectPipeline(const GeometryPipelineEntry& pipeline, std::uint32_t chunk) noexcept
    {
        std::lock_guard lock(rejectionMutex);
        ++rejections.missingPipeline;
        noteChunk(rejections.missingChunks, chunk);
        if (pipeline.deltaMissing)
        {
            ++rejections.deltaMissingDraws;
            noteChunk(rejections.deltaMissingChunks, chunk);
        }
    }
    void rejectTopology(std::uint64_t Rejections::* reason, std::uint32_t chunk) noexcept
    {
        std::lock_guard lock(rejectionMutex);
        ++rejections.topologyRejected;
        ++(rejections.*reason);
        noteChunk(rejections.topologyChunks, chunk);
    }
    void rejectSpans(const std::array<std::uint32_t, SpanRefusalCount>& refused, std::uint64_t total,
                     std::uint32_t chunk) noexcept
    {
        std::lock_guard lock(rejectionMutex);
        rejections.unknownIdentity += total;
        rejections.unknownOwnerSpan += refused[SpanOwner];
        rejections.unknownResolve += refused[SpanResolve];
        rejections.unknownOwnerMismatch += refused[SpanOwnerMismatch];
        rejections.unknownFieldMismatch += refused[SpanFieldMismatch];
        rejections.unknownNoArrayGeneration += refused[SpanNoArrayGeneration];
        noteChunk(rejections.unknownChunks, chunk, total);
    }
    static bool fits(const Frame& slot, std::uint32_t instances) noexcept
    {
        return slot.mappingUsed <= MappingCapacity - instances && slot.constantsUsed != ConstantCapacity;
    }
    // Draw-level admission, under the mutex, in the order a draw always met
    // it: capture state, frame slot, mapping frame, capacity, recording.
    // GateStageCount when the draw may admit elements into `slot`.
    unsigned openDraw(ID3D12GraphicsCommandList* command, std::uint64_t epoch, std::uint32_t frameNumber,
                      std::uint32_t instances, Frame*& slot, DeferredLog& deferred)
    {
        if (failed)
            return GatePrepareFailed;
        slot = frame(frameNumber);
        if (!slot)
            return GatePrepareFrameSlot;
        if (!beginMappings(frameNumber, deferred))
        {
            ++counters.orderingRejected;
            return GatePrepareOrdering;
        }
        if (!fits(*slot, instances))
        {
            ++counters.mappingOverflow;
            return GatePrepareMapping;
        }
        if (!epoch || !record(*slot, command, epoch))
        {
            ++counters.orderingRejected;
            return GatePrepareOrdering;
        }
        return GateStageCount;
    }

  public:
    bool prepare(ID3D12GraphicsCommandList* command, const GeometryDrawView& draw,
                 const GeometryIndexedArguments& args, const std::shared_ptr<const GeometryPipelineEntry>& pipeline,
                 const GraphicsRootBindings&, GeometryPreparedDraw& prepared) noexcept override
    {
        const bool gate = GateArmed();
        // Per-pipeline coverage (GEOMETRY_PIPELINES, GeometryHost.cpp). It is
        // counted only while the gate trace is armed, like the stage counters.
        if (gate && pipeline)
            pipeline->coverage.draws.fetch_add(1, std::memory_order_relaxed);
        const auto frameNumber = resolvedDrawFrame(draw);
        const bool broken = failed.load(std::memory_order_relaxed);
        if (broken || !command || frameNumber == 0 || !args.instances || args.instances > MappingCapacity ||
            !pipeline || !pipeline->packed || !pipeline->root || !pipeline->root->extended)
        {
            refuse(gate, pipeline.get(),
                   broken                             ? GatePrepareFailed
                   : !command                         ? GatePrepareCommand
                   : frameNumber == 0                 ? GatePrepareFrameId
                   : !args.instances                  ? GatePrepareInstances
                   : args.instances > MappingCapacity ? GatePrepareMapping
                   : !pipeline                        ? GatePreparePipeline
                   : !pipeline->packed                ? GatePrepareNotPacked
                                                      : GatePrepareRoot);
            if (pipeline && !pipeline->packed)
                rejectPipeline(*pipeline, draw.chunk);
            return false;
        }
        // Graft variant gate. The engine evaluates the MotionMatrix supply once
        // per draw proxy and its array append copies every element's transform
        // without a per-element evaluation (EngineMotionSupply.md), so the root
        // graft would move every element of an array/grouped span, and every
        // instance of a multi-instance draw, by the root's previous transform.
        // Such a draw takes the camera-only graft variant (packedArray): the
        // engine's own velocity for array elements is the previous
        // view-projection applied to each element's current world position.
        // Without it the draw takes the vertex-history variant when one was
        // compiled under VertexHistoryFallback, otherwise it keeps the engine's
        // motion.
        ID3D12PipelineState* packedPipeline = pipeline->packed.Get();
        bool graftDraw = pipeline->nativeGraft;
        bool graftArrayDraw = false;
        if (graftDraw)
        {
            bool arrayDraw = args.instances != 1;
            for (const auto& span : draw.objects)
                arrayDraw = arrayDraw || (span.count && (!span.identity || span.count != 1));
            if (arrayDraw)
            {
                // Gate-trace evidence for the engine's array convention: which
                // draws of a graft pipeline arrive with more than one instance
                // or a non-single span, and whether their depth test is on. The
                // line is several writes; the file lock keeps it whole against
                // the other recording threads.
                if (gate && log && claimLine(gateDetailLines, GateDetailLimit))
                {
                    const bool depthTest = pipeline->description.DepthStencilState.DepthEnable != 0;
                    const bool blended = pipeline->description.BlendState.RenderTarget[0].BlendEnable != 0;
                    _lock_file(log);
                    std::fprintf(log,
                                 "GATE_DETAIL reason=array pipeline=%llu chunk=%u instances=%u spans=%zu depth=%d "
                                 "blend=%d frame=%u",
                                 static_cast<unsigned long long>(pipeline->identity), draw.chunk, args.instances,
                                 draw.objects.size(), depthTest ? 1 : 0, blended ? 1 : 0, frameNumber);
                    for (const auto& span : draw.objects)
                        std::fprintf(log, " [first=%u count=%u id=%d parent=%d]", span.first, span.count,
                                     span.identity ? 1 : 0, span.parent ? 1 : 0);
                    std::fputc('\n', log);
                    std::fflush(log);
                    _unlock_file(log);
                }
                if (pipeline->packedArray)
                {
                    packedPipeline = pipeline->packedArray.Get();
                    graftArrayDraw = true;
                }
                else if (pipeline->packedHistory)
                    packedPipeline = pipeline->packedHistory.Get();
                else
                {
                    NoteGeometryGraft(GraftArrayRejected);
                    if (gate)
                        pipeline->coverage.arrayRejected.fetch_add(1, std::memory_order_relaxed);
                    return false;
                }
                graftDraw = false;
            }
        }
        // Stale MotionMatrix rule (stalemotion=camera, GlassControls.h). The
        // root graft reads the previous transform from the engine's MotionMatrix
        // rows. The engine's velocity collector gives a single non-array proxy
        // object velocity only in history state 1, or with the motion flag and
        // an active skinning or special-input component (EngineMotionSupply.md
        // "Proxy history convention"). Any other proxy keeps the velocity
        // initialization, the previous camera applied to the current surface.
        // The draw of such a proxy takes the camera-only variant, which is that
        // convention (CyberpunkMotionHistory::cameraOnly; the motion flag alone
        // keeps the root graft). An unreadable owner keeps the root graft.
        const GeometryBatchSpan* ownerSpan = nullptr;
        for (const auto& span : draw.objects)
            if (span.count)
            {
                ownerSpan = &span;
                break;
            }
        bool staleCamera = false;
        if (graftDraw && ownerSpan && pipeline->packedArray && StaleMotionCameraEnabled())
        {
            CyberpunkMotionHistory history;
            if (ReadCyberpunkMotionHistory(ownerSpan->identity.proxy, history) && history.cameraOnly())
            {
                packedPipeline = pipeline->packedArray.Get();
                graftDraw = false;
                graftArrayDraw = true;
                staleCamera = true;
            }
        }
        if (MotionProbeArmed() && MotionProbeMatches(pipeline->vertexHash))
            probeMotion(draw, args, *pipeline, ownerSpan,
                        graftDraw        ? "root"
                        : staleCamera    ? "stale"
                        : graftArrayDraw ? "array"
                                         : "history",
                        staleCamera);
        // Graft variants (root or camera) read no GlassHistory and write no
        // GlassNext: their VS tests only the mapping generation and exports the
        // map index. Their elements take identity-only mappings (mapping slot
        // and boundary ID, no arena block), so the arena capacity and a full
        // arena bound only the vertex-history path. Fixed for the whole draw:
        // the element loop can only move a root graft to the camera graft.
        const bool historyFree = graftDraw || graftArrayDraw;
        const auto* raster = ReadGeometryRasterState(command);
        const auto shape = ReadCyberpunkMeshShape(draw);
        if (!raster || !raster->usable())
        {
            refuse(gate, pipeline.get(), GatePrepareRaster);
            rejectTopology(&Rejections::rasterRejected, draw.chunk);
            return false;
        }
        if (!shape || !shape.vertices || (!historyFree && shape.vertices > HistoryCapacity))
        {
            refuse(gate, pipeline.get(), GatePrepareShape);
            rejectTopology(&Rejections::shapeRejected, draw.chunk);
            return false;
        }
        // Sizes the arena block on the vertex-history path; a graft draw skipped
        // the capacity check and uses the count for diagnostics only.
        const auto vertices = shape.vertices;
        const auto& viewport = raster->viewport;
        if (!std::isfinite(viewport.TopLeftX) || !std::isfinite(viewport.TopLeftY) || !std::isfinite(viewport.Width) ||
            !std::isfinite(viewport.Height) || viewport.Width <= 0 || viewport.Height <= 0 || viewport.TopLeftX < 0 ||
            viewport.TopLeftY < 0 || viewport.TopLeftX + viewport.Width > configuredWidth ||
            viewport.TopLeftY + viewport.Height > configuredHeight)
        {
            refuse(gate, pipeline.get(), GatePrepareViewport);
            rejectTopology(&Rejections::viewportRejected, draw.chunk);
            return false;
        }
        // Identities are resolved outside the capture mutex, a batch at a time:
        // the provider is safe on concurrent recording threads
        // (PackedMotionCapture.h) and the scratch belongs to this draw. The
        // mutex then covers what the recording threads share: the frame slot,
        // the object mappings and tables, the reservation and the upload writes
        // of that batch. The element storage stays uninitialized until an
        // element is resolved into it, so a draw pays only for its elements.
        PackedMotionIdentityScratch identityScratch;
        std::array<std::uint32_t, SpanRefusalCount> refused {};
        alignas(Element) std::byte storage[ElementBatch * sizeof(Element)];
        const auto element = [&storage](unsigned index) noexcept -> Element&
        { return *std::launder(reinterpret_cast<Element*>(storage + index * sizeof(Element))); };
        // Resolution cursor: the next element is `ordinal` of span `spanIndex`.
        unsigned spanIndex = 0;
        std::uint32_t ordinal = 0;
        const auto resolveBatch = [&]() noexcept -> unsigned
        {
            unsigned count = 0;
            while (count < ElementBatch && spanIndex < draw.objects.size())
            {
                const auto& span = draw.objects[spanIndex];
                const auto& owner = span.parent ? span.parent : span.identity;
                if (!ordinal && (!owner || !span.count || std::uint64_t(span.first) + span.count > args.instances))
                {
                    refused[SpanOwner] += span.count ? 1u : 0u;
                    ++spanIndex;
                    continue;
                }
                const auto current = spanIndex;
                const auto elementOrdinal = ordinal;
                if (++ordinal == span.count)
                {
                    ordinal = 0;
                    ++spanIndex;
                }
                auto& item = *::new (storage + count * sizeof(Element)) Element;
                auto& key = item.key;
                if (!identitySource.resolve(identitySource.context, identityScratch, command, draw, shape, *pipeline,
                                            current, elementOrdinal, key) ||
                    !key || !key.object.generation)
                {
                    ++refused[SpanResolve];
                    continue;
                }
                if (!sameOwner(key.object, owner))
                {
                    ++refused[SpanOwnerMismatch];
                    continue;
                }
                if (key.chunk != draw.chunk || key.vertexFactory != shape.vertexFactory ||
                    key.pipeline != pipeline->identity)
                {
                    ++refused[SpanFieldMismatch];
                    continue;
                }
                if ((!span.identity || span.count != 1) && !key.arrayGeneration)
                {
                    ++refused[SpanNoArrayGeneration];
                    continue;
                }
                // Second half of the graft gate: an identity the resolver
                // placed in an array lifetime is an array element even when
                // the span looked single (one instance, so at most one element
                // precedes this switch). It takes the camera-only variant.
                if (graftDraw && key.arrayGeneration)
                {
                    if (!pipeline->packedArray)
                    {
                        NoteGeometryGraft(GraftArrayRejected);
                        if (gate)
                            pipeline->coverage.arrayRejected.fetch_add(1, std::memory_order_relaxed);
                        continue;
                    }
                    packedPipeline = pipeline->packedArray.Get();
                    graftDraw = false;
                    graftArrayDraw = true;
                }
                item.index = span.first + elementOrdinal;
                ++count;
            }
            return count;
        };
        Frame* frameSlot = nullptr;
        std::uint32_t slotSerial = 0, mappingBase = 0, constantIndex = 0;
        bool opened = false, reserved = false;
        // Draw-level refusal met under the mutex; GateStageCount when none.
        unsigned refusal = GateStageCount;
        std::uint64_t historyMisses = 0, epoch = 0;
        bool traceWaits = false;
        // Draw-wide inputs of the locked phase, read once the draw has an
        // element to admit.
        MaterialCaptureConstants constants {};
        auto count = resolveBatch();
        if (count)
        {
            epoch = ReadGeometryRecordingEpoch(command);
            const auto depthFunction = pipeline->description.DepthStencilState.DepthFunc;
            const bool reverse = depthFunction == D3D12_COMPARISON_FUNC_GREATER ||
                                 depthFunction == D3D12_COMPARISON_FUNC_GREATER_EQUAL;
            // Far-surface skip. The capture shader drops a record whose surface
            // is farther than this view distance, so distant level-of-detail
            // glass costs nothing and keeps the engine's own motion. 0 = keep
            // all.
            std::uint32_t farCutoffBits = 0;
            const auto controls = ReadControls();
            traceWaits = controls.trace;
            const auto farCutoffMeters = controls.farCutoffMeters();
            if (farCutoffMeters > 0.f)
                std::memcpy(&farCutoffBits, &farCutoffMeters, sizeof(farCutoffBits));
            constants = {
                viewport.TopLeftX, viewport.TopLeftY, 1.f / viewport.Width, 1.f / viewport.Height,
                0, 0, frameNumber, reverse ? 1u : 0u,
                0, 0, configuredWidth, configuredHeight,
                0, configuredWidth, configuredWidth * configuredHeight, farCutoffBits,
                // Coverage class boundary. The capture marks a record covered
                // when its material opacity reaches this value, and the packed
                // store keeps the nearest covered record ahead of any uncovered
                // one.
                controls.opacityThreshold(), 0.f, 0.f, 0.f
            };
        }
        while (count)
        {
            bool acquired = false;
            // Lines of this batch's holders (beginMappings), written after the
            // lock below is released.
            DeferredLog deferred(log);
            {
                std::unique_lock lock(mutex, std::try_to_lock);
                if (!lock)
                {
                    // Another recording thread is inside its own batch. Wait
                    // for it: the hold is bounded (ElementBatch) and dropping
                    // the draw would flip this surface between corrected and
                    // uncorrected from frame to frame.
                    if (gate)
                        GateNote(GatePrepareLockWait);
                    if (traceWaits)
                    {
                        LARGE_INTEGER begin {}, end {};
                        QueryPerformanceCounter(&begin);
                        lock.lock();
                        QueryPerformanceCounter(&end);
                        noteLockWait(std::uint64_t(end.QuadPart - begin.QuadPart));
                    }
                    else
                        lock.lock();
                }
                if (!opened)
                {
                    opened = true;
                    refusal = openDraw(command, epoch, frameNumber, args.instances, frameSlot, deferred);
                    if (refusal != GateStageCount)
                        break;
                    slotSerial = frameSlot->serial;
                }
                else if (frameSlot->serial != slotSerial || objectMappings.frame() != frameNumber)
                {
                    // Between two batches of this draw another thread moved the
                    // slot to a newer frame, which voids the reservation, or
                    // began the next frame, which ends admission for the rest.
                    // A draw that keeps its reservation is a partial capture,
                    // not an ordering refusal.
                    reserved = reserved && frameSlot->serial == slotSerial;
                    if (!reserved)
                        ++counters.orderingRejected;
                    refusal = GatePrepareOrdering;
                    break;
                }
                else if (!reserved && !fits(*frameSlot, args.instances))
                {
                    ++counters.mappingOverflow;
                    refusal = GatePrepareMapping;
                    break;
                }
                acquired = true;
                unsigned admitted = 0;
                // Consecutive admitted elements of one owner mesh; the family
                // table is updated once per run.
                std::uint32_t familyMesh = 0, familyRun = 0;
                for (unsigned i = 0; i < count; ++i)
                {
                    auto& item = element(i);
                    // A graft element fails only when the frame-local boundary
                    // table has no ID left for it; the arena is never asked.
                    item.allocation = historyFree ? objectMappings.acquireIdentity(item.key, frameNumber)
                                                  : objectMappings.acquire(item.key, vertices, frameNumber);
                    if (!item.allocation)
                    {
                        ++counters.historyOverflow;
                        noteChunk(overflowChunks, draw.chunk);
                        continue;
                    }
                    ++admitted;
                    // The family table is a bounded diagnostic key, not an
                    // identity check (the hash still mixes the full mesh
                    // address), so the low 32 bits printed in the status line
                    // are enough here.
                    const auto familyKey = std::uint32_t(item.key.object.mesh);
                    if (familyRun && familyKey != familyMesh)
                    {
                        noteSpanFamily(draw.chunk, familyMesh, vertices, familyRun);
                        familyRun = 0;
                    }
                    familyMesh = familyKey;
                    ++familyRun;
                }
                if (familyRun)
                    noteSpanFamily(draw.chunk, familyMesh, vertices, familyRun);
                frameSpanCount += admitted;
                // The first admitted element reserves the draw's mapping range
                // and constant slot, under the same hold that checked the
                // capacity. A draw that admits nothing reserves nothing.
                const bool fresh = admitted && !reserved;
                if (fresh)
                {
                    reserved = true;
                    mappingBase = frameSlot->mappingUsed;
                    frameSlot->mappingUsed += args.instances;
                    constantIndex = frameSlot->constantsUsed++;
                    frameSlot->pipelines[constantIndex] = pipeline;
                    if (historyFree)
                    {
                        ++frameSlot->graftDraws;
                        ++counters.historyBypassed;
                    }
                }
                // Dump id table: one row per admitted element, keyed by the
                // draw's constant index.
                if (reserved && frameSlot->idsRecording)
                {
                    frameSlot->idVariants[constantIndex] = graftDraw        ? DumpVariantRoot
                                                           : staleCamera    ? DumpVariantStale
                                                           : graftArrayDraw ? DumpVariantArray
                                                                            : DumpVariantHistory;
                    for (unsigned i = 0; i < count; ++i)
                    {
                        const auto& item = element(i);
                        if (item.allocation && frameSlot->idCount < frameSlot->ids.size())
                            frameSlot->ids[frameSlot->idCount++] = { std::uint16_t(item.allocation.boundaryId),
                                                                     std::uint16_t(constantIndex) };
                    }
                }
                // Upload writes last. The mapping and constant buffers are
                // write-combined upload memory, and a locked instruction
                // (reference counts, the mutex release) waits until pending
                // write-combined lines have drained, so the records are written
                // together as whole 64-byte records after the last locked
                // instruction but the release. The bytes are unchanged: zeroes
                // over the draw's range, then each admitted record in admission
                // order. They stay under the mutex, so a slot another thread
                // reclaims later is rewritten after these writes, never under
                // them.
                if (admitted)
                {
                    auto* mapping = frameSlot->mappings + mappingBase;
                    if (fresh)
                    {
                        std::memset(mapping, 0, args.instances * sizeof(GeometryInstance));
                        std::memcpy(frameSlot->constants + constantIndex * 256, &constants, sizeof(constants));
                    }
                    GeometryInstance record {};
                    record.width = configuredWidth;
                    record.height = configuredHeight;
                    record.pixelBase = 1;
                    record.stride = configuredWidth;
                    record.pixelCapacity = configuredWidth * configuredHeight + 1;
                    for (unsigned i = 0; i < count; ++i)
                    {
                        const auto& item = element(i);
                        if (!item.allocation)
                            continue;
                        record.historyBase = item.allocation.history.base;
                        record.vertices = item.allocation.history.vertices;
                        record.generation = item.allocation.history.generation;
                        record.reserved[0] = item.allocation.boundaryId;
                        mapping[item.index] = record;
                    }
                }
            }
            // Outside the mutex: the batch's refused elements and the bounded
            // detail lines.
            if (acquired)
                for (unsigned i = 0; i < count; ++i)
                {
                    const auto& item = element(i);
                    if (!item.allocation)
                    {
                        ++historyMisses;
                        if (gate && log && claimLine(gateDetailLines, GateDetailLimit))
                        {
                            std::fprintf(log,
                                         "GATE_DETAIL reason=%s pipeline=%llu chunk=%u mesh=%u verts=%u "
                                         "vp=%.0f,%.0f,%.0f,%.0f frame=%u\n",
                                         historyFree ? "identity" : "history",
                                         static_cast<unsigned long long>(pipeline->identity), draw.chunk,
                                         std::uint32_t(item.key.object.mesh), vertices, viewport.TopLeftX,
                                         viewport.TopLeftY, viewport.Width, viewport.Height, frameNumber);
                            std::fflush(log);
                        }
                    }
                    else if (gate && log && claimLine(gateAdmitLines, GateAdmitLimit))
                    {
                        std::fprintf(log,
                                     "GATE_DETAIL reason=admit pipeline=%llu chunk=%u mesh=%u verts=%u "
                                     "vp=%.0f,%.0f,%.0f,%.0f frame=%u\n",
                                     static_cast<unsigned long long>(pipeline->identity), draw.chunk,
                                     std::uint32_t(item.key.object.mesh), vertices, viewport.TopLeftX,
                                     viewport.TopLeftY, viewport.Width, viewport.Height, frameNumber);
                        std::fflush(log);
                    }
                }
            count = resolveBatch();
        }
        identitySource.flush(identitySource.context, identityScratch);
        std::uint64_t spanRefused = 0;
        for (const auto value : refused)
            spanRefused += value;
        if (spanRefused)
            rejectSpans(refused, spanRefused, draw.chunk);
        if (gate)
        {
            if (spanRefused)
            {
                GateNote(GatePrepareSpan, spanRefused);
                for (unsigned reason = 0; reason < SpanRefusalCount; ++reason)
                    if (refused[reason])
                        pipeline->coverage.gates[SpanRefusalGate[reason]].fetch_add(refused[reason],
                                                                                    std::memory_order_relaxed);
            }
            if (historyMisses)
            {
                GateNote(GatePrepareHistory, historyMisses);
                pipeline->coverage.gates[GeometryPipelineEntry::GateHistory].fetch_add(historyMisses,
                                                                                       std::memory_order_relaxed);
            }
        }
        if (!reserved)
        {
            refuse(gate, pipeline.get(), refusal != GateStageCount ? refusal : GatePrepareNoElement);
            return false;
        }
        // Admission ended early (the next frame began between two batches):
        // the admitted part stands and the rest keeps the engine's motion. The
        // draw counts as a capture, so it goes to `partial`, which the coverage
        // report does not take off the eligible draws, never to `ordering`.
        if (refusal != GateStageCount && gate)
            pipeline->coverage.gates[GeometryPipelineEntry::GatePartial].fetch_add(1, std::memory_order_relaxed);
        if (historyFree)
            NoteGeometryGraft(graftDraw ? GraftDraws : staleCamera ? GraftStaleCameraDraws : GraftArrayDraws);
        if (gate)
        {
            auto& coverage = pipeline->coverage;
            coverage.captures.fetch_add(1, std::memory_order_relaxed);
            // A stale-rule draw used the camera-only variant, so it counts as
            // `array` here; GRAFT stale_camera and the dump variant `stale`
            // tell it apart.
            if (graftDraw)
                coverage.graft.fetch_add(1, std::memory_order_relaxed);
            else if (graftArrayDraw)
                coverage.array.fetch_add(1, std::memory_order_relaxed);
        }
        // Crash attribution for the replay path. Sparse on purpose: one line per
        // few hundred frames keeps the log bounded while still proving that the
        // packed raster was drawn after the last load.
        if (log && frameNumber % 300 == 0 && lastReplayFrame.load(std::memory_order_relaxed) != frameNumber &&
            lastReplayFrame.exchange(frameNumber, std::memory_order_relaxed) != frameNumber)
        {
            std::fprintf(log, "TRACE_REPLAY frame=%u chunk=%u\n", frameNumber, draw.chunk);
            std::fflush(log);
        }
        prepared.pipeline = packedPipeline;
        prepared.history = { mappingBase, MappingCapacity, HistoryCapacity, 0, args.instances, 0,
                             frameNumber, frameNumber - 1 };
        prepared.previous = historyAddress[(frameNumber - 1) & 1];
        prepared.current = historyAddress[frameNumber & 1];
        prepared.material = frameSlot->constantAddress + UINT64(constantIndex) * 256;
        prepared.capture = frameSlot->captureAddress;
        prepared.mapping = frameSlot->mappingAddress;
        return true;
    }

    void finish(ID3D12GraphicsCommandList*, bool recorded) noexcept override
    {
        if (!recorded) return;
        // No lock: prepare already reserved everything the recorded draw uses.
        admittedDraws.fetch_add(1, std::memory_order_relaxed);
        GeometryTelemetry::counts[GeometryCaptureDraws].fetch_add(1, std::memory_order_relaxed);
        GeometryTelemetry::changedMs[GeometryCaptureDraws].store(GetTickCount64(), std::memory_order_relaxed);
    }

    void beforeSubmit(ID3D12CommandQueue* queue, UINT count, ID3D12CommandList* const* lists) noexcept override
    {
        if (!queue || !lists || !count || queue->GetDesc().Type != D3D12_COMMAND_LIST_TYPE_DIRECT) return;
        // The history clear and one clear per slot, queued after the mutex is
        // released so a recording thread in prepare never waits on the driver.
        // The host's submission scope (D3D12Observer submit) serializes every
        // hooked submission, so the clears still reach the queue ahead of these
        // lists, and nothing acts on clearSubmitted before submitted() has
        // seen these lists.
        std::array<ID3D12CommandList*, FrameCount + 1> clears {};
        UINT clearCount = 0;
        {
            std::lock_guard lock(mutex);
            if (failed) return;
            for (auto& frame : frames)
            {
                bool selected = false;
                for (UINT i = 0; i < count; ++i) selected |= contains(frame, lists[i]);
                if (!selected || frame.clearSubmitted) continue;
                if (!historyClearSubmitted)
                {
                    historyQueue = queue;
                    clears[clearCount++] = historyClearCommand.Get();
                    historyClearSubmitted = true;
                }
                else if (historyQueue.Get() != queue)
                {
                    failed = true; ++counters.orderingRejected; continue;
                }
                if (frame.producerQueue && frame.producerQueue.Get() != queue)
                {
                    failed = true; ++counters.orderingRejected; continue;
                }
                frame.producerQueue = queue;
                clears[clearCount++] = frame.clearCommand.Get();
                frame.clearSubmitted = true;
                ++counters.capturedFrames;
            }
        }
        for (UINT i = 0; i < clearCount; ++i)
            queue->ExecuteCommandLists(1, &clears[i]);
    }

    void submitted(ID3D12CommandQueue* queue, UINT count, ID3D12CommandList* const* lists) noexcept override
    {
        std::lock_guard lock(mutex);
        if (failed || !queue || !lists || !count) return;
        const auto batch = ++submitBatchSequence;
        for (auto& command : fgCommands)
            if (command.command)
                for (UINT i = 0; i < count; ++i)
                    if (lists[i] == command.command && (!command.queue || command.queue.Get() == queue))
                    {
                        command.queue = queue;
                        command.submitOrder = ++submitSequence;
                        command.submitBatch = batch;
                    }
        for (auto& frame : frames)
        {
            bool produced = false, consumed = false;
            for (UINT i = 0; i < count; ++i)
            {
                produced |= contains(frame, lists[i]);
                consumed |= frame.consumerCommand && lists[i] == frame.consumerCommand;
            }
            if (produced)
            {
                if (!frame.clearSubmitted || (frame.producerQueue && frame.producerQueue.Get() != queue) ||
                    nextProducer == UINT64_MAX || FAILED(queue->Signal(producerFence.Get(), ++nextProducer)))
                { failed = true; ++counters.orderingRejected; }
                else
                {
                    frame.producerQueue = queue; frame.producerValue = nextProducer;
                    frame.submitOrder = ++submitSequence;
                    frame.submitBatch = batch;
                    frame.syncPending = true; frame.syncFence.Reset(); frame.orderedQueue.Reset();
                }
            }
            if (consumed)
            {
                if ((frame.consumerQueue && frame.consumerQueue.Get() != queue) || nextConsumer == UINT64_MAX ||
                    FAILED(queue->Signal(consumerFence.Get(), ++nextConsumer)))
                { failed = true; ++counters.orderingRejected; }
                else
                { frame.consumerQueue = queue; frame.consumerValue = nextConsumer; }
            }
        }
    }

    void discarded(ID3D12GraphicsCommandList* command) noexcept override
    {
        discard(command, false);
    }

    void discard(const void* command, bool destroyed) noexcept
    {
        std::lock_guard lock(mutex);
        for (auto& frame : frames)
        {
            for (auto& recording : frame.recordings)
                if (recording.command == command) recording = {};
            if (frame.consumerCommand == command) frame.consumerCommand = nullptr;
        }
        if (destroyed)
            for (auto& fg : fgCommands)
                if (fg.command == command) fg = {};
    }

    void signal(ID3D12CommandQueue* queue, ID3D12Fence* fence, UINT64 value) noexcept override
    {
        std::lock_guard lock(mutex);
        if (failed || !queue || !fence) return;
        // Record engine-owned signals so a later Wait on the same fence/value can
        // establish the producer -> FG queue dependency. Our own fences are
        // private to this capture and nothing in the engine waits on them.
        if (fence != producerFence.Get() && fence != consumerFence.Get())
        {
            auto& record = observedSignals[nextObservedSignal++ % observedSignals.size()];
            record.fence = fence;
            record.queue = queue;
            record.value = value;
            record.order = submitSequence;
        }
        for (auto& frame : frames)
            if (frame.syncPending && frame.producerQueue.Get() == queue)
            {
                frame.syncFence = fence; frame.syncValue = value; frame.syncPending = false;
            }
    }

    void wait(ID3D12CommandQueue* queue, ID3D12Fence* fence, UINT64 value) noexcept override
    {
        std::lock_guard lock(mutex);
        if (failed || !queue || !fence) return;
        for (const auto& record : observedSignals)
            if (record.fence.Get() == fence && record.value == value && record.queue && record.queue.Get() != queue)
                for (auto& frame : frames)
                    if (frame.producerQueue.Get() == record.queue.Get() && frame.submitOrder &&
                        frame.submitOrder <= record.order)
                        frame.orderedQueue = queue;
        for (auto& frame : frames)
            if (frame.syncFence.Get() == fence && frame.syncValue == value)
                frame.orderedQueue = queue;
    }

    PackedMotionFrame acquire(ID3D12GraphicsCommandList* command, std::uint32_t width, std::uint32_t height,
                              std::uint64_t fgFrame, bool reset)
    {
        DeferredLog deferred(log); // Written after the lock below is released.
        std::lock_guard lock(mutex);
        if (failed || !command || fgFrame == UINT64_MAX || width != configuredWidth || height != configuredHeight)
        {
            if (fgFrame == UINT64_MAX) ++counters.noFgFrame;
            return {};
        }
        FgCommand* fg = nullptr;
        for (auto& value : fgCommands) if (value.command == command) fg = &value;
        if (!fg)
            for (auto& value : fgCommands) if (!value.command) { value.command = command; fg = &value; break; }
        if (!fg || !fg->queue) { ++counters.noFgQueue; return {}; }
        Frame* selected = nullptr;
        bool multiple = false;
        for (auto& value : frames)
            // A frame is admitted when its producer submission precedes the FG
            // command submission. If an engine Wait established the dependency,
            // the waiting queue must be the FG queue. The private-fence condition
            // used previously could never be satisfied.
            if (value.producerValue && value.clearSubmitted && value.number > framePair.engineFrame() &&
                value.submitBatch && fg->submitBatch && value.submitBatch <= fg->submitBatch &&
                (!value.orderedQueue || value.orderedQueue.Get() == fg->queue.Get()))
            {
                // Several frames can satisfy the order rule (older batches stay
                // eligible until the pair advances). The newest eligible frame
                // is the one this FG call consumed, so pick it instead of
                // rejecting the batch.
                if (selected)
                {
                    multiple = true;
                    if (value.submitBatch < selected->submitBatch ||
                        (value.submitBatch == selected->submitBatch && value.number <= selected->number))
                        continue;
                }
                selected = &value;
            }
        if (!selected)
        {
            // Driver path: Streamline's frame generation block carries no engine
            // frame number, so the ordered lookup above cannot match. Take the
            // newest frame this queue already produced instead; the frames are
            // held until a consumer takes them, so it is the frame under
            // construction for this evaluation.
            for (auto& value : frames)
                if (value.producerValue && value.clearSubmitted && !value.consumerCommand &&
                    (!value.orderedQueue || value.orderedQueue.Get() == fg->queue.Get()) &&
                    (!selected || value.number > selected->number))
                    selected = &value;
        }
        if (!selected)
        {
            ++counters.orderingRejected;
            ++counters.acquireNoCandidate;
            // Bounded attribution: which admission condition the candidate frames
            // failed, so a silent rejection can be interpreted.
            static std::atomic<unsigned> rejected { 0 };
            if (log != nullptr && rejected.fetch_add(1, std::memory_order_relaxed) < 6)
            {
                deferred.add("PACKED_ACQUIRE reject fgFrame=%llu engineFrame=%llu fgSubmitBatch=%llu\n",
                             static_cast<unsigned long long>(fgFrame),
                             static_cast<unsigned long long>(framePair.engineFrame()),
                             static_cast<unsigned long long>(fg->submitBatch));
                for (const auto& value : frames)
                    deferred.add("PACKED_ACQUIRE frame number=%u producer=%llu clear=%u submitBatch=%llu ordered=%u\n",
                                 value.number, static_cast<unsigned long long>(value.producerValue),
                                 value.clearSubmitted ? 1u : 0u, static_cast<unsigned long long>(value.submitBatch),
                                 value.orderedQueue ? 1u : 0u);
                deferred.flush = true;
            }
            return {};
        }
        if (multiple)
            ++counters.acquireAmbiguous; // Informational: older eligible frames were present.
        if (selected->consumerCommand && selected->consumerCommand != command)
        { ++counters.orderingRejected; ++counters.acquireConsumerBusy; return {}; }
        if (!framePair.advance(selected->number, fgFrame, reset))
        { ++counters.orderingRejected; ++counters.acquireStalePair; return {}; }
        selected->consumerCommand = command;
        ++counters.fgFrames;
        return { selected->capture.Get(),
                 configuredWidth,
                 configuredHeight,
                 selected->number,
                 fgFrame,
                 producerFence.Get(),
                 selected->producerValue,
                 static_cast<void*>(selected->producerQueue.Get()),
                 selected->graftDraws };
    }

    // A recording thread in prepare waits for whoever holds the capture mutex.
    // The longest holders, which bound that wait: this copy (the pinned-entry
    // count walks the 16,384-entry history table, about 1.4 MB, tens of
    // microseconds; once or twice per second from the health thread and the
    // report, and per UI frame while the settings overlay is open),
    // selectDumpFrame and writeDumpIds (dump requests only), and a prepare
    // batch on the vertex-history path whose arena is full (each failed
    // reservation sweeps up to 1,024 entries; graft draws never ask the arena).
    // Every other holder scans the three slots, one element batch (at most 64
    // table lookups and upload records; the first also zeroes the draw's
    // 64-byte records, one per instance) or the 256-entry signal ring;
    // submitted() adds its fence Signal calls, and beforeSubmit queues its
    // clears after releasing the mutex.
    PackedMotionCaptureStatus status()
    {
        PackedMotionCaptureStatus value;
        std::array<SpanFamily, SpanFamilyCount> sorted;
        {
            std::lock_guard lock(mutex);
            value = counters;
            value.healthy = counters.initialized && !failed;
            const auto& history = objectMappings.historyStats();
            value.historyHits = history.hits;
            value.historyInserted = history.inserted;
            value.historyReclaimed = history.reclaimed;
            value.historyRejectedTopology = history.rejectedTopology;
            value.historySetFull = history.setFull;
            value.historyArenaFull = history.arenaFull;
            value.historyArenaReclaimed = history.arenaReclaimed;
            value.historyArenaFullPages = history.arenaFullPages;
            value.historyArenaFullPageCount = history.arenaFullPageCount;
            value.historyLive = objectMappings.liveHistories();
            value.historyArenaPages = objectMappings.arenaPages();
            value.historyArenaUsedPages = objectMappings.arenaUsedPages();
            value.historyArenaLargestFree = objectMappings.arenaLargestFreePages();
            value.historyFrame = objectMappings.historyFrame();
            value.historyRetiredFrame = objectMappings.historyRetiredFrame();
            value.historyPinnedEntries = objectMappings.historyPinnedEntries();
            value.overflowChunks = overflowChunks;
            value.spanFamilyEvictions = spanFamilyEvictions;
            value.frameSpanCount = frameSpanCount;
            sorted = spanFamilies;
        }
        {
            std::lock_guard lock(rejectionMutex);
            value.missingPipeline = rejections.missingPipeline;
            value.deltaMissingDraws = rejections.deltaMissingDraws;
            value.topologyRejected = rejections.topologyRejected;
            value.rasterRejected = rejections.rasterRejected;
            value.shapeRejected = rejections.shapeRejected;
            value.viewportRejected = rejections.viewportRejected;
            value.unknownIdentity = rejections.unknownIdentity;
            value.unknownOwnerSpan = rejections.unknownOwnerSpan;
            value.unknownResolve = rejections.unknownResolve;
            value.unknownOwnerMismatch = rejections.unknownOwnerMismatch;
            value.unknownFieldMismatch = rejections.unknownFieldMismatch;
            value.unknownNoArrayGeneration = rejections.unknownNoArrayGeneration;
            value.unknownChunks = rejections.unknownChunks;
            value.topologyChunks = rejections.topologyChunks;
            value.missingChunks = rejections.missingChunks;
            value.deltaMissingChunks = rejections.deltaMissingChunks;
        }
        value.admittedDraws = admittedDraws.load(std::memory_order_relaxed);
        // Copy the heaviest families only; the full table stays internal so
        // the periodic line and the control response keep a fixed size. Sorted
        // after the mutex is released.
        std::sort(sorted.begin(), sorted.end(),
                  [](const SpanFamily& a, const SpanFamily& b) { return a.count > b.count; });
        value.admittedSpanFamilyCount = 0;
        for (const auto& entry : sorted)
        {
            if (!entry.count || value.admittedSpanFamilyCount >= value.admittedSpans.size()) break;
            value.admittedSpans[value.admittedSpanFamilyCount++] =
                { entry.chunk, entry.mesh, entry.vertices, entry.count };
        }
        return value;
    }

    // Second consumer selection for the DLSS-NR seam. The frame generation rule
    // ("the producer submitted before the frame generation command") cannot be
    // evaluated here, because the neural rendering pass is recorded on the
    // game's own list before that list is submitted. What can be established is
    // that the frame's draw work has already been submitted, which is what the
    // inline compose needs. A frame the substitution already took is skipped.
    PackedMotionFrame acquireSecondConsumer(std::uint32_t width, std::uint32_t height, std::uint64_t engineFrame)
    {
        DeferredLog deferred(log); // Written after the lock below is released.
        std::lock_guard lock(mutex);
        if (failed || width != configuredWidth || height != configuredHeight)
            return {};
        auto* selected = SelectSecondConsumerFrame(frames, engineFrame);
        if (!selected)
        {
            ++counters.acquireNoCandidate;
            static std::atomic<unsigned> logged { 0 };
            if (log != nullptr && logged.fetch_add(1, std::memory_order_relaxed) < 6)
            {
                deferred.add("PACKED_SECOND reject engineFrame=%llu pair=%llu\n",
                             static_cast<unsigned long long>(engineFrame),
                             static_cast<unsigned long long>(framePair.engineFrame()));
                for (const auto& value : frames)
                    deferred.add("PACKED_SECOND frame number=%u producer=%llu clear=%u consumed=%u\n",
                                 value.number, static_cast<unsigned long long>(value.producerValue),
                                 value.clearSubmitted ? 1u : 0u, value.consumerCommand ? 1u : 0u);
                deferred.flush = true;
            }
            return {};
        }
        return { selected->capture.Get(),
                 configuredWidth,
                 configuredHeight,
                 selected->number,
                 selected->number,
                 producerFence.Get(),
                 selected->producerValue,
                 static_cast<void*>(selected->producerQueue.Get()),
                 selected->graftDraws };
    }

    // Dump id table, compose side (PackedMotionCapture.h). The compose calls
    // this on the frame generation thread when it would copy `number` for a
    // pending dump. A slot that recorded from its first draw holds the complete
    // table. The first call for a request arms the recording and holds the
    // dump back. After DumpIdMaxDeferred calls the dump goes ahead with an
    // empty, incomplete table, so a capture that never records again cannot
    // stall a dump.
    bool selectDumpFrame(std::uint32_t number)
    {
        DeferredLog deferred(log); // Written after the lock below is released.
        std::lock_guard lock(mutex);
        const Frame* slot = nullptr;
        for (const auto& value : frames)
            if (value.number == number)
                slot = &value;
        const bool complete = slot && slot->idsRecording;
        if (!complete)
        {
            if (!idsArmed)
            {
                idsArmed = true;
                idsArmedFrames = idsDeferred = 0;
            }
            if (idsDeferred < DumpIdMaxDeferred)
            {
                ++idsDeferred;
                return false;
            }
        }
        // Freeze: plain copies, since the slot is reused long before the
        // health thread writes the dump.
        auto& snapshot = idSnapshot;
        snapshot.frame = number;
        snapshot.complete = complete;
        snapshot.deferred = idsDeferred;
        snapshot.count = snapshot.pipelines = 0;
        if (complete)
        {
            snapshot.count = slot->idCount;
            snapshot.pipelines = slot->constantsUsed;
            std::copy_n(slot->ids.data(), snapshot.count, snapshot.ids.data());
            for (unsigned i = 0; i < snapshot.pipelines; ++i)
            {
                const auto& entry = slot->pipelines[i];
                snapshot.pipeline[i] = entry ? DumpIdPipeline { entry->identity, entry->vertexHash, entry->pixelHash,
                                                                entry->graftKind, slot->idVariants[i] }
                                             : DumpIdPipeline {};
            }
        }
        snapshot.valid = true;
        idsArmed = false;
        idsDeferred = 0;
        deferred.add("PACKED_IDS frozen frame=%u elements=%u draws=%u complete=%u deferred=%u\n", number,
                     snapshot.count, snapshot.pipelines, complete ? 1u : 0u, snapshot.deferred);
        deferred.flush = true;
        return true;
    }

    // Dump id table, writer side: the table frozen for `number`, as
    // `id pipeline vs16 ps16 kind variant` rows, one per distinct
    // (id, pipeline, variant). Runs where the dump files are written. The
    // capture mutex covers only the copy out of the snapshot.
    bool writeDumpIds(const wchar_t* path, unsigned serial, std::uint32_t number, PackedMotionDumpIdSummary* summary)
    {
        std::vector<DumpId> ids(MappingCapacity);
        std::vector<DumpIdPipeline> pipelines(ConstantCapacity);
        std::uint32_t count = 0, deferred = 0;
        bool complete = false;
        {
            std::lock_guard lock(mutex);
            if (!path || !idSnapshot.valid || idSnapshot.frame != number)
                return false;
            idSnapshot.valid = false;
            count = idSnapshot.count;
            deferred = idSnapshot.deferred;
            complete = idSnapshot.complete;
            std::copy_n(idSnapshot.ids.data(), count, ids.data());
            std::copy_n(idSnapshot.pipeline.data(), idSnapshot.pipelines, pipelines.data());
            pipelines.resize(idSnapshot.pipelines);
        }
        struct Row
        {
            std::uint32_t id;
            const DumpIdPipeline* pipeline;
        };
        std::vector<Row> rows;
        rows.reserve(count);
        for (std::uint32_t i = 0; i < count; ++i)
            if (ids[i].constant < pipelines.size())
                rows.push_back({ ids[i].id, &pipelines[ids[i].constant] });
        const auto before = [](const Row& a, const Row& b)
        {
            if (a.id != b.id)
                return a.id < b.id;
            if (a.pipeline->identity != b.pipeline->identity)
                return a.pipeline->identity < b.pipeline->identity;
            return a.pipeline->variant < b.pipeline->variant;
        };
        std::sort(rows.begin(), rows.end(), before);
        rows.erase(std::unique(rows.begin(), rows.end(),
                               [&](const Row& a, const Row& b) { return !before(a, b) && !before(b, a); }),
                   rows.end());
        std::uint32_t distinct = 0;
        for (std::size_t i = 0; i < rows.size(); ++i)
            distinct += i == 0 || rows[i].id != rows[i - 1].id ? 1u : 0u;
        FILE* file = _wfopen(path, L"wb");
        if (!file)
            return false;
        std::fprintf(file, "PIPELINES serial=%u frame=%u records=%zu ids=%u complete=%u deferred=%u\n", serial,
                     number, rows.size(), distinct, complete ? 1u : 0u, deferred);
        std::fprintf(file, "# id pipeline vs16 ps16 kind variant\n");
        for (const auto& row : rows)
            std::fprintf(file, "%u %llu %016llx %016llx %s %s\n", row.id,
                         static_cast<unsigned long long>(row.pipeline->identity),
                         static_cast<unsigned long long>(row.pipeline->vertexHash),
                         static_cast<unsigned long long>(row.pipeline->pixelHash),
                         GeometryGraftKindName(row.pipeline->kind), dumpVariantName(row.pipeline->variant));
        const bool written = !std::ferror(file);
        if (std::fclose(file) != 0 || !written)
            return false;
        if (summary)
            *summary = { static_cast<std::uint32_t>(rows.size()), distinct, deferred, complete };
        return true;
    }

    void resetCounters()
    {
        {
            std::lock_guard lock(mutex);
            const auto initialized = counters.initialized, healthy = counters.healthy;
            const auto width = counters.width, height = counters.height;
            counters = {};
            counters.initialized = initialized;
            counters.healthy = healthy;
            counters.width = width;
            counters.height = height;
            overflowChunks = {};
            spanFamilies = {};
            spanFamilyEvictions = 0;
        }
        {
            std::lock_guard lock(rejectionMutex);
            rejections = {};
        }
        admittedDraws.store(0, std::memory_order_relaxed);
    }
};

std::atomic<Capture*> active = nullptr;

// Dump id table hooks (PackedMotionCapture.h). A missing capture never holds a
// dump back, and a failure in the table never blocks the dump either.
bool SelectPackedMotionDumpFrame(std::uint32_t frame) noexcept
{
    try
    {
        auto* capture = active.load(std::memory_order_acquire);
        return !capture || capture->selectDumpFrame(frame);
    }
    catch (...)
    {
        return true;
    }
}
bool WritePackedMotionDumpIds(const wchar_t* path, unsigned serial, std::uint32_t frame,
                              PackedMotionDumpIdSummary* summary) noexcept
{
    try
    {
        auto* capture = active.load(std::memory_order_acquire);
        return capture && capture->writeDumpIds(path, serial, frame, summary);
    }
    catch (...)
    {
        return false;
    }
}
} // namespace

bool InitializePackedMotionCapture(ID3D12Device* device, std::uint32_t width, std::uint32_t height, FILE* log,
                                   PackedMotionIdentityProvider identities) noexcept
{
    try
    {
        if (auto* current = active.load(std::memory_order_acquire))
        {
            const auto status = current->status();
            return status.healthy && status.width == width && status.height == height;
        }
        auto* capture = new Capture(device, width, height, identities, log);
        if (!RegisterGeometryDrawCapture(capture))
        {
            delete capture;
            if (log) { std::fprintf(log, "PACKED_CAPTURE ready=0 reason=capture_owner_busy\n"); std::fflush(log); }
            return false;
        }
        active.store(capture, std::memory_order_release);
        packedMotionDumpSelect.store(SelectPackedMotionDumpFrame, std::memory_order_release);
        packedMotionDumpWrite.store(WritePackedMotionDumpIds, std::memory_order_release);
        if (log)
        {
            std::fprintf(log, "PACKED_CAPTURE ready=1 width=%u height=%u frames=%u mapping=%u history_vertices=%u\n",
                         width, height, FrameCount, MappingCapacity, HistoryCapacity);
            // Admission of the proxy history fields the stale MotionMatrix rule
            // and the motion probe read (CyberpunkDraws.h), once per process.
            // admitted=0 keeps every root draw on the root graft.
            static std::atomic<bool> admissionLogged { false };
            if (!admissionLogged.exchange(true, std::memory_order_relaxed))
            {
                const auto admission = ReadCyberpunkMotionHistoryAdmission();
                std::fprintf(log,
                             "STALE_MOTION_ADMISSION checked=%d admitted=%d collector=0x%x supplier=0x%x reader=0x%x\n",
                             admission.checked ? 1 : 0, admission.admitted ? 1 : 0, admission.collector,
                             admission.supplier, admission.reader);
            }
            std::fflush(log);
        }
        return true;
    }
    catch (const std::exception& error)
    {
        if (log) { std::fprintf(log, "PACKED_CAPTURE ready=0 reason=%s\n", error.what()); std::fflush(log); }
        return false;
    }
}

PackedMotionFrame AcquirePackedMotionFrame(ID3D12GraphicsCommandList* command,
                                           std::uint32_t width, std::uint32_t height,
                                           std::uint64_t fgFrame, bool reset) noexcept
{
    try
    {
        auto* capture = active.load(std::memory_order_acquire);
        return capture ? capture->acquire(command, width, height, fgFrame, reset) : PackedMotionFrame {};
    }
    catch (...) { return {}; }
}

PackedMotionFrame AcquirePackedMotionFrameForSecondConsumer(std::uint32_t width, std::uint32_t height,
                                                            std::uint64_t engineFrame) noexcept
{
    try
    {
        auto* capture = active.load(std::memory_order_acquire);
        return capture ? capture->acquireSecondConsumer(width, height, engineFrame) : PackedMotionFrame {};
    }
    catch (...)
    {
        return {};
    }
}

bool ReleasePackedMotionCapture() noexcept
{
    try
    {
        auto* capture = active.exchange(nullptr, std::memory_order_acq_rel);
        if (!capture)
            return false;
        UnregisterGeometryDrawCapture(capture);
        delete capture;
        return true;
    }
    catch (...)
    {
        return false;
    }
}

PackedMotionCaptureStatus ReadPackedMotionCaptureStatus() noexcept
{
    try
    {
        auto* capture = active.load(std::memory_order_acquire);
        return capture ? capture->status() : PackedMotionCaptureStatus {};
    }
    catch (...) { return {}; }
}

PackedMotionLockWaits ReadPackedMotionLockWaits() noexcept
{
    static const std::uint64_t frequency = []
    {
        LARGE_INTEGER value {};
        return QueryPerformanceFrequency(&value) && value.QuadPart > 0 ? std::uint64_t(value.QuadPart) : 0;
    }();
    const auto microseconds = [](std::uint64_t ticks)
    { return frequency ? ticks / frequency * 1000000 + ticks % frequency * 1000000 / frequency : 0; };
    return { lockWaits.load(std::memory_order_relaxed),
             microseconds(lockWaitTicks.load(std::memory_order_relaxed)),
             microseconds(lockWaitMaxTicks.exchange(0, std::memory_order_relaxed)) };
}

void ResetPackedMotionCounters() noexcept
{
    try
    {
        if (auto* capture = active.load(std::memory_order_acquire))
            capture->resetCounters();
    }
    catch (...)
    {
    }
}

void DiscardPackedMotionRecording(const void* command, bool destroyed) noexcept
{
    if (auto* capture = active.load(std::memory_order_acquire)) capture->discard(command, destroyed);
}
} // namespace GlassFg
