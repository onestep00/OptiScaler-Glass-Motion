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
} // namespace GlassFg
