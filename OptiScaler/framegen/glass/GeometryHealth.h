#pragma once
#include <array>
#include <atomic>
#include <cstdint>

namespace GlassFg
{
enum GeometryCapability : unsigned
{
    GeometryStarted = 1,
    GeometryCompiler = 2,
    GeometryCreationHooks = 4,
    GeometryEngineLayout = 8,
    GeometryObjectHooks = 16,
    GeometryDrawHooks = 32,
    GeometryCommandHooks = 64
};
enum GeometryEvidence : unsigned
{
    GeometryRoots,
    GeometryPipelines,
    GeometryCompiled,
    GeometryPending,
    GeometryCompileRejected,
    GeometryIdentities,
    GeometryEngineDraws,
    GeometryPublicDraws,
    GeometryPackets,
    GeometryPipelineMatches,
    GeometryBindingMatches,
    GeometryIndirectKnown,
    GeometryIndirectUnknown,
    GeometryCaptureDraws,
    GeometryFgReplacements,
    GeometryEvidenceCount
};
struct GeometryHealth
{
    unsigned capabilities = 0;
    std::array<std::uint64_t, GeometryEvidenceCount> counts {};
    std::array<std::uint64_t, GeometryEvidenceCount> lastChangeMs {};
    std::uint64_t lastPacketMs = 0, lastFgMs = 0, sampledMs = 0;
    std::uint32_t frame = 0;
    bool has(GeometryCapability capability) const { return (capabilities & capability) != 0; }
    bool recentlyApplied(std::uint64_t now) const
    {
        return (capabilities & 127) == 127 && counts[GeometryCaptureDraws] && counts[GeometryFgReplacements] &&
               lastFgMs && now >= lastFgMs && now - lastFgMs <= 15000;
    }
    const char* reason(std::uint64_t now) const
    {
        if (!has(GeometryStarted))
            return "Waiting for graphics initialization";
        if (!has(GeometryCompiler))
            return "Glass compiler files are missing";
        if (!has(GeometryCreationHooks))
            return "D3D12 creation hooks unavailable";
        if (!has(GeometryEngineLayout))
            return "Engine layout is unsupported or ambiguous";
        if (!has(GeometryObjectHooks))
            return "Engine object hooks unavailable";
        if (!has(GeometryDrawHooks))
            return "Engine draw hooks unavailable";
        if (!has(GeometryCommandHooks))
            return "D3D12 command hooks unavailable";
        if (!counts[GeometryPackets])
            return "Hooks installed; waiting for identified world draws";
        if (!lastPacketMs || now < lastPacketMs || now - lastPacketMs > 15000)
            return "No recent world draw evidence (menu, paused, or unsupported route)";
        if (!counts[GeometryCompiled])
            return "World draws received; waiting for supported shaders";
        if (!counts[GeometryPipelineMatches])
            return "World draws received; no matching capture pipeline";
        if (now - lastChangeMs[GeometryPipelineMatches] > 15000)
            return "World draws are progressing; capture pipeline matches have stopped";
        if (!counts[GeometryBindingMatches])
            return "Capture pipeline found; root bindings not admitted";
        if (now - lastChangeMs[GeometryBindingMatches] > 15000)
            return "World draws are progressing; valid root binding matches have stopped";
        if (!counts[GeometryCaptureDraws])
            return "Inputs observed; object MV capture is not connected";
        if (!counts[GeometryFgReplacements])
            return "Object MV captured; FG input replacement not observed";
        if (!lastFgMs || now < lastFgMs || now - lastFgMs > 15000)
            return "FG replacement was observed, but is not currently progressing";
        return "Object MV inputs are reaching FG (quality remains experimental)";
    }
};

namespace GeometryTelemetry
{
inline std::atomic<unsigned> capabilities = 0;
inline std::array<std::atomic<std::uint64_t>, GeometryEvidenceCount> counts {};
inline std::array<std::atomic<std::uint64_t>, GeometryEvidenceCount> changedMs {};
inline std::atomic<std::uint64_t> packetMs = 0, fgMs = 0, sampledMs = 0;
inline std::atomic<std::uint32_t> frame = 0;
inline std::atomic<void (*)()> refresh = nullptr;
inline std::atomic<std::uint64_t> refreshAttemptMs = 0;
} // namespace GeometryTelemetry

inline void RefreshGeometryHealthIfNeeded(std::uint64_t now)
{
    const auto refresh = GeometryTelemetry::refresh.load(std::memory_order_acquire);
    auto previous = GeometryTelemetry::refreshAttemptMs.load(std::memory_order_relaxed);
    if (refresh && now >= previous && now - previous >= 1000 &&
        GeometryTelemetry::refreshAttemptMs.compare_exchange_strong(previous, now, std::memory_order_relaxed))
        refresh(); // Nonblocking numeric sampling, independent of FG/log success.
}

// Diagnostic evidence only: independent atomic counters, not a GPU ownership
// snapshot. Publication uses the throttled host report or a once/second UI
// refresh with a nonblocking cache lock. No binary scan, driver call or GPU wait.
inline void PublishGeometryHealth(const GeometryHealth& value)
{
    GeometryTelemetry::capabilities.store(value.capabilities, std::memory_order_relaxed);
    for (unsigned i = 0; i < GeometryEvidenceCount; ++i)
    {
        auto previous = GeometryTelemetry::counts[i].load(std::memory_order_relaxed);
        if (i == GeometryPending)
            GeometryTelemetry::counts[i].store(value.counts[i], std::memory_order_relaxed);
        bool advanced = false;
        while (i != GeometryPending && value.counts[i] > previous)
            if (GeometryTelemetry::counts[i].compare_exchange_weak(previous, value.counts[i],
                                                                   std::memory_order_relaxed))
            {
                advanced = true;
                break;
            }
        if (advanced)
        {
            GeometryTelemetry::changedMs[i].store(value.sampledMs, std::memory_order_relaxed);
            if (i == GeometryPackets)
                GeometryTelemetry::packetMs.store(value.sampledMs, std::memory_order_relaxed);
            if (i == GeometryFgReplacements)
                GeometryTelemetry::fgMs.store(value.sampledMs, std::memory_order_relaxed);
        }
    }
    GeometryTelemetry::frame.store(value.frame, std::memory_order_relaxed);
    GeometryTelemetry::sampledMs.store(value.sampledMs, std::memory_order_relaxed);
}
inline GeometryHealth ReadGeometryHealth()
{
    GeometryHealth result;
    result.capabilities = GeometryTelemetry::capabilities.load(std::memory_order_relaxed);
    for (unsigned i = 0; i < GeometryEvidenceCount; ++i)
    {
        result.counts[i] = GeometryTelemetry::counts[i].load(std::memory_order_relaxed);
        result.lastChangeMs[i] = GeometryTelemetry::changedMs[i].load(std::memory_order_relaxed);
    }
    result.lastPacketMs = GeometryTelemetry::packetMs.load(std::memory_order_relaxed);
    result.lastFgMs = GeometryTelemetry::fgMs.load(std::memory_order_relaxed);
    result.sampledMs = GeometryTelemetry::sampledMs.load(std::memory_order_relaxed);
    result.frame = GeometryTelemetry::frame.load(std::memory_order_relaxed);
    return result;
}
} // namespace GlassFg
