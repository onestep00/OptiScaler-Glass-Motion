#pragma once
// Deliberately free of the NGX/D3D12 headers: the settings panel and the
// settings test include this for the hook state only.
#include <atomic>
#include <cstdint>

namespace GlassFg
{
// Streamline drives its frame generation directly: sl.dlss_g.dll calls
// nvngx_dlssg.dll!NVSDK_NGX_D3D12_EvaluateFeature without passing the host's NGX
// proxy, which is the only place the correction hook was installed. This detour
// routes that evaluation through the same EvaluateNativeFG entry point, so the
// Streamline FG path gets the object-motion correction as well.
//
// Idempotent. module is the loaded nvngx_dlssg.dll; false means the export was
// missing or the detour transaction failed and the process is left untouched.
bool InstallNvngxDlssgHook(void* module) noexcept;

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
