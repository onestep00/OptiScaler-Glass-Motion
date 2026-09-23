#pragma once
#include <atomic>
#include <cstdint>

namespace GlassFg
{
// The packed capture drops a draw at several points. Most of them are the
// common case for opaque content, so they return silently and leave no counter.
// When a transparent surface never reaches the packed raster there is then no
// evidence at all about which gate took it. These counters are the cheap half
// of the answer: two relaxed atomics per object-bearing draw and nothing while
// the trace is disarmed. The bounded detail lines are written by the packed
// capture, which owns the log handle.
enum GateStage : unsigned
{
    GateObjectDraws = 0,
    GateNoPipeline,
    GateNoBindings,
    GateNoRecord,
    GateNoOwner,
    GatePrepareLock,
    GatePrepareFailed,
    GatePrepareCommand,
    GatePrepareFrameId,
    GatePrepareInstances,
    GatePrepareMapping,
    GatePreparePipeline,
    GatePrepareNotPacked,
    GatePrepareRoot,
    GatePrepareRaster,
    GatePrepareShape,
    GatePrepareViewport,
    GatePrepareFrameSlot,
    GatePrepareOrdering,
    GatePrepareSpan,
    GatePrepareHistory,
    GatePrepareNoElement,
    GateBindRejected,
    GateCaptured,
    GateStageCount
};

struct GateState
{
    std::atomic<std::uint64_t> stage[GateStageCount] {};
    std::atomic<bool> armed { false };
    // Object-bearing draws whose original pipeline had no rewritten entry. The
    // distinct count separates "one unhandled shader" from "a whole family".
    std::atomic<std::uint64_t> unseenPipelineProbes {};
    std::atomic<std::uint64_t> unseenPipelineDistinct {};
    // Draws whose pipeline was not rewritten, split by what creation recorded
    // for that pointer. "Rejected transparent" is the actionable class: the
    // pipeline looked blended and was refused by the rewrite filter.
    std::atomic<std::uint64_t> unseenRejectedTransparent {};
    std::atomic<std::uint64_t> unseenRejectedOpaque {};
    std::atomic<std::uint64_t> unseenAcceptedNotReady {};
    std::atomic<std::uint64_t> unseenUnknown {};
};

inline GateState& Gate() noexcept
{
    static GateState state;
    return state;
}

inline bool GateArmed() noexcept
{
    return Gate().armed.load(std::memory_order_relaxed);
}

inline void GateArm(bool armed) noexcept
{
    Gate().armed.store(armed, std::memory_order_relaxed);
}

inline void GateNote(unsigned stage) noexcept
{
    Gate().stage[stage].fetch_add(1, std::memory_order_relaxed);
}

// Lock-free distinct-pipeline probe: a 64-word Bloom filter. Pointers can
// collide, so the result is an upper bound on the distinct pipelines, which is
// what separates "one object's shader was never rewritten" from "most of the
// scene's shaders were never rewritten".
inline void GateNoteUnseenPipeline(const void* pipeline) noexcept
{
    static std::atomic<std::uint64_t> bits[64] {};
    auto hash = (std::uint64_t(pipeline) >> 4) * 0x9e3779b97f4a7c15ull;
    hash ^= hash >> 29;
    const auto word = unsigned(hash >> 58) & 63u;
    const auto bit = std::uint64_t(1) << (unsigned(hash >> 6) & 63u);
    auto& state = Gate();
    state.unseenPipelineProbes.fetch_add(1, std::memory_order_relaxed);
    if ((bits[word].fetch_or(bit, std::memory_order_relaxed) & bit) == 0)
        state.unseenPipelineDistinct.fetch_add(1, std::memory_order_relaxed);
}

// The pipeline-creation filter runs once per created pipeline state and decides
// whether the module is allowed to rewrite it at all. A pipeline it rejects can
// never be found by the draw hook, so every draw that uses it is dropped with no
// counter. These counters are always on: creation is rare, and the interesting
// pipelines are created while the scene loads, before a live trace can be armed.
enum GateCandidateReason : unsigned
{
    GateCandidateRoot = 0,
    GateCandidateVertex,
    GateCandidatePixel,
    GateCandidateVertexBytes,
    GateCandidatePixelBytes,
    GateCandidateTargets,
    GateCandidateSamples,
    GateCandidateShaderStages,
    GateCandidateStreamOutput,
    GateCandidateTopology,
    GateCandidateInputLayout,
    GateCandidateDepthWrite,
    GateCandidateStencilWrite,
    GateCandidateBlend,
    GateCandidateRootUnknown,
    GateCandidateLimits,
    GateCandidateCompilationOff,
    GateCandidateReasonCount
};

struct GateCandidateState
{
    std::atomic<std::uint64_t> reason[GateCandidateReasonCount] {};
    std::atomic<std::uint64_t> rejected {}, transparentLooking {};
};

inline GateCandidateState& GateCandidate() noexcept
{
    static GateCandidateState state;
    return state;
}

inline void GateCandidateNote(unsigned reason, bool transparentLooking) noexcept
{
    auto& state = GateCandidate();
    if (reason < GateCandidateReasonCount)
        state.reason[reason].fetch_add(1, std::memory_order_relaxed);
    state.rejected.fetch_add(1, std::memory_order_relaxed);
    if (transparentLooking)
        state.transparentLooking.fetch_add(1, std::memory_order_relaxed);
}

// Creation-time census of the graphics pipelines the module actually sees, and
// the reason the rewrite filter accepted or refused each one. A pipeline that
// the filter refuses can never be found by the draw hook, so without this the
// only evidence is a bare "no pipeline" count at draw time that cannot say
// whether the unseen pipelines are opaque content or transparent surfaces.
struct GateCreationCensusState
{
    std::atomic<std::uint64_t> created {}, accepted {}, rejected {}, rejectedTransparent {};
    std::atomic<std::uint64_t> reason[GateCandidateReasonCount] {};
    std::atomic<std::uint64_t> transparentReason[GateCandidateReasonCount] {};
};

inline GateCreationCensusState& GateCreationCensus() noexcept
{
    static GateCreationCensusState state;
    return state;
}

inline void GateNoteCreatedPipeline(unsigned reason, bool transparentLooking) noexcept
{
    auto& state = GateCreationCensus();
    const auto index = reason < GateCandidateReasonCount ? reason : unsigned(GateCandidateReasonCount - 1);
    state.created.fetch_add(1, std::memory_order_relaxed);
    state.reason[index].fetch_add(1, std::memory_order_relaxed);
    if (reason == GateCandidateRoot)
    {
        state.accepted.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    state.rejected.fetch_add(1, std::memory_order_relaxed);
    if (transparentLooking)
    {
        state.rejectedTransparent.fetch_add(1, std::memory_order_relaxed);
        state.transparentReason[index].fetch_add(1, std::memory_order_relaxed);
    }
}

inline void GateNoteUnseenClass(unsigned classification, bool transparentLooking) noexcept
{
    auto& state = Gate();
    if (classification == 0)
        state.unseenUnknown.fetch_add(1, std::memory_order_relaxed);
    else if (classification == GateCandidateRoot + 1)
        state.unseenAcceptedNotReady.fetch_add(1, std::memory_order_relaxed);
    else if (transparentLooking)
        state.unseenRejectedTransparent.fetch_add(1, std::memory_order_relaxed);
    else
        state.unseenRejectedOpaque.fetch_add(1, std::memory_order_relaxed);
}

// Pointer to creation verdict, for classifying a draw whose pipeline is missing
// from the rewrite cache. Writes happen once per created pipeline; reads happen
// only while the gate is armed. The key shares the word with the value so a
// reader that matches the key is guaranteed to see the matching verdict.
struct GatePipelineRecord
{
    std::atomic<std::uint64_t> key {};
    std::atomic<std::uint64_t> value {};
};

inline constexpr unsigned GatePipelineRecordSlots = 8192;
inline constexpr std::uint64_t GatePipelineRecordMask = GatePipelineRecordSlots - 1;
inline constexpr unsigned GatePipelineReasonShift = 8;
inline constexpr std::uint64_t GatePipelineTransparentBit = 1ull << 7;

inline GatePipelineRecord* GatePipelineRecords() noexcept
{
    static GatePipelineRecord table[GatePipelineRecordSlots];
    return table;
}

inline unsigned GatePipelineRecordSlot(std::uint64_t key) noexcept
{
    auto hash = key * 0x9e3779b97f4a7c15ull;
    hash ^= hash >> 29;
    return unsigned(hash >> 20) & GatePipelineRecordMask;
}

inline void GateRecordCreatedPipeline(const void* pipeline, unsigned reason, bool transparentLooking) noexcept
{
    if (!pipeline)
        return;
    const auto key = std::uint64_t(reinterpret_cast<std::uintptr_t>(pipeline));
    const auto value = (std::uint64_t(reason) << GatePipelineReasonShift) |
                       (transparentLooking ? GatePipelineTransparentBit : 0ull);
    auto* table = GatePipelineRecords();
    auto slot = GatePipelineRecordSlot(key);
    for (unsigned probe = 0; probe < GatePipelineRecordSlots; ++probe)
    {
        auto& entry = table[slot];
        auto current = entry.key.load(std::memory_order_relaxed);
        if (current == key)
        {
            entry.value.store(value, std::memory_order_release);
            return;
        }
        if (current == 0)
        {
            std::uint64_t expected = 0;
            if (entry.key.compare_exchange_strong(expected, key, std::memory_order_acq_rel,
                                                  std::memory_order_relaxed))
            {
                entry.value.store(value, std::memory_order_release);
                return;
            }
            if (expected != key)
                continue;
            entry.value.store(value, std::memory_order_release);
            return;
        }
        slot = (slot + 1) & GatePipelineRecordMask;
    }
}

// Returns reason + 1, or zero when the creation filter never saw this pointer.
inline unsigned GateClassifyCreatedPipeline(const void* pipeline, bool* transparentLooking) noexcept
{
    if (transparentLooking)
        *transparentLooking = false;
    if (!pipeline)
        return 0;
    const auto key = std::uint64_t(reinterpret_cast<std::uintptr_t>(pipeline));
    auto* table = GatePipelineRecords();
    auto slot = GatePipelineRecordSlot(key);
    for (unsigned probe = 0; probe < GatePipelineRecordSlots; ++probe)
    {
        auto& entry = table[slot];
        const auto current = entry.key.load(std::memory_order_acquire);
        if (current == key)
        {
            const auto value = entry.value.load(std::memory_order_acquire);
            if (transparentLooking)
                *transparentLooking = (value & GatePipelineTransparentBit) != 0;
            return unsigned(value >> GatePipelineReasonShift) + 1;
        }
        if (current == 0)
            return 0;
        slot = (slot + 1) & GatePipelineRecordMask;
    }
    return 0;
}
} // namespace GlassFg
