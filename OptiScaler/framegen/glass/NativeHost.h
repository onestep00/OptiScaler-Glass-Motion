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
                                  NativeEvaluate original);
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
    // Skips that were harmless (the motion texture was already corrected) and
    // skips that left a frame with the engine's original motion vectors.
    std::uint64_t unsubstitutedReusedMotion = 0, unsubstitutedFreshMotion = 0;
    unsigned active = 0, retiring = 0, stopped = 0, unavailable = 0;
};
NativeHostStatus ReadNativeHostStatus() noexcept;

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
// Process lifetime diagnostics: the module log records the attach, the detach
// and the first unhandled exception, so a session that ends can be classified
// as a clean exit, a crash or an external termination.
void InstallProcessDiagnostics() noexcept;
void NoteProcessAttach() noexcept;
void NoteProcessDetach() noexcept;
} // namespace GlassFg
