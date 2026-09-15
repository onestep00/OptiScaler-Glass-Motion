#pragma once
#include <algorithm>
#include <atomic>
#include <cstdint>

namespace GlassFg
{
struct Controls
{
    bool enabled = false;
    unsigned strength = 100;
    bool measureGpuTime = true;
    unsigned edgeWidth = 2;
    // Safety staging for the full-screen packed object-motion dispatch.
    bool packedDispatch = true;
    unsigned packedRows = 240;
    // Isolation staging: run the packed dispatch without swapping the FG
    // inputs, so a driver reset can be attributed to the new GPU work or to
    // the NGX input replacement instead of both at once.
    bool packedSubstitute = false;
    // Diagnostic stage: per-step trace lines for attribution. Costs one
    // fprintf/fflush per step and is off by default.
    bool trace = false;
    // Automatic staged ramp: 0-20 s dispatch on/swap off at 240 rows,
    // 20-40 s full rows, then the FG input swap. Off by default.
    bool autoStage = false;
    // Isolation switch: keep the input copies and the FG swap but skip the
    // compose compute dispatch, so the swap can be tested with zero new GPU work.
    bool packedCompute = true;
    // Integration without foreign resources: copy the composed motion/depth
    // back into the game's own FG inputs instead of substituting parameters.
    bool packedWriteBack = false;
    // Diagnostic: run the compose dispatch without reading the packed object
    // records, so a GPU stall can be attributed to the dispatch itself or to
    // the record read that the capture raster wrote.
    bool packedSkipRead = false;
    // Consume the live plugin's grouped-array element mapping for element
    // identity. Off until the published list is verified.
    bool arrayMapping = false;
    // Bisect switch: when false the module still observes draws and FG frames
    // but never creates its rewritten pipelines. Used to separate the D3D12
    // hooks from the pipeline-creation path in the 2026-09-14 resets.
    bool compilePipelines = true;
    // Grouped update arrays (flag 0x2000) have no engine-verified source order
    // at the append site. When enabled, the packet ordinal is used as the
    // element position and the array's observed lifetime generation is the
    // guard: any mutation of the array invalidates the element history.
    bool groupedOrder = true;
    // Diagnostic: compare the element bytes a grouped array packet receives
    // between consecutive frames for the same array. An unchanged element set
    // in a changed position is a permutation, which is the condition under
    // which a packet ordinal stops being the element's previous-frame key.
    bool arrayProbe = false;

    bool active() const { return enabled && strength > 0; }
    float coverage() const { return std::min(strength, 100u) / 100.f; }
    uint64_t packed() const
    {
        return (std::uint64_t(std::min(strength, 100u)) << 1) | (enabled ? 1u : 0u) |
               (measureGpuTime ? 256u : 0u) | (std::uint64_t(std::clamp(edgeWidth, 1u, 4u)) << 9) |
               (packedDispatch ? (std::uint64_t(1) << 12) : 0u) |
               (std::uint64_t((std::min)(packedRows, 0xffffu)) << 13) |
               (packedSubstitute ? (std::uint64_t(1) << 29) : 0u) | (trace ? (std::uint64_t(1) << 30) : 0u) |
               (autoStage ? (std::uint64_t(1) << 31) : 0u) | (packedCompute ? (std::uint64_t(1) << 32) : 0u) |
               (packedWriteBack ? (std::uint64_t(1) << 33) : 0u) |
               (arrayMapping ? (std::uint64_t(1) << 34) : 0u) |
               (packedSkipRead ? (std::uint64_t(1) << 36) : 0u) |
               (compilePipelines ? (std::uint64_t(1) << 37) : 0u) |
               (groupedOrder ? (std::uint64_t(1) << 38) : 0u) |
               (arrayProbe ? (std::uint64_t(1) << 39) : 0u);
    }
    static Controls unpack(std::uint64_t value)
    {
        const auto edge = unsigned((value >> 9) & 7u);
        return { (value & 1u) != 0, std::min(unsigned((value >> 1) & 127u), 100u), (value & 256u) != 0,
                 std::clamp(edge ? edge : 2u, 1u, 4u), (value & (1ull << 12)) != 0,
                 unsigned((value >> 13) & 0xffffu), (value & (1ull << 29)) != 0, (value & (1ull << 30)) != 0,
                 (value & (1u << 31)) != 0, (value & (std::uint64_t(1) << 32)) != 0,
                 (value & (std::uint64_t(1) << 33)) != 0, (value & (std::uint64_t(1) << 36)) != 0,
                 (value & (std::uint64_t(1) << 34)) != 0, (value & (std::uint64_t(1) << 37)) != 0,
                 (value & (std::uint64_t(1) << 38)) != 0, (value & (std::uint64_t(1) << 39)) != 0 };
    }
};

// UI and the native host share one atomic snapshot. No INI reads per FG call.
Controls ReadControls();
void WriteControls(Controls value);
// Bisect switch shared with the pipeline cache. Inline so every build target
// (module, settings test, GPU fixtures) resolves it without extra linkage.
inline std::atomic<bool>& GeometryPipelineCompilationFlag() noexcept
{
    static std::atomic<bool> value { true };
    return value;
}
inline void SetGeometryPipelineCompilation(bool enabled) noexcept
{
    GeometryPipelineCompilationFlag().store(enabled, std::memory_order_relaxed);
}
inline bool GeometryPipelineCompilationEnabled() noexcept
{
    return GeometryPipelineCompilationFlag().load(std::memory_order_relaxed);
}
void RenderSettings();
void PublishGpuMilliseconds(double milliseconds);
// Last completed asynchronous GPU sample, in milliseconds. Negative when the
// timer has not produced a sample yet (disabled or waiting).
double ReadGpuMilliseconds() noexcept;
// Settings-panel view of the live FG integration. The host publishes a few
// relaxed counters per evaluation; the panel reads them without a lock, and the
// compose fence pair is the direct evidence that our own GPU work finished.
enum LiveStatusField
{
    LiveStatusEvaluations,
    LiveStatusSubstitutions,
    LiveStatusComposeSubmitted,
    LiveStatusComposeCompleted,
    LiveStatusComposeForced,
    LiveStatusComposeInFlight,
    LiveStatusUnavailable,
    LiveStatusRetiring,
    LiveStatusFieldCount
};
inline std::atomic<std::uint64_t>& LiveStatusSlot(unsigned field) noexcept
{
    static std::atomic<std::uint64_t> slots[LiveStatusFieldCount] {};
    return slots[field < LiveStatusFieldCount ? field : 0u];
}
inline void PublishLiveStatus(LiveStatusField field, std::uint64_t value) noexcept
{
    LiveStatusSlot(field).store(value, std::memory_order_relaxed);
}
inline std::uint64_t ReadLiveStatus(LiveStatusField field) noexcept
{
    return LiveStatusSlot(field).load(std::memory_order_relaxed);
}
// Frames the correction prepared from the engine's DLSS-G tag handoff. Header
// only so the settings panel and its test do not need the native host headers.
inline std::atomic<std::uint64_t>& StreamlineFrameCounter() noexcept
{
    static std::atomic<std::uint64_t> value { 0 };
    return value;
}
inline std::uint64_t ReadStreamlineFrameCalls() noexcept
{
    return StreamlineFrameCounter().load(std::memory_order_relaxed);
}
enum class RuntimeStatus : unsigned { Waiting, Correcting, Unavailable, Retiring, Stopped };
void PublishRuntimeStatus(RuntimeStatus status);
} // namespace GlassFg
