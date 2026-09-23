#pragma once
#include <d3d12.h>
#include <nvsdk_ngx.h>
#include <nvsdk_ngx_params.h>
#include <array>
#include <cstdint>

namespace GlassFg
{
using NativeEvaluate = NVSDK_NGX_Result (*)(ID3D12GraphicsCommandList*, const NVSDK_NGX_Handle*,
                                            const NVSDK_NGX_Parameter*, PFN_NVSDK_NGX_ProgressCallback);
NVSDK_NGX_Result EvaluateNativeFG(ID3D12GraphicsCommandList* command, const NVSDK_NGX_Handle* handle,
                                  NVSDK_NGX_Parameter* parameters, PFN_NVSDK_NGX_ProgressCallback callback,
                                  NativeEvaluate original, bool providerFrameGeneration = false,
                                  bool dlssgProviderModule = false, bool frameGenerationCaller = false);
// Bounded record of the identity decision the gate made for an evaluation that
// came in through the DLSS-G provider hook, so a session can tell "the provider
// proved the handle is the generator" from "the alias table had to be refused".
void NoteProviderFrameGenerationIdentity(bool confirmed, unsigned handleId, const char* motionKey) noexcept;
void RetireNativeFG(const NVSDK_NGX_Handle* handle);
void CreatedNativeFG();
void StopNativeFG();
// True only after the process-resident native queue observer is installed.
bool NativeCaptureSubmissionReady() noexcept;
// Live control: retire every native recording and re-admit on the next
// evaluation. Equivalent to a module restart without touching the process.
void RequestNativeSoftReload() noexcept;
// Live diagnostics: one-frame motion/depth dump and its deferred write-out.
void RequestPackedDump() noexcept;
void ServiceNativeDiagnostics() noexcept;
// Snapshot for the live status response: whether the native host is consuming
// FG frames at all, independent of the log.
struct NativeHostStatus
{
    std::uint64_t evaluations = 0, substitutions = 0, captures = 0;
    // Which generated-frame indices reach the proxy at all, and which of them
    // actually received the corrected motion. An index that is evaluated but
    // never substituted keeps the engine's own motion vectors, which is exactly
    // the "transparent object attached to the background" behaviour.
    std::uint64_t evaluationsByIndex[8] {};
    std::uint64_t substitutionsByIndex[8] {};
    // Path 0 carries the DLSS-G parameter names, path 1 the MotionVectors/Depth
    // alias shared with the upscaler and Ray Reconstruction. Only path 0 can be
    // the frame generator, so these separate "correction ran" from "correction
    // ran on the call the generator consumes".
    std::uint64_t evaluationsByPath[2] {};
    std::uint64_t substitutionsByPath[2] {};
    std::uint64_t preparedByPath[2] {};
    // Skips that were harmless (the motion texture was already corrected) and
    // skips that left a frame with the engine's original motion vectors.
    std::uint64_t unsubstitutedReusedMotion = 0, unsubstitutedFreshMotion = 0;
    // Evaluations that were passed through untouched because they are not the
    // frame generator (the upscaler and Ray Reconstruction share the
    // MotionVectors/Depth names). They never reach a session or a parameter
    // swap, so this counter is the direct evidence that the correction stays on
    // DLSS FG alone.
    std::uint64_t nonFrameGenerationEvaluations = 0;
    // Per-evaluation read-back of every parameter name the substitution
    // touched. restoreFailures has to stay 0: a non-zero value means a name was
    // left pointing at our composed texture after the generator returned.
    std::uint64_t restoreChecks = 0, restoreFailures = 0;
    // DLSS-G provider-hook identity: an evaluation whose handle the provider
    // created for NVSDK_NGX_Feature_FrameGeneration. The driver-level parameter
    // table names only MotionVectors/Depth there, so this count is the evidence
    // that the admitted alias evaluations are the generator and not the
    // upscaler or Ray Reconstruction. providerUnconfirmed counts the provider
    // evaluations the gate still had to refuse.
    std::uint64_t providerConfirmedEvaluations = 0, providerUnconfirmedEvaluations = 0;
    // Evaluations the gate admitted because the calling module was the
    // Streamline frame generation plugin. The driver-level table carries no
    // DLSSG.* key and the handle can predate the create hook, so this is the
    // count that separates "the generator was identified" from "the generator
    // never reached the gate" in a configuration where the provider module
    // (nvngx_dlssg.dll) is not loaded at all.
    std::uint64_t callerConfirmedEvaluations = 0;
    unsigned active = 0, retiring = 0, stopped = 0, unavailable = 0;
};
NativeHostStatus ReadNativeHostStatus() noexcept;
// Periodic module log report for the host and the geometry stages. It formats
// dozens of lines and flushes the log, so the host's background thread calls it
// and the frame generation callback (engine render thread) never does.
void ReportNativeHostLog() noexcept;
// Precompiles the packed object-motion shader on a helper thread. The first
// session creation happens inside the frame generation callback when the engine
// rebuilds its lists (game regains focus), where the HLSL compile measured 153ms.
void WarmPackedShaderOnce() noexcept;
// Installs the process-resident D3D12 state observer at device creation. The
// observer is a Detours transaction that suspends every game thread; running it
// there keeps it off the render thread, where it measured 134.9ms inside the
// first frame generation evaluation (the focus-regain path, 2026-09-16 05:46).
bool PreinstallNativeObserver(ID3D12Device* device) noexcept;

// Game-side Streamline handoff, independent of OptiScaler's own frame generation
// setting. The host passes the motion/depth/hudless tags the engine submits for
// the frame; the correction runs the existing packed capture and compose and the
// existing write-back copies the composed pair into those same engine textures.
// No frame generation provider resource or DLL is touched.
struct StreamlineFrame
{
    ID3D12Resource* motion = nullptr;
    ID3D12Resource* depth = nullptr;
    ID3D12Resource* color = nullptr;
    D3D12_RESOURCE_STATES motionState = D3D12_RESOURCE_STATE_COMMON;
    D3D12_RESOURCE_STATES depthState = D3D12_RESOURCE_STATE_COMMON;
    std::uint64_t frame = UINT64_MAX;
    float scaleX = 1.f, scaleY = 1.f, jitterX = 0.f, jitterY = 0.f;
    std::array<float, 16> clipToPrevious {};
    unsigned index = 1, count = 1, reset = 0;
};
bool CorrectStreamlineFrame(ID3D12GraphicsCommandList* command, const void* featureKey,
                            const StreamlineFrame& frame) noexcept;
// Bounded observation of the feature ids the host's Streamline evaluate hook
// sees, so the frame generation id of this engine build can be identified.
void NoteStreamlineFeature(unsigned id) noexcept;
// Bounded observation of the NGX evaluate calls the host proxy sees: feature id
// and handle id, so the correction can match the real call.
void NoteNgxFeature(unsigned feature, unsigned handleId, const char* provider) noexcept;
// Bounded observation of the feature creations the proxy performs: feature id,
// handle id and which route created it.
void NoteNgxCreate(unsigned feature, unsigned handleId, const char* route) noexcept;
// Bounded observation of nvngx library loads the loader hook sees, so a host
// that never reaches the proxy can be told apart from one that never loads.
void NoteNvngxLoad(const wchar_t* name, bool redirect) noexcept;
// Bounded record of a failed NGX evaluate-hook installation: module and stage.
void NoteHookStage(const wchar_t* name, unsigned stage) noexcept;
// Bounded record of the provider-level frame generation handle: the creation
// pointer the create hook registered and the pointer the evaluate hook looked
// up. A mismatch between the two is what makes the identity gate refuse a
// generator evaluation whose parameter table carries no DLSSG.* keys.
void NoteFrameGenerationHandle(bool creation, const void* handle, unsigned handleId, bool confirmed) noexcept;
// Bounded record of the module that issued a driver-level NGX evaluation. The
// identity of a frame generation evaluation depends on it when the provider
// module is not the dedicated DLSS-G library, so the raw caller path has to be
// readable from the log instead of inferred.
void NoteFrameGenerationCaller(const void* address, const void* handle, unsigned handleId,
                               bool classified) noexcept;
// Process lifetime diagnostics: the module log records the attach, the detach
// and the first unhandled exception, so a session that ends can be classified
// as a clean exit, a crash or an external termination.
void InstallProcessDiagnostics() noexcept;
void NoteProcessAttach() noexcept;
void NoteProcessDetach() noexcept;
} // namespace GlassFg
