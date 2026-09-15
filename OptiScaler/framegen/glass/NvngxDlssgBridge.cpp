#include "pch.h"
#include "NvngxDlssgBridge.h"
#include "NativeHost.h"
#include <detours/detours.h>

namespace GlassFg
{
namespace
{
// Matches the real export: the NGX API hands the parameter block over as a
// mutable pointer.
using EvaluateFeatureFn = NVSDK_NGX_Result (*)(ID3D12GraphicsCommandList*, const NVSDK_NGX_Handle*,
                                               NVSDK_NGX_Parameter*, PFN_NVSDK_NGX_ProgressCallback);

EvaluateFeatureFn originalEvaluate = nullptr;

// The NGX provider is loaded and unloaded per initialisation and can come from
// the game folder (nvngx_dlssg.dll) or the driver (_nvngx.dll), each reload
// landing on a different base address. Detours needs one trampoline per target,
// so the slots are pooled instead of latching a single address.
constexpr unsigned kMaxHookSlots = 8;
struct HookSlot
{
    void* module = nullptr;
    EvaluateFeatureFn original = nullptr;
    const char* origin = nullptr;
};
HookSlot slots[kMaxHookSlots] {};

NVSDK_NGX_Result HookedImpl(EvaluateFeatureFn original, const char* origin, ID3D12GraphicsCommandList* command,
                            const NVSDK_NGX_Handle* handle, NVSDK_NGX_Parameter* parameters,
                            PFN_NVSDK_NGX_ProgressCallback callback)
{
    if (!original)
        return NVSDK_NGX_Result_FAIL_FeatureNotFound;

    // The host's own native-DLSS branch already runs the correction for this
    // evaluation; handling it twice would duplicate the history and the GPU work.
    if (InsideNativeFgHook())
        return original(command, handle, parameters, callback);

    NvngxDlssgHookCalls().fetch_add(1, std::memory_order_relaxed);
    NoteNgxFeature(static_cast<unsigned>(NVSDK_NGX_Feature_FrameGeneration),
                   handle != nullptr ? handle->Id : 0u, origin);
    try
    {
        return EvaluateNativeFG(command, handle, parameters, callback, reinterpret_cast<NativeEvaluate>(original));
    }
    catch (...)
    {
        return original(command, handle, parameters, callback);
    }
}

template <unsigned Index>
NVSDK_NGX_Result HookedEvaluateIndexed(ID3D12GraphicsCommandList* command, const NVSDK_NGX_Handle* handle,
                                       NVSDK_NGX_Parameter* parameters, PFN_NVSDK_NGX_ProgressCallback callback)
{
    const auto& slot = slots[Index];
    return HookedImpl(slot.original, slot.origin != nullptr ? slot.origin : "ngx-hook", command, handle, parameters,
                      callback);
}

constexpr EvaluateFeatureFn kHookEntries[kMaxHookSlots] {
    &HookedEvaluateIndexed<0>, &HookedEvaluateIndexed<1>, &HookedEvaluateIndexed<2>, &HookedEvaluateIndexed<3>,
    &HookedEvaluateIndexed<4>, &HookedEvaluateIndexed<5>, &HookedEvaluateIndexed<6>, &HookedEvaluateIndexed<7>,
};
} // namespace

bool InstallNgxEvaluateHook(void* module) noexcept
{
    if (!module)
        return false;
    wchar_t modulePath[MAX_PATH] {};
    GetModuleFileNameW(static_cast<HMODULE>(module), modulePath, MAX_PATH);
    try
    {
        HMODULE self = nullptr;
        if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                   GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                               reinterpret_cast<LPCWSTR>(&InstallNgxEvaluateHook), &self) &&
            self == static_cast<HMODULE>(module))
            return false; // Never wrap ourselves.
        for (const auto& slot : slots)
            if (slot.module == module)
                return true;

        auto* address = reinterpret_cast<EvaluateFeatureFn>(
            GetProcAddress(static_cast<HMODULE>(module), "NVSDK_NGX_D3D12_EvaluateFeature"));
        if (!address)
        {
            NoteHookStage(modulePath, 0);
            return false;
        }

        unsigned index = kMaxHookSlots;
        for (unsigned i = 0; i < kMaxHookSlots; ++i)
            if (slots[i].module == nullptr)
            {
                index = i;
                break;
            }
        if (index == kMaxHookSlots)
        {
            NoteHookStage(modulePath, 1);
            return false;
        }

        slots[index].original = address;
        slots[index].origin = "ngx-evaluate-hook";
        if (DetourTransactionBegin() != NO_ERROR)
        {
            NoteHookStage(modulePath, 2);
            slots[index] = {};
            return false;
        }
        DetourUpdateThread(GetCurrentThread());
        if (DetourAttach(&(PVOID&) slots[index].original, kHookEntries[index]) != NO_ERROR ||
            DetourTransactionCommit() != NO_ERROR)
        {
            NoteHookStage(modulePath, 3);
            DetourTransactionAbort();
            slots[index] = {};
            return false;
        }
        slots[index].module = module;
        NvngxDlssgHookFlag().store(true, std::memory_order_release);
        return true;
    }
    catch (...)
    {
        return false;
    }
}

void InstallLoadedNgxHooks() noexcept
{
    // The provider is loaded lazily and can be either the game folder copy or
    // the driver core, depending on which build Streamline ends up using, so the
    // health thread retries for a bounded while instead of latching once.
    static std::atomic<unsigned> attempts { 0 };
    if (attempts.fetch_add(1, std::memory_order_relaxed) >= 1200)
        return;
    for (const wchar_t* name : { L"nvngx_dlssg.dll", L"_nvngx.dll" })
        if (auto* module = GetModuleHandleW(name))
        {
            const bool installed = InstallNgxEvaluateHook(module);
            // Bounded record: shows whether the provider was found and wrapped at
            // all, which otherwise cannot be told from "the call never happens".
            NoteNvngxLoad(name, installed);
        }
}
} // namespace GlassFg
