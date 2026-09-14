#include "pch.h"
#include "NvngxDlssgBridge.h"
#include "NativeHost.h"
#include <detours/detours.h>

namespace GlassFg
{
namespace
{
using EvaluateFeatureFn = NVSDK_NGX_Result (*)(ID3D12GraphicsCommandList*, const NVSDK_NGX_Handle*,
                                               const NVSDK_NGX_Parameter*, PFN_NVSDK_NGX_ProgressCallback);

EvaluateFeatureFn originalEvaluate = nullptr;

NVSDK_NGX_Result HookedEvaluate(ID3D12GraphicsCommandList* command, const NVSDK_NGX_Handle* handle,
                                NVSDK_NGX_Parameter* parameters, PFN_NVSDK_NGX_ProgressCallback callback)
{
    if (!originalEvaluate)
        return NVSDK_NGX_Result_FAIL_FeatureNotFound;

    // The host's own native-DLSS branch already runs the correction for this
    // evaluation; handling it twice would duplicate the history and the GPU work.
    if (InsideNativeFgHook())
        return originalEvaluate(command, handle, parameters, callback);

    NvngxDlssgHookCalls().fetch_add(1, std::memory_order_relaxed);
    try
    {
        return EvaluateNativeFG(command, handle, parameters, callback, originalEvaluate);
    }
    catch (...)
    {
        return originalEvaluate(command, handle, parameters, callback);
    }
}
} // namespace

bool InstallNvngxDlssgHook(void* module) noexcept
{
    if (NvngxDlssgHookInstalled())
        return true;
    if (!module)
        return false;
    try
    {
        auto* address = reinterpret_cast<EvaluateFeatureFn>(
            GetProcAddress(static_cast<HMODULE>(module), "NVSDK_NGX_D3D12_EvaluateFeature"));
        if (!address)
            return false;
        originalEvaluate = address;
        if (DetourTransactionBegin() != NO_ERROR)
        {
            originalEvaluate = nullptr;
            return false;
        }
        DetourUpdateThread(GetCurrentThread());
        if (DetourAttach(&(PVOID&)originalEvaluate, HookedEvaluate) != NO_ERROR ||
            DetourTransactionCommit() != NO_ERROR)
        {
            DetourTransactionAbort();
            originalEvaluate = nullptr;
            return false;
        }
        NvngxDlssgHookFlag().store(true, std::memory_order_release);
        return true;
    }
    catch (...)
    {
        originalEvaluate = nullptr;
        return false;
    }
}
} // namespace GlassFg
