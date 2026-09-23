#pragma once
// Deliberately free of the NGX/D3D12 headers: the settings panel and the
// settings test include this for the hook state only.
#include <atomic>
#include <cstdint>

namespace GlassFg
{
// Streamline drives its frame generation directly: sl.dlss_g.dll calls
// nvngx_dlssg.dll!NVSDK_NGX_D3D12_EvaluateFeature without passing the host's NGX
// proxy, and the loader hook hands the real module back unchanged so the MFG
// unlocker can patch it. This detour routes that evaluation through the same
// EvaluateNativeFG entry point.
//
// Idempotent per module. module is a loaded NGX provider (nvngx_dlssg.dll from
// the game folder, or the driver's _nvngx.dll which the Streamline over-the-air
// plugin evaluates through). false means the export was missing, the module is
// this module itself, or the detour transaction failed.
bool InstallNgxEvaluateHook(void* module) noexcept;
// Hook every NGX provider that is already loaded. Safe to call repeatedly.
void InstallLoadedNgxHooks() noexcept;
// True when the provider created this handle for
// NVSDK_NGX_Feature_FrameGeneration, or when the module is the dedicated
// nvngx_dlssg.dll provider. The driver-level DLSS-G evaluation names only
// MotionVectors/Depth, which the upscaler and Ray Reconstruction share, so the
// parameter table cannot prove the identity; the provider hook can.
bool ProviderConfirmsFrameGeneration(const void* handle, bool dedicatedProvider) noexcept;

// Identity fallback for the driver-level DLSS-G evaluation, which names only
// MotionVectors/Depth and can carry a handle that was created before this hook
// installed. Which module issued the evaluation is then the remaining proof:
// Streamline's frame generation plugin only evaluates the generator.
// 0 = off, 1 = only the frame generation plugin (default), 2 = every caller.
// Mode 2 exists for a bounded live experiment from the control channel; it is
// never written to the INI and never enabled by default.
void SetFrameGenerationCallerMode(unsigned mode) noexcept;
unsigned FrameGenerationCallerMode() noexcept;

// Header-only state: the settings panel reads it and the GPU fixtures must not
// need the bridge object file to link.
inline std::atomic<bool>& NvngxDlssgHookFlag() noexcept
{
    static std::atomic<bool> value { false };
    return value;
}
inline bool NvngxDlssgHookInstalled() noexcept
{
    return NvngxDlssgHookFlag().load(std::memory_order_acquire);
}
inline std::atomic<std::uint64_t>& NvngxDlssgHookCalls() noexcept
{
    static std::atomic<std::uint64_t> value { 0 };
    return value;
}
// True once the create/release pair of a DLSS-G provider is wrapped too. The
// create hook is what carries the frame-generation identity of a handle whose
// evaluation only names MotionVectors/Depth, so a session with the evaluate
// hook installed but this flag false cannot prove the identity from the
// provider side.
inline std::atomic<bool>& NvngxDlssgCreateHookFlag() noexcept
{
    static std::atomic<bool> value { false };
    return value;
}

// Marks the thread that is already inside the host's native DLSS-G branch, so a
// nested nvngx_dlssg evaluation is not handled a second time.
inline bool& InsideNativeFgHook() noexcept
{
    static thread_local bool value = false;
    return value;
}
struct NativeFgScope
{
    NativeFgScope() noexcept { InsideNativeFgHook() = true; }
    ~NativeFgScope() noexcept { InsideNativeFgHook() = false; }
    NativeFgScope(const NativeFgScope&) = delete;
    NativeFgScope& operator=(const NativeFgScope&) = delete;
};
} // namespace GlassFg
