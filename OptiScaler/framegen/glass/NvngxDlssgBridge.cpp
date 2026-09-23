#include "pch.h"
#include "NvngxDlssgBridge.h"
#include "NativeHost.h"
#include <detours/detours.h>
#include <intrin.h>
#include <cwctype>
#include <string>

namespace GlassFg
{
namespace
{
// Matches the real export: the NGX API hands the parameter block over as a
// mutable pointer.
using EvaluateFeatureFn = NVSDK_NGX_Result (*)(ID3D12GraphicsCommandList*, const NVSDK_NGX_Handle*,
                                               NVSDK_NGX_Parameter*, PFN_NVSDK_NGX_ProgressCallback);
using CreateFeatureFn = NVSDK_NGX_Result (*)(ID3D12GraphicsCommandList*, NVSDK_NGX_Feature, NVSDK_NGX_Parameter*,
                                              NVSDK_NGX_Handle**);
using ReleaseFeatureFn = NVSDK_NGX_Result (*)(const NVSDK_NGX_Handle*);

EvaluateFeatureFn originalEvaluate = nullptr;

// Handles the provider created for NVSDK_NGX_Feature_FrameGeneration. The
// driver-level DLSS-G evaluation names only MotionVectors/Depth, so the
// parameter table cannot prove which feature is being evaluated; the creation
// call can. Registration is bounded and lock free because creation happens on
// the render thread during initialisation while the evaluate hook reads it on
// every frame.
constexpr unsigned kMaxFrameGenerationHandles = 8;
std::atomic<const void*> frameGenerationHandles[kMaxFrameGenerationHandles] {};
std::atomic<unsigned> frameGenerationHandleIds[kMaxFrameGenerationHandles] {};
std::atomic<unsigned> frameGenerationHandleCursor { 0 };

void RegisterFrameGenerationHandle(const void* handle, unsigned id) noexcept
{
    if (handle == nullptr)
        return;
    for (unsigned i = 0; i < kMaxFrameGenerationHandles; ++i)
        if (frameGenerationHandles[i].load(std::memory_order_acquire) == handle &&
            frameGenerationHandleIds[i].load(std::memory_order_relaxed) == id)
            return;
    const unsigned index =
        frameGenerationHandleCursor.fetch_add(1, std::memory_order_relaxed) % kMaxFrameGenerationHandles;
    frameGenerationHandleIds[index].store(id, std::memory_order_relaxed);
    frameGenerationHandles[index].store(handle, std::memory_order_release);
}

void ForgetFrameGenerationHandle(const void* handle, unsigned id) noexcept
{
    if (handle == nullptr)
        return;
    for (unsigned i = 0; i < kMaxFrameGenerationHandles; ++i)
    {
        const void* expected = frameGenerationHandles[i].load(std::memory_order_acquire);
        // The provider reuses handle ids, so a release only clears the entry it
        // actually created: both the pointer and the id have to match.
        if (expected != handle || frameGenerationHandleIds[i].load(std::memory_order_relaxed) != id)
            continue;
        frameGenerationHandles[i].compare_exchange_strong(expected, nullptr, std::memory_order_acq_rel);
    }
}

// The NGX provider is loaded and unloaded per initialisation and can come from
// the game folder (nvngx_dlssg.dll) or the driver (_nvngx.dll), each reload
// landing on a different base address. Detours needs one trampoline per target,
// so the slots are pooled instead of latching a single address.
constexpr unsigned kMaxHookSlots = 8;
struct HookSlot
{
    void* module = nullptr;
    EvaluateFeatureFn original = nullptr;
    CreateFeatureFn createOriginal = nullptr;
    ReleaseFeatureFn releaseOriginal = nullptr;
    const char* origin = nullptr;
    // nvngx_dlssg.dll serves frame generation only, so every evaluation on it
    // is the generator even before a creation call has been observed.
    bool dedicatedProvider = false;
};
HookSlot slots[kMaxHookSlots] {};

bool IsDedicatedDlssgProvider(const wchar_t* path) noexcept
{
    if (path == nullptr)
        return false;
    constexpr wchar_t suffix[] = L"nvngx_dlssg.dll";
    constexpr size_t suffixLength = (sizeof(suffix) / sizeof(suffix[0])) - 1;
    const size_t length = wcslen(path);
    return length >= suffixLength && _wcsicmp(path + length - suffixLength, suffix) == 0;
}

// Which module issued the evaluation is the one identity the driver-level table
// cannot carry: its parameter names are shared with the upscaler and Ray
// Reconstruction, and its handle can be older than the create hook that would
// register it. Streamline's frame generation plugin evaluates nothing else, so
// a call from that module is the generator. The verdict is cached per
// allocation base because every evaluation of a session arrives from the same
// one or two modules, and the over-the-air plugin is mapped as an image without
// a loader entry, so its name has to come from the mapping rather than from
// GetModuleHandle.
constexpr unsigned kMaxCallerSlots = 8;
struct CallerSlot
{
    std::atomic<const void*> base { nullptr };
    std::atomic<int> verdict { 0 };
};
CallerSlot callerSlots[kMaxCallerSlots];

// 0 = off, 1 = only the frame generation plugin, 2 = every caller (diagnostic
// only, selected from the control channel and never persisted).
std::atomic<unsigned> callerIdentityMode { 1 };

using GetMappedFileNameWfn = DWORD(WINAPI*)(HANDLE, LPVOID, LPWSTR, DWORD);

bool PathIsFrameGenerationPlugin(const wchar_t* path) noexcept
{
    if (path == nullptr || path[0] == L'\0')
        return false;
    std::wstring lower(path);
    for (auto& character : lower)
        character = static_cast<wchar_t>(std::towlower(character));
    return lower.find(L"sl.dlss_g") != std::wstring::npos || lower.find(L"sl_dlss_g") != std::wstring::npos ||
           lower.find(L"nvngx_dlssg") != std::wstring::npos;
}

bool FrameGenerationCaller(const void* address) noexcept
{
    const unsigned mode = callerIdentityMode.load(std::memory_order_relaxed);
    if (mode == 0 || address == nullptr)
        return false;
    if (mode >= 2)
        return true;
    MEMORY_BASIC_INFORMATION info {};
    if (VirtualQuery(address, &info, sizeof(info)) != sizeof(info) || info.AllocationBase == nullptr)
        return false;
    const void* base = info.AllocationBase;
    for (const auto& slot : callerSlots)
        if (slot.base.load(std::memory_order_acquire) == base)
            return slot.verdict.load(std::memory_order_relaxed) > 0;
    wchar_t path[MAX_PATH] {};
    HMODULE module = nullptr;
    if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           static_cast<LPCWSTR>(address), &module) &&
        module != nullptr)
        GetModuleFileNameW(module, path, MAX_PATH);
    if (path[0] == L'\0')
    {
        static const auto getMappedFileName = []() -> GetMappedFileNameWfn {
            const auto kernel = GetModuleHandleW(L"kernel32.dll");
            return kernel != nullptr ? reinterpret_cast<GetMappedFileNameWfn>(
                                           GetProcAddress(kernel, "GetMappedFileNameW"))
                                     : nullptr;
        }();
        if (getMappedFileName != nullptr)
            getMappedFileName(GetCurrentProcess(), const_cast<void*>(base), path, MAX_PATH);
    }
    const bool verdict = PathIsFrameGenerationPlugin(path);
    static std::atomic<unsigned> cursor { 0 };
    const unsigned index = cursor.fetch_add(1, std::memory_order_relaxed) % kMaxCallerSlots;
    // The verdict is written before the base, so a reader that observes the
    // base also observes a decided slot.
    callerSlots[index].base.store(nullptr, std::memory_order_relaxed);
    callerSlots[index].verdict.store(verdict ? 1 : -1, std::memory_order_relaxed);
    callerSlots[index].base.store(base, std::memory_order_release);
    return verdict;
}

NVSDK_NGX_Result HookedImpl(EvaluateFeatureFn original, const char* origin, bool dedicatedProvider,
                            ID3D12GraphicsCommandList* command, const NVSDK_NGX_Handle* handle,
                            NVSDK_NGX_Parameter* parameters, PFN_NVSDK_NGX_ProgressCallback callback,
                            const void* caller)
{
    if (!original)
        return NVSDK_NGX_Result_FAIL_FeatureNotFound;

    // The host's own native-DLSS branch already runs the correction for this
    // evaluation; handling it twice would duplicate the history and the GPU work.
    if (InsideNativeFgHook())
        return original(command, handle, parameters, callback);

    NvngxDlssgHookCalls().fetch_add(1, std::memory_order_relaxed);
    const bool providerFrameGeneration =
        ProviderConfirmsFrameGeneration(static_cast<const void*>(handle), dedicatedProvider);
    const bool frameGenerationCaller = FrameGenerationCaller(caller);
    NoteFrameGenerationHandle(false, static_cast<const void*>(handle), handle != nullptr ? handle->Id : 0u,
                              providerFrameGeneration);
    NoteFrameGenerationCaller(caller, static_cast<const void*>(handle), handle != nullptr ? handle->Id : 0u,
                              frameGenerationCaller);
    NoteNgxFeature(static_cast<unsigned>(NVSDK_NGX_Feature_FrameGeneration),
                   handle != nullptr ? handle->Id : 0u, origin);
    try
    {
        return EvaluateNativeFG(command, handle, parameters, callback, reinterpret_cast<NativeEvaluate>(original),
                                providerFrameGeneration, true, frameGenerationCaller);
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
    // The return address has to be taken here: this template is the function
    // Detours calls, so its caller is the module that evaluated the feature.
    const void* const caller = _ReturnAddress();
    return HookedImpl(slot.original, slot.origin != nullptr ? slot.origin : "ngx-hook", slot.dedicatedProvider, command,
                      handle, parameters, callback, caller);
}

template <unsigned Index>
NVSDK_NGX_Result HookedCreateIndexed(ID3D12GraphicsCommandList* command, NVSDK_NGX_Feature feature,
                                     NVSDK_NGX_Parameter* parameters, NVSDK_NGX_Handle** outHandle)
{
    const auto& slot = slots[Index];
    if (!slot.createOriginal)
        return NVSDK_NGX_Result_FAIL_FeatureNotFound;
    const auto result = slot.createOriginal(command, feature, parameters, outHandle);
    // Bounded record of every creation this provider hook sees. Without it the
    // difference between "the hook never installed" and "the feature was
    // created before the hook" cannot be told from the log.
    static std::atomic<unsigned> createLogged { 0 };
    if (createLogged.fetch_add(1, std::memory_order_relaxed) < 8)
        NoteNgxCreate(static_cast<unsigned>(feature),
                      outHandle != nullptr && *outHandle != nullptr ? (*outHandle)->Id : 0u,
                      slot.dedicatedProvider ? "provider-dlssg" : "provider-ngx");
    if (result == NVSDK_NGX_Result_Success && feature == NVSDK_NGX_Feature_FrameGeneration)
    {
        const void* created = outHandle != nullptr ? static_cast<const void*>(*outHandle) : nullptr;
        RegisterFrameGenerationHandle(created, outHandle != nullptr && *outHandle != nullptr ? (*outHandle)->Id : 0u);
        NoteFrameGenerationHandle(true, created,
                                  outHandle != nullptr && *outHandle != nullptr ? (*outHandle)->Id : 0u, true);
        NoteNgxFeature(static_cast<unsigned>(feature),
                       outHandle != nullptr && *outHandle != nullptr ? (*outHandle)->Id : 0u,
                       slot.origin != nullptr ? slot.origin : "ngx-create-hook");
    }
    return result;
}

template <unsigned Index> NVSDK_NGX_Result HookedReleaseIndexed(const NVSDK_NGX_Handle* handle)
{
    const auto& slot = slots[Index];
    if (!slot.releaseOriginal)
        return NVSDK_NGX_Result_FAIL_FeatureNotFound;
    ForgetFrameGenerationHandle(static_cast<const void*>(handle), handle != nullptr ? handle->Id : 0u);
    return slot.releaseOriginal(handle);
}

constexpr EvaluateFeatureFn kHookEntries[kMaxHookSlots] {
    &HookedEvaluateIndexed<0>, &HookedEvaluateIndexed<1>, &HookedEvaluateIndexed<2>, &HookedEvaluateIndexed<3>,
    &HookedEvaluateIndexed<4>, &HookedEvaluateIndexed<5>, &HookedEvaluateIndexed<6>, &HookedEvaluateIndexed<7>,
};
constexpr CreateFeatureFn kCreateHookEntries[kMaxHookSlots] {
    &HookedCreateIndexed<0>, &HookedCreateIndexed<1>, &HookedCreateIndexed<2>, &HookedCreateIndexed<3>,
    &HookedCreateIndexed<4>, &HookedCreateIndexed<5>, &HookedCreateIndexed<6>, &HookedCreateIndexed<7>,
};
constexpr ReleaseFeatureFn kReleaseHookEntries[kMaxHookSlots] {
    &HookedReleaseIndexed<0>, &HookedReleaseIndexed<1>, &HookedReleaseIndexed<2>, &HookedReleaseIndexed<3>,
    &HookedReleaseIndexed<4>, &HookedReleaseIndexed<5>, &HookedReleaseIndexed<6>, &HookedReleaseIndexed<7>,
};
} // namespace

bool ProviderConfirmsFrameGeneration(const void* handle, bool dedicatedProvider) noexcept
{
    if (handle == nullptr)
        return false;
    if (dedicatedProvider)
        return true;
    for (const auto& slot : frameGenerationHandles)
        if (slot.load(std::memory_order_acquire) == handle)
            return true;
    return false;
}

void SetFrameGenerationCallerMode(unsigned mode) noexcept
{
    callerIdentityMode.store(mode > 2u ? 2u : mode, std::memory_order_relaxed);
}

unsigned FrameGenerationCallerMode() noexcept
{
    return callerIdentityMode.load(std::memory_order_relaxed);
}

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
        wchar_t originPath[MAX_PATH] {};
        GetModuleFileNameW(static_cast<HMODULE>(module), originPath, MAX_PATH);
        slots[index].dedicatedProvider = IsDedicatedDlssgProvider(originPath);
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
        // The creation hook is what proves the feature identity of a handle the
        // driver-level evaluation only names MotionVectors/Depth for, so it is
        // installed with the evaluate hook. A provider that does not export the
        // create/release pair keeps the dedicated-module fallback instead of
        // losing the evaluate hook.
        auto* createAddress = reinterpret_cast<CreateFeatureFn>(
            GetProcAddress(static_cast<HMODULE>(module), "NVSDK_NGX_D3D12_CreateFeature"));
        auto* releaseAddress = reinterpret_cast<ReleaseFeatureFn>(
            GetProcAddress(static_cast<HMODULE>(module), "NVSDK_NGX_D3D12_ReleaseFeature"));
        if (createAddress != nullptr || releaseAddress != nullptr)
        {
            if (DetourTransactionBegin() == NO_ERROR)
            {
                DetourUpdateThread(GetCurrentThread());
                bool attached = true;
                if (createAddress != nullptr)
                {
                    slots[index].createOriginal = createAddress;
                    attached = DetourAttach(&(PVOID&) slots[index].createOriginal, kCreateHookEntries[index]) == NO_ERROR;
                }
                if (attached && releaseAddress != nullptr)
                {
                    slots[index].releaseOriginal = releaseAddress;
                    attached =
                        DetourAttach(&(PVOID&) slots[index].releaseOriginal, kReleaseHookEntries[index]) == NO_ERROR;
                }
                if (attached && DetourTransactionCommit() == NO_ERROR)
                    NvngxDlssgCreateHookFlag().store(true, std::memory_order_release);
                else
                {
                    DetourTransactionAbort();
                    slots[index].createOriginal = nullptr;
                    slots[index].releaseOriginal = nullptr;
                }
            }
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
