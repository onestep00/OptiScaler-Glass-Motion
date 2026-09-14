// Live array element mapping hook.
//
// Cyberpunk's grouped-array path (proxy +0xEA flag 0x2000) rebuilds the draw's
// instance order from the group's source-index list:
//   0x1e8778(proxy, context) walks the selected group records and, for every
//   16-bit source index, appends proxy+0x108[index*0x30] through 0x9c19e8 into
//   a container. The packet receives those entries in append order with the
//   output start stored at proxy+0x114.
//
// This plugin attaches both functions at runtime (no process restart) and logs
// the resulting element -> source-index mapping so the resident module can use
// engine data instead of rejecting grouped arrays.
#include <windows.h>
#include <detours.h>
#include "../DetourThreads.h"
#include <cstdio>
#include <cstdint>
#include <cstdarg>
#include <cstring>
#include <iterator>
#include <atomic>
#include <string>

namespace
{
struct GlassArrayMappingEntry
{
    std::uintptr_t proxy = 0;
    std::uint32_t outputStart = 0, count = 0;
    std::uint64_t frame = 0;
    std::uint32_t indices[64] {};
};

struct GlassPluginApi
{
    unsigned version = 1;
    FILE* log = nullptr;
    const wchar_t* moduleDirectory = nullptr;
    const void* engineUpdate = nullptr;
    void (*publishArrayMapping)(const GlassArrayMappingEntry*) noexcept = nullptr;
    void (*trace)(const char* text) noexcept = nullptr;
};

// The host pointer must never outlive the caller's stack frame: the resident
// module builds the API struct on its poll thread and returns immediately after
// Attach. Storing that pointer crashed the game on 2026-09-14 17:37 when the
// first grouped-array publish call jumped through reused stack memory. Keep a
// private copy and publish it with release/acquire ordering.
GlassPluginApi hostStorage {};
std::atomic<const GlassPluginApi*> host { nullptr };
std::atomic<unsigned> groupedCalls { 0 }, appendCalls { 0 };
std::atomic<bool> draining { false };
std::atomic<int> inFlight { 0 };
std::atomic<unsigned> hitsBase { 0 }, hitsFlag { 0 };
HMODULE selfModule = nullptr;
std::wstring ownLogPath;
using GroupedPathFn = void (*)(std::uintptr_t proxy, std::uintptr_t* context);
using AppendFn = void (*)(void* container, std::uintptr_t sourceMatrix);
GroupedPathFn originalGrouped = nullptr;
AppendFn originalAppend = nullptr;

struct Group
{
    std::uintptr_t proxy = 0, arrayBase = 0;
    std::uint32_t count = 0, outputStart = 0;
    std::uint16_t flags = 0;
    bool valid = false;
};
Group current {};
std::atomic<unsigned> appends { 0 };
std::uint32_t firstIndices[64] {};
unsigned firstCount = 0;
std::uint64_t publishedFrames = 0;

void trace(const char* text)
{
    const auto* api = host.load(std::memory_order_acquire);
    if (api && api->trace)
        api->trace(text);
}

// Independent of the resident module so hook-entry and fault evidence survives
// even when the module's own trace path is the suspect.
void directLog(const char* format, ...) noexcept
{
    if (ownLogPath.empty())
        return;
    char line[480] {};
    std::va_list args;
    va_start(args, format);
    std::vsnprintf(line, sizeof(line), format, args);
    va_end(args);
    if (FILE* file = _wfopen(ownLogPath.c_str(), L"a"))
    {
        SYSTEMTIME now {};
        GetLocalTime(&now);
        std::fprintf(file, "%02u:%02u:%02u.%03u %s\n", now.wHour, now.wMinute, now.wSecond, now.wMilliseconds,
                     line);
        std::fclose(file);
    }
}

LONG CALLBACK faultHandler(EXCEPTION_POINTERS* info) noexcept
{
    const auto code = info && info->ExceptionRecord ? info->ExceptionRecord->ExceptionCode : 0ul;
    if (code == 0xC0000005ul || code == 0xC000001Dul || code == 0xC0000094ul || code == 0xC0000096ul ||
        code == 0xC0000409ul)
    {
        void* address = info->ExceptionRecord->ExceptionAddress;
        char module[MAX_PATH] {};
        HMODULE owner = nullptr;
        if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                   GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                               reinterpret_cast<LPCSTR>(address), &owner) &&
            owner)
            GetModuleFileNameA(owner, module, sizeof(module));
        directLog("fault code=%08lx addr=%p module=%s", code, address, module);
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

void probeFields(const char* tag, unsigned call, std::uintptr_t pointer) noexcept;

void hookedAppend(void* container, std::uintptr_t sourceMatrix)
{
    (void)container;
    if (draining.load(std::memory_order_relaxed))
        return;
    ++inFlight;
    const auto call = appendCalls.fetch_add(1, std::memory_order_relaxed) + 1;
    if (call <= 4)
        directLog("append_enter=%u source=%llx valid=%u", call,
                  static_cast<unsigned long long>(sourceMatrix), current.valid ? 1u : 0u);
    if (current.valid && sourceMatrix >= current.arrayBase &&
        sourceMatrix < current.arrayBase + std::uint64_t(current.count) * 0x30)
    {
        const auto index = std::uint32_t((sourceMatrix - current.arrayBase) / 0x30);
        if (firstCount < std::size(firstIndices))
            firstIndices[firstCount++] = index;
        appends.fetch_add(1, std::memory_order_relaxed);
    }
    --inFlight;
}

void hookedGrouped(std::uintptr_t proxy, std::uintptr_t* context)
{
    // During detach the hook must still forward the original call, but it must
    // stop touching plugin state so the DLL can be unmapped safely.
    if (draining.load(std::memory_order_relaxed))
    {
        if (originalGrouped)
            originalGrouped(proxy, context);
        return;
    }
    ++inFlight;
    const auto call = groupedCalls.fetch_add(1, std::memory_order_relaxed) + 1;
    if (call <= 4)
        directLog("grouped_enter=%u proxy=%llx context=%llx", call,
                  static_cast<unsigned long long>(proxy),
                  static_cast<unsigned long long>(reinterpret_cast<std::uintptr_t>(context)));
    // Unreadable proxy fields must never crash the game: this hook runs on the
    // engine's render path.
    __try
    {
        if (proxy)
        {
            current.proxy = proxy;
            current.arrayBase = *reinterpret_cast<std::uintptr_t*>(proxy + 0x108);
            current.count = *reinterpret_cast<std::uint32_t*>(proxy + 0x110);
            current.outputStart = *reinterpret_cast<std::uint32_t*>(proxy + 0x114);
            current.flags = *reinterpret_cast<std::uint16_t*>(proxy + 0xea);
            current.valid = current.arrayBase != 0 && current.count != 0;
            appends.store(0, std::memory_order_relaxed);
            for (auto& seen : firstIndices)
                seen = 0;
            firstCount = 0;
        }
    }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            current.valid = false;
            directLog("read_fault=1");
        }
    // Raw field probe so a wrong base object or a wrong layout shows up as zeros
    // instead of looking like "the path never has elements".
    if (call <= 4 || (call % 256) == 0)
    {
        probeFields("grouped_proxy", call, proxy);
        if (context)
            probeFields("grouped_context", call, reinterpret_cast<std::uintptr_t>(context));
    }
    if (originalGrouped)
        originalGrouped(proxy, context);
    const auto* api = host.load(std::memory_order_acquire);
    if (current.valid && firstCount && api && api->publishArrayMapping)
    {
        __try
        {
            GlassArrayMappingEntry entry;
            entry.proxy = current.proxy;
            entry.outputStart = current.outputStart;
            entry.count = firstCount;
            entry.frame = ++publishedFrames;
            for (unsigned i = 0; i < firstCount && i < 64; ++i)
                entry.indices[i] = firstIndices[i];
            api->publishArrayMapping(&entry);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            directLog("publish_fault=1");
        }
    }
    if (current.valid)
    {
        static unsigned reported = 0;
        if (++reported % 60 == 0)
        {
            char text[256] {};
            std::snprintf(text, sizeof(text),
                          "ARRAY_MAP proxy=%llx count=%u output_start=%u flags=%04x appends=%u indices=%u,%u,%u,%u,%u,%u",
                          static_cast<unsigned long long>(current.proxy), current.count, current.outputStart,
                          current.flags, appends.load(std::memory_order_relaxed), firstIndices[0], firstIndices[1],
                          firstIndices[2], firstIndices[3], firstIndices[4], firstIndices[5]);
            directLog("%s", text);
        }
    }
    current.valid = false;
    --inFlight;
}

bool markerPresent(const wchar_t* name) noexcept
{
    if (ownLogPath.empty())
        return false;
    auto directory = ownLogPath;
    const auto cut = directory.find_last_of(L"\\/");
    if (cut == std::wstring::npos)
        return false;
    directory.resize(cut);
    const auto candidate = directory + L"\\" + name;
    return GetFileAttributesW(candidate.c_str()) != INVALID_FILE_ATTRIBUTES;
}

// Layout probe: reads the four fields the decompiled grouped path uses from an
// arbitrary candidate object, so a wrong base object or wrong offsets show up
// as zeros instead of silently producing no elements.
void probeFields(const char* tag, unsigned call, std::uintptr_t pointer) noexcept
{
    std::uintptr_t base = 0;
    std::uint32_t count = 0, start = 0;
    std::uint16_t flags = 0;
    bool read = false;
    __try
    {
        base = *reinterpret_cast<std::uintptr_t*>(pointer + 0x108);
        count = *reinterpret_cast<std::uint32_t*>(pointer + 0x110);
        start = *reinterpret_cast<std::uint32_t*>(pointer + 0x114);
        flags = *reinterpret_cast<std::uint16_t*>(pointer + 0xea);
        read = true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        read = false;
    }
    if (base)
        hitsBase.fetch_add(1, std::memory_order_relaxed);
    if (flags & 0x2000)
        hitsFlag.fetch_add(1, std::memory_order_relaxed);
    directLog("%s=%u ptr=%llx base=%llx count=%u start=%u flags=%04x read=%u", tag, call,
              static_cast<unsigned long long>(pointer), static_cast<unsigned long long>(base), count, start, flags,
              read ? 1u : 0u);
}
} // namespace

// Fresh-hook entry points used by GlassPluginAppendHook.asm. The trampoline
// saves every volatile argument register around this call, so a function with
// more parameters than the decompiler shows still receives its own values.
extern "C" void glassArrayMapAppendHook(void* container, std::uintptr_t sourceMatrix) noexcept
{
    hookedAppend(container, sourceMatrix);
}

extern "C" void glassArrayMapAppendTrampoline();
extern "C" void* glassArrayMapAppendTarget = nullptr;
extern "C" void glassArrayMapAppendPassTrampoline();
extern "C" void glassArrayMapGroupedPassTrampoline();
extern "C" void* glassArrayMapGroupedTarget = nullptr;

// Heartbeat from plugin-owned thread: proves whether the process survived the
// patch and whether the hooks ever ran before a crash.
std::atomic<bool> monitorStop { false };
HANDLE monitorThread = nullptr;

DWORD WINAPI monitorLoop(LPVOID) noexcept
{
    unsigned beat = 0;
    while (!monitorStop.load(std::memory_order_relaxed))
    {
        Sleep(250);
        if (monitorStop.load(std::memory_order_relaxed))
            break;
        directLog("beat=%u grouped=%u append=%u basehits=%u flaghits=%u", ++beat,
                  groupedCalls.load(std::memory_order_relaxed), appendCalls.load(std::memory_order_relaxed),
                  hitsBase.load(std::memory_order_relaxed), hitsFlag.load(std::memory_order_relaxed));
    }
    return 0;
}

extern "C" __declspec(dllexport) bool GlassPluginAttach(const GlassPluginApi* api)
{
    if (!api || api->version != 1 || !api->trace)
        return false;
    {
        wchar_t path[MAX_PATH] {};
        const auto length = GetModuleFileNameW(selfModule, path, MAX_PATH);
        std::wstring text(path, length);
        const auto cut = text.find_last_of(L"\\/");
        if (cut != std::wstring::npos)
            text.resize(cut);
        ownLogPath = text + L"\\glass-plugin.log";
    }
    directLog("attach_begin exe=%p grouped=%p append=%p", GetModuleHandleW(nullptr),
              reinterpret_cast<void*>(reinterpret_cast<std::byte*>(GetModuleHandleW(nullptr)) + 0x1e8778),
              reinterpret_cast<void*>(reinterpret_cast<std::byte*>(GetModuleHandleW(nullptr)) + 0x9c19e8));
    if (!AddVectoredExceptionHandler(1, &faultHandler))
        directLog("veh_failed=1");
    hostStorage = *api;
    host.store(&hostStorage, std::memory_order_release);
    auto* base = reinterpret_cast<std::byte*>(GetModuleHandleW(nullptr));
    if (!base)
        return false;
    // Sanity: the grouped path tests the 16-bit flag at +0xEA.
    auto* grouped = reinterpret_cast<std::byte*>(base + 0x1e8778);
    bool flagTest = false;
    for (unsigned i = 0; i < 64; ++i)
        if (std::memcmp(grouped + i, "\xea\x00\x00\x00", 4) == 0)
            flagTest = true;
    if (!flagTest)
    {
        trace("ARRAY_MAP rejected=1 reason=signature");
        return false;
    }
    auto* append = reinterpret_cast<std::byte*>(base + 0x9c19e8);
    originalGrouped = reinterpret_cast<GroupedPathFn>(const_cast<void*>(static_cast<const void*>(grouped)));
    originalAppend = reinterpret_cast<AppendFn>(const_cast<void*>(static_cast<const void*>(append)));
    // Detours requires a transaction and the enlistment of every thread that can
    // execute the patched code. Attaching outside a transaction crashed the game
    // on 2026-09-14 16:59.
    const bool skipGrouped = markerPresent(L"plugin-grouped.off");
    const bool skipAppend = markerPresent(L"plugin-append.off");
    const bool passThrough = markerPresent(L"plugin-passthrough.on");
    directLog("attach switches grouped=%u append=%u passthrough=%u", skipGrouped ? 0u : 1u,
              skipAppend ? 0u : 1u, passThrough ? 1u : 0u);
    GlassFg::DetourThreads threads;
    if (!threads.gather() || DetourTransactionBegin() != NO_ERROR)
    {
        trace("ARRAY_MAP rejected=1 reason=transaction_begin");
        return false;
    }
    bool attached = true;
    if (!skipGrouped)
        attached = attached &&
                   DetourAttach(reinterpret_cast<PVOID*>(&originalGrouped),
                                passThrough ? reinterpret_cast<PVOID>(&glassArrayMapGroupedPassTrampoline)
                                            : reinterpret_cast<PVOID>(&hookedGrouped)) == NO_ERROR;
    if (!skipAppend)
        attached = attached &&
                   DetourAttach(reinterpret_cast<PVOID*>(&originalAppend),
                                passThrough ? reinterpret_cast<PVOID>(&glassArrayMapAppendPassTrampoline)
                                            : reinterpret_cast<PVOID>(&glassArrayMapAppendTrampoline)) == NO_ERROR;
    const bool detached = attached;
    if (!detached || !threads.enlist() || DetourTransactionCommit() != NO_ERROR)
    {
        DetourTransactionAbort();
        originalGrouped = nullptr;
        originalAppend = nullptr;
        directLog("attach failed");
        trace("ARRAY_MAP rejected=1 reason=transaction_commit");
        return false;
    }
    // The trampoline tail-jumps here, so it must hold the real body.
    glassArrayMapAppendTarget = reinterpret_cast<void*>(originalAppend);
    glassArrayMapGroupedTarget = reinterpret_cast<void*>(originalGrouped);
    directLog("attach_committed grouped=%u append=%u", originalGrouped ? 1u : 0u, originalAppend ? 1u : 0u);
    monitorStop.store(false, std::memory_order_relaxed);
    monitorThread = CreateThread(nullptr, 0, &monitorLoop, nullptr, 0, nullptr);
    if (!monitorThread)
        directLog("monitor_thread_failed=1");
    // The build tag makes the module log identify which DLL actually loaded.
    trace("ARRAY_MAP attached=1 build=4-lifetime-veh");
    return true;
}

extern "C" __declspec(dllexport) void GlassPluginDetach()
{
    // A hook body can still be running on a render thread when the module calls
    // FreeLibrary. Unloading straight away crashed the game on 2026-09-14
    // 17:48:55. Drain the hook bodies first and give the engine one more frame
    // after the detour transaction before the DLL disappears.
    draining.store(true, std::memory_order_relaxed);
    for (unsigned i = 0; i < 200 && inFlight.load(std::memory_order_relaxed) != 0; ++i)
        Sleep(5);
    Sleep(250);
    monitorStop.store(true, std::memory_order_relaxed);
    if (monitorThread)
    {
        WaitForSingleObject(monitorThread, 1000);
        CloseHandle(monitorThread);
        monitorThread = nullptr;
    }
    if (originalGrouped || originalAppend)
    {
        GlassFg::DetourThreads threads;
        threads.gather();
        DetourTransactionBegin();
        if (originalGrouped)
            DetourDetach(reinterpret_cast<PVOID*>(&originalGrouped), reinterpret_cast<PVOID>(&hookedGrouped));
        if (originalAppend)
            DetourDetach(reinterpret_cast<PVOID*>(&originalAppend),
                         reinterpret_cast<PVOID>(&glassArrayMapAppendTrampoline));
        threads.enlist();
        DetourTransactionCommit();
    }
    Sleep(250);
    originalGrouped = nullptr;
    originalAppend = nullptr;
    glassArrayMapAppendTarget = nullptr;
    glassArrayMapGroupedTarget = nullptr;
    directLog("detached");
    trace("ARRAY_MAP detached=1");
    host.store(nullptr, std::memory_order_release);
    draining.store(false, std::memory_order_relaxed);
}

BOOL WINAPI DllMain(HINSTANCE instance, DWORD, LPVOID)
{
    selfModule = instance;
    return TRUE;
}
