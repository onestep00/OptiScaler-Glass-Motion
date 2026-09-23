#pragma once
#include <algorithm>
#include <atomic>
#include <cstdint>

namespace GlassFg
{
struct Controls
{
    // Product defaults (2026-09-23): the module is on, the compose covers the
    // whole render height and the FG inputs are replaced. A missing INI must
    // give the shipped behaviour, not a staging configuration.
    bool enabled = true;
    // Interior opacity threshold in percent. A covered pixel takes the object's
    // own motion and depth when its material opacity reaches this value; below
    // it the engine's motion and depth stay untouched. The visible boundary
    // always takes the exact object motion.
    unsigned opacityPercent = 50;
    bool measureGpuTime = true;
    unsigned edgeWidth = 2;
    // Safety staging for the full-screen packed object-motion dispatch. The
    // compose clamps packedRows to the render height, so the default covers
    // every row at any resolution (C15); the staging ladder lowers it.
    bool packedDispatch = true;
    unsigned packedRows = 32768;
    // Isolation staging: running the packed dispatch without swapping the FG
    // inputs attributes a driver reset to the new GPU work or to the NGX input
    // replacement. Off only during that ladder.
    bool packedSubstitute = true;
    // Diagnostic stage: per-step trace lines for attribution. Costs one
    // fprintf/fflush per step and is off by default.
    bool trace = false;
    // Automatic staged ramp: 0-20 s dispatch on/swap off at 240 rows,
    // 20-40 s full rows, then the FG input swap. Off by default.
    bool autoStage = false;
    // Isolation switch: keep the input copies and the FG swap but skip the
    // compose compute dispatch, so the swap can be tested with zero new GPU work.
    bool packedCompute = true;
    // Diagnostic: run the compose dispatch without reading the packed object
    // records, so a GPU stall can be attributed to the dispatch itself or to
    // the record read that the capture raster wrote.
    bool packedSkipRead = false;
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
    // Packet-local instanced packets (the engine's non-global selection path,
    // used by particles and other instanced transparency) expose no source
    // index either. When enabled, the packet ordinal is used as the element
    // key guarded by the array's observed lifetime generation. Off by default:
    // the order probe has to show that the packet order is stable first.
    bool packetLocalOrder = false;
    // Supply the composed motion and depth through the parameter table the
    // provider reads, instead of swapping the table entries around the call.
    // The provider copies input pointers on some evaluations and reuses that
    // copy on the others, so the read-time answer is what reaches generation.
    bool packedSupply = false;

    // Supply the documented DLSS-G transparency-layer inputs (the composed
    // motion and the packed coverage as opacity) so the generator treats the
    // transparent surface as its own layer instead of letting the content
    // behind it drive the region.
    bool packedLayer = false;

    // Projection-jitter compensation for the captured object motion. Measured
    // 2026-09-16: the injected vector differs from the engine's own motion by
    // one vector shared by every object in the frame (20 of 20 object groups,
    // spread 0.05 px), i.e. a screen-space offset, not object motion. A
    // sub-pixel camera jitter that the material path cancels differently from
    // the engine's opaque path produces exactly that. 0 = off, 1 = -previous,
    // 2 = +previous, 3 = +current, 4 = -current, 5 = current - previous. Gain
    // scales it in percent.
    // Measured 2026-09-18 (delta wiring absent, per-pixel fixed effects with
    // the engine motion as the camera proxy, 406,615 px x 14 frames, mode 0):
    // the captured vector carried the current frame's jitter and not the
    // predecessor's, a(J) = -1.105 [-1.137,-1.043] b(P) = +0.008 on x, a(J) =
    // -1.142 b(P) = -0.067 on y, while the engine's own field is jitter
    // independent. That is why mode 3 compensated the capture in that build.
    // 2026-09-19: the capture endpoint now adds the frame-to-frame constant
    // delta, so the captured record is jitter free and mode 3 double counts the
    // term. Measured on the delta build (24 frames x 2, still and moving): the
    // delivered-minus-engine field has no jitter dependence at mode 0, a(J) x
    // +0.004 y +0.033 with a 0.023 px boundary step, while mode 3 re-adds
    // +0.887*J with a 0.348 px boundary step. Product default: 0. Modes 1-5 stay
    // reachable only through the live diagnostic channel for re-measurement.
    unsigned jitterMode = 0;
    unsigned jitterGain = 100;

    // Diagnostic: deliver an all-zero motion field to the frame generator
    // instead of the engine's or the captured motion. Depth is left as the
    // engine wrote it. This is the "no motion vectors at all" baseline for FG
    // artifact comparison; it never touches the game's own motion texture.
    bool zeroMotion = false;

    // Second consumer. Off by default: the composed motion reaches only the
    // frame generator through the input substitution. When on, the DLSS-NR
    // pass (DLSS 5 neural rendering) is handed the composed motion and depth
    // through its own evaluate inputs. The engine's own textures are never
    // written, so DLSS-SR, Ray Reconstruction and the ray traced passes keep
    // reading exactly what the game produced.
    bool nrMotion = false;

    // Far-surface skip, in steps of 25 m of view distance. 0 keeps every
    // surface. Distant level-of-detail glass carries no detail the generator
    // can act on, and its capture costs the same as a near one, so the capture
    // shader drops records whose previous-frame clip w (view distance, in the
    // same units as the world) is at or beyond this distance. The units are
    // world units; the game's world unit is one metre.
    unsigned farSkipStep = 0;

    // Diagnostic: one line per compose carrying the (jitter, previous) pair the
    // compose actually used. The dump interval is 36-37 frames, so a dump header
    // cannot say which pair produced a captured vector; this line can, which is
    // what separates a wrong frame offset from a wrong jitter term in the
    // capture residual. One fprintf per compose, off by default.
    bool jitterLog = false;

    // Engine-proximity gate, diagnostic only. When on, the compose still
    // selects the object's own motion for a covered pixel but writes nothing
    // when that motion is already within EngineGatePx of the value the engine
    // put there. It is off by default because the condition set is a binary
    // blend of engine and object motion (C21, C3, C4): the border and every
    // covered pixel take the object's motion, and any skip leaves the pixel on
    // the background value the user reports as sticking. The 2026-09-19
    // Songbird mirror band A/B that motivated the gate (`tvar_band` +1.31 luma
    // and `hptvar_band_hi` +1.61 on ceilband1, +2.66 / +4.58 on ceilband2)
    // measured `substitute=off` against mode 3, and the later gate-on A/B in
    // the same band is neutral inside its noise (railgate2 `j0g - off` +0.16
    // tvar / +0.33 hptvar_hi with round spread of about 5-10 luma). The live
    // diagnostic channel opens it (`enggate=on`) and resizes it (`gatepx=`).
    bool engineGate = false;
    // Gate radius in 1/16 px steps, 0..7 (2 = 0.125 px, the packing quantum).
    unsigned engineGateSteps = 2;

    bool active() const { return enabled; }
    float opacityThreshold() const { return std::min(opacityPercent, 100u) / 100.f; }
    float engineGatePx() const { return float(std::min(engineGateSteps, 7u)) / 16.f; }
    // 0 = off. Otherwise the smallest distance the capture shader rejects.
    float farCutoffMeters() const { return farSkipStep ? 25.f * float(farSkipStep) : 0.f; }
    uint64_t packed() const
    {
        return (std::uint64_t(std::min(opacityPercent, 100u)) << 1) | (enabled ? 1u : 0u) |
               (measureGpuTime ? 256u : 0u) | (std::uint64_t(std::clamp(edgeWidth, 1u, 4u)) << 9) |
               (packedDispatch ? (std::uint64_t(1) << 12) : 0u) |
               (std::uint64_t((std::min)(packedRows, 0xffffu)) << 13) |
               (packedSubstitute ? (std::uint64_t(1) << 29) : 0u) | (trace ? (std::uint64_t(1) << 30) : 0u) |
               (autoStage ? (std::uint64_t(1) << 31) : 0u) | (packedCompute ? (std::uint64_t(1) << 32) : 0u) |
               (packedSkipRead ? (std::uint64_t(1) << 36) : 0u) |
               (compilePipelines ? (std::uint64_t(1) << 37) : 0u) |
               (groupedOrder ? (std::uint64_t(1) << 38) : 0u) |
               (arrayProbe ? (std::uint64_t(1) << 39) : 0u) |
               (packetLocalOrder ? (std::uint64_t(1) << 40) : 0u) |
               (packedSupply ? (std::uint64_t(1) << 41) : 0u) |
               (packedLayer ? (std::uint64_t(1) << 42) : 0u) |
               (std::uint64_t(std::min(jitterMode, 7u)) << 43) |
               (std::uint64_t(std::min(jitterGain, 255u)) << 46) |
               (zeroMotion ? (std::uint64_t(1) << 35) : 0u) |
               (nrMotion ? (std::uint64_t(1) << 55) : 0u) |
               (jitterLog ? (std::uint64_t(1) << 54) : 0u) |
               (engineGate ? (std::uint64_t(1) << 56) : 0u) |
               (std::uint64_t(std::min(engineGateSteps, 7u)) << 57) |
               (std::uint64_t(std::min(farSkipStep, 15u)) << 60);
    }
    static Controls unpack(std::uint64_t value)
    {
        const auto edge = unsigned((value >> 9) & 7u);
        return { (value & 1u) != 0, std::min(unsigned((value >> 1) & 127u), 100u), (value & 256u) != 0,
                 std::clamp(edge ? edge : 2u, 1u, 4u), (value & (1ull << 12)) != 0,
                 unsigned((value >> 13) & 0xffffu), (value & (1ull << 29)) != 0, (value & (1ull << 30)) != 0,
                 (value & (1u << 31)) != 0, (value & (std::uint64_t(1) << 32)) != 0,
                 (value & (std::uint64_t(1) << 36)) != 0, (value & (std::uint64_t(1) << 37)) != 0,
                 (value & (std::uint64_t(1) << 38)) != 0, (value & (std::uint64_t(1) << 39)) != 0,
                 (value & (std::uint64_t(1) << 40)) != 0, (value & (std::uint64_t(1) << 41)) != 0,
                 (value & (std::uint64_t(1) << 42)) != 0, unsigned((value >> 43) & 7u),
                 unsigned((value >> 46) & 0xffu),
                 (value & (std::uint64_t(1) << 35)) != 0,
                 (value & (std::uint64_t(1) << 55)) != 0,
                 unsigned((value >> 60) & 0xfu),
                 (value & (std::uint64_t(1) << 54)) != 0,
                 (value & (std::uint64_t(1) << 56)) != 0,
                 unsigned((value >> 57) & 7u) };
    }
};

// UI and the native host share one atomic snapshot. No INI reads per FG call.
Controls ReadControls();
void WriteControls(Controls value);
// Session-only diagnostic, live channel only. Not persisted and not part of
// the packed control word. When on, the compose substitutes the object's own
// motion but leaves the depth the engine wrote under the pixel. The
// transparent pass writes no depth of its own, so the engine's value describes
// the content behind the surface while the substituted motion describes the
// surface itself. The A/B separates "the generator resolves occlusion against
// the substituted depth" from "the substituted motion alone still ripples the
// mirror band". Default off keeps the delivered behaviour.
inline std::atomic<bool>& DepthKeepFlag() noexcept
{
    static std::atomic<bool> value { false };
    return value;
}
inline void SetDepthKeep(bool enabled) noexcept { DepthKeepFlag().store(enabled, std::memory_order_relaxed); }
inline bool DepthKeepEnabled() noexcept { return DepthKeepFlag().load(std::memory_order_relaxed); }
// Frame depth convention declared by the frame generation provider
// (DLSSG.DepthInverted), published by the FG host before each compose so the
// opaque-occlusion test compares the object's record and the engine depth in
// the same direction. Default 1: Cyberpunk declares reverse-Z.
inline std::atomic<unsigned>& FrameDepthInvertedValue() noexcept
{
    static std::atomic<unsigned> value { 1u };
    return value;
}
inline void SetFrameDepthInverted(unsigned inverted) noexcept
{
    FrameDepthInvertedValue().store(inverted ? 1u : 0u, std::memory_order_relaxed);
}
inline bool FrameDepthInverted() noexcept { return FrameDepthInvertedValue().load(std::memory_order_relaxed) != 0; }
// Diagnostic opaque-pipeline probe, off by default and persisted in the INI as
// GlassFG/OpaqueProbe. Session value only: not part of the packed control word.
// When on, the pipeline cache admits the depth-writing and unblended pipelines
// it normally refuses, so the capture can be pointed at the opaque surfaces the
// user asked to compare against the engine's own motion. The compiler half of
// the same flag decides whether such a pipeline can be rewritten without losing
// its colour exports or its SV_Depth export; an unblended target is treated as
// a fully opaque surface (weight 255) because the game's blend state writes the
// source colour directly.
inline std::atomic<bool>& OpaqueProbeFlag() noexcept
{
    static std::atomic<bool> value { false };
    return value;
}
inline void SetOpaqueProbe(bool enabled) noexcept { OpaqueProbeFlag().store(enabled, std::memory_order_relaxed); }
inline bool OpaqueProbeEnabled() noexcept { return OpaqueProbeFlag().load(std::memory_order_relaxed); }
// Vertex-history fallback for the packed capture, off by default and persisted
// in the INI as GlassFG/VertexHistoryFallback; live vhfallback=on|off. Session
// value only, like OpaqueProbe.
// Off: a packed pipeline takes the previous clip from the engine's own
// MotionMatrix supply through a grafted vertex shader, and a pipeline without
// an enabled graft gets no packed variant (the engine's motion and depth stay).
// On: pipelines without a graft, and graft pipelines drawn as arrays, use the
// module's vertex history instead (C2 does not accept this as the default).
// The compiler reads it when a pipeline is compiled, so a change applies to
// pipelines the game creates afterwards.
inline std::atomic<bool>& VertexHistoryFallbackFlag() noexcept
{
    static std::atomic<bool> value { false };
    return value;
}
inline void SetVertexHistoryFallback(bool enabled) noexcept
{
    VertexHistoryFallbackFlag().store(enabled, std::memory_order_relaxed);
}
inline bool VertexHistoryFallbackEnabled() noexcept
{
    return VertexHistoryFallbackFlag().load(std::memory_order_relaxed);
}
// Graft supply classes admitted at pipeline compile time (INI
// GlassFG/GraftClassMask, live graftclass=<n>, default 1). Bit 0: the previous
// graph reads only the MotionMatrix and camera rows (root transform). Bit 1: it
// also reads skinning inputs / the t10 bone buffer. Bit 2: it reads t9/b3
// preskinned previous vertices. A graft is admitted when every bit of its class
// is enabled; otherwise it counts GraftClassDisabled and the pipeline is
// treated as a graft miss. Applies to pipelines compiled afterwards.
inline constexpr unsigned GraftClassRootOnly = 1u, GraftClassSkinning = 2u, GraftClassPreskinned = 4u,
                          GraftClassAll = 7u;
inline std::atomic<unsigned>& GraftClassMaskValue() noexcept
{
    // Root-only by default. Class 2 (skinning) was tried as the default on
    // 2026-09-23 (build 0c5bb34e): the transparent pass does not carry the
    // velocity pass's previous skinning supply, so a skinned hair/glasses draw
    // delivered ~77 px where the engine had ~3.5 px (p6-20260923b-on/still).
    static std::atomic<unsigned> value { GraftClassRootOnly };
    return value;
}
inline void SetGraftClassMask(unsigned mask) noexcept
{
    GraftClassMaskValue().store(mask & GraftClassAll, std::memory_order_relaxed);
}
inline unsigned ReadGraftClassMask() noexcept { return GraftClassMaskValue().load(std::memory_order_relaxed); }
inline bool GraftClassEnabled(unsigned supplyClass) noexcept
{
    return supplyClass && !(supplyClass & ~ReadGraftClassMask());
}
// Simultaneous stripe A/B for the delivered frame, off by default. The compose
// keeps the object's motion in even 64-pixel column bands and leaves the
// engine's value in odd bands, so both arms sample the same pan, the same
// animation phase and the same frames. A temporal metric can then compare them
// inside one capture instead of across two runs. Session-only diagnostic: not
// part of the packed control word.
inline std::atomic<bool>& StripeProbeFlag() noexcept
{
    static std::atomic<bool> value { false };
    return value;
}
inline void SetStripeProbe(bool enabled) noexcept { StripeProbeFlag().store(enabled, std::memory_order_relaxed); }
inline bool StripeProbeEnabled() noexcept { return StripeProbeFlag().load(std::memory_order_relaxed); }
// Engine MotionMatrix probe, live motionprobe=<hex>|off, diagnostic only and off
// by default. <hex> is a vertex-shader hash prefix of 1..16 hex digits,
// compared with the top bits of GeometryPipelineEntry::vertexHash (the %016llx
// value the coverage report prints). Draws of a matching pipeline log what the
// engine's MotionMatrix supply gave them (MOTION_PROBE lines,
// PackedMotionCapture.cpp). Session value only. The prefix is written before
// the digit count and read after it, so a reader never pairs a new count with
// an old prefix.
inline std::atomic<std::uint64_t>& MotionProbePrefixValue() noexcept
{
    static std::atomic<std::uint64_t> value { 0 };
    return value;
}
inline std::atomic<unsigned>& MotionProbeDigitsValue() noexcept
{
    static std::atomic<unsigned> value { 0 };
    return value;
}
// digits 0 turns the probe off. prefix holds the digits right-aligned.
inline void SetMotionProbe(std::uint64_t prefix, unsigned digits) noexcept
{
    digits = std::min(digits, 16u);
    MotionProbeDigitsValue().store(0, std::memory_order_release);
    MotionProbePrefixValue().store(digits ? prefix << (64 - 4 * digits) : 0, std::memory_order_relaxed);
    MotionProbeDigitsValue().store(digits, std::memory_order_release);
}
inline unsigned MotionProbeDigits() noexcept { return MotionProbeDigitsValue().load(std::memory_order_acquire); }
inline bool MotionProbeArmed() noexcept { return MotionProbeDigitsValue().load(std::memory_order_relaxed) != 0; }
inline bool MotionProbeMatches(std::uint64_t vertexHash) noexcept
{
    const auto digits = MotionProbeDigits();
    if (!digits)
        return false;
    const auto shift = 64 - 4 * digits;
    return (vertexHash >> shift) == (MotionProbePrefixValue().load(std::memory_order_relaxed) >> shift);
}
// Stale MotionMatrix rule, live stalemotion=camera|off, default camera. A root
// graft takes its previous clip from the engine's MotionMatrix rows 24..26. The
// engine writes a previous pose there only for a proxy whose transform history
// record is in state <= 1 with a nonzero motion weight (supplier 0x56c194), and
// it draws a rigid proxy in its velocity pass only in state 1 (gate 0x1e9228).
// Every other proxy keeps the camera-only velocity of the velocity
// initialization (EngineMotionSupply.md "Proxy history convention").
// Camera: a single-instance root-graft draw whose owner proxy has no supplied
// previous pose and no motion flag takes the camera-only variant, the engine's
// own convention for that proxy. Off: the draw keeps the root graft whatever
// the rows hold, for A/B. Session value only.
inline std::atomic<bool>& StaleMotionCameraFlag() noexcept
{
    static std::atomic<bool> value { true };
    return value;
}
inline void SetStaleMotionCamera(bool enabled) noexcept
{
    StaleMotionCameraFlag().store(enabled, std::memory_order_relaxed);
}
inline bool StaleMotionCameraEnabled() noexcept { return StaleMotionCameraFlag().load(std::memory_order_relaxed); }
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
enum class RuntimeStatus : unsigned { Waiting, Correcting, Unavailable, Retiring, Stopped };
void PublishRuntimeStatus(RuntimeStatus status);
} // namespace GlassFg
