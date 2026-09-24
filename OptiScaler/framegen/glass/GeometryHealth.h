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
    GeometryCommandHooks = 64,
    // Grouped instance-array element identity. Without it an array element is
    // keyed by its position in the draw instead of by the engine's own source
    // index, which is what a reordered group update silently breaks.
    GeometryGroupHooks = 128
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
    // Set while the packed capture reports a full vertex-history arena: the
    // object correction is still replacing FG inputs, but a growing share of
    // surfaces can no longer reserve a history block and keeps the engine's
    // motion. This is the state measured live on 2026-09-16 in which covered
    // pixels fell from 6 % of the screen to 0.09 %.
    bool motionDegraded = false;
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
        if (!has(GeometryGroupHooks))
            return "Grouped array element identity hooks unavailable";
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
        if (motionDegraded)
            return "Object MV correction degraded: vertex-history arena full (edges keep background motion)";
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
inline std::atomic<unsigned> motionDegraded = 0;
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
    result.motionDegraded = GeometryTelemetry::motionDegraded.load(std::memory_order_relaxed) != 0;
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
// Published by the once-per-second health refresh and by the host report, from
// the packed capture status the caller already reads.
inline void PublishGeometryMotionDegraded(bool value)
{
    GeometryTelemetry::motionDegraded.store(value ? 1u : 0u, std::memory_order_relaxed);
}

// Native graft path outcomes (engine MotionMatrix supply through a grafted VS).
// Compile outcomes are counted once per pipeline job on the cache worker:
// ready + camera only + missing + refused + rejected + class disabled =
// pipelines that asked for a packed variant, plus the explicit vertex-only jobs
// refused for a refused VS; array ready + array missing = ready; background
// counts the jobs among them that ended with a packed variant for a PS that
// background-ps.bin lists. Draw outcomes are counted by the packed capture's
// prepare, and NativePreviousEvaluations by the FG host for a substituted
// evaluation whose frame drew at least one graft variant while the
// vertex-history fallback was off. Diagnostics only; no render path reads them.
enum GeometryGraftCounter : unsigned
{
    GraftReady,
    GraftMissing,
    GraftRejected,
    GraftClassDisabled,
    // Pipelines whose VS the catalog refused (NativeGraftRefusal, refused.bin):
    // vehicle geometry without the engine's object-motion supply. No packed
    // graft variant; the draws keep the engine's motion. Also counts explicit
    // vertex-only jobs for such a VS, which fail unpublished.
    GraftRefused,
    // Pipelines whose VS has a camera-only catalog record (no native
    // current-position twin) and whose camera variant compiled: it is both
    // `packed` and `packedArray`. A failed compile counts GraftRejected.
    GraftCameraOnly,
    // Pipelines whose PS shows background content (background-ps.bin,
    // NativeGraftCatalog.h IsBackgroundPixelShader) and that have a packed
    // variant: every packed variant of such a pipeline records coverage only
    // (opacity 0), so the interior keeps the engine's motion and only the
    // boundary takes the object's.
    GraftBackground,
    // Graft pipelines that also compiled the camera-only array variant
    // (packedArray), and those whose graft has no camera variant or whose
    // camera variant failed to compile.
    GraftArrayReady,
    GraftArrayMissing,
    // Draws of a graft pipeline whose identity is an array/grouped span or
    // that carries more than one instance, with no camera-only variant (and no
    // vertex-history variant). The engine evaluates MotionMatrix once per proxy
    // (no per-element evaluation in the array append), so one root previous
    // transform would be applied to independently moving elements; the draw
    // keeps the engine's motion instead.
    GraftArrayRejected,
    // Array/multi-instance draws of a graft pipeline drawn with the camera-only
    // variant: the engine's own array convention, previous view-projection
    // applied to each element's current world position.
    GraftArrayDraws,
    // Single-instance draws of a root-graft pipeline drawn with the camera-only
    // variant because the engine's velocity collector gives the owner proxy no
    // object velocity (stalemotion=camera, GlassControls.h): the engine's own
    // convention for such a proxy. Not included in GraftArrayDraws or GraftDraws.
    GraftStaleCameraDraws,
    // Draws of a graft pipeline drawn with the camera-only variant that
    // admitted elements without an engine owner (no span identity, no parent)
    // through a draw-local identity (PackedMotionCapture.cpp prepare). The
    // engine supplies no motion of their own (particles, rain), so this is the
    // camera component only. Not included in GraftArrayDraws or GraftDraws.
    GraftOwnerlessCameraDraws,
    GraftDraws,
    NativePreviousEvaluations,
    // Native declaration hook (NativeMotionDeclarations.cpp), the engine side
    // of the same supply: it makes the engine fill MotionMatrix rows 24..26 for
    // the declared materials. Installed is 1 once both detours committed. Seen
    // counts provider results, Matched the augmented declarations returned,
    // Rejected declared keys whose native record did not match its plan.
    // StageSeen counts native stage resolutions, StageSelected the vertex
    // stages of a declared VS/PS pair.
    DeclarationHookInstalled,
    DeclarationSeen,
    DeclarationMatched,
    DeclarationRejected,
    DeclarationStageSeen,
    DeclarationStageSelected,
    GraftCounterCount
};
namespace GeometryTelemetry
{
inline std::array<std::atomic<std::uint64_t>, GraftCounterCount> grafts {};
} // namespace GeometryTelemetry
inline void NoteGeometryGraft(GeometryGraftCounter counter) noexcept
{
    GeometryTelemetry::grafts[counter].fetch_add(1, std::memory_order_relaxed);
}
inline std::uint64_t ReadGeometryGraft(GeometryGraftCounter counter) noexcept
{
    return GeometryTelemetry::grafts[counter].load(std::memory_order_relaxed);
}
} // namespace GlassFg
