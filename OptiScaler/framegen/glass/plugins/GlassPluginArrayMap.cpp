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
#include <cstring>
#include <iterator>
#include <atomic>

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

const GlassPluginApi* host = nullptr;
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
    if (host && host->trace)
        host->trace(text);
}

void hookedAppend(void* container, std::uintptr_t sourceMatrix)
{
    if (current.valid && sourceMatrix >= current.arrayBase &&
        sourceMatrix < current.arrayBase + std::uint64_t(current.count) * 0x30)
    {
        const auto index = std::uint32_t((sourceMatrix - current.arrayBase) / 0x30);
        if (firstCount < std::size(firstIndices))
            firstIndices[firstCount++] = index;
        appends.fetch_add(1, std::memory_order_relaxed);
    }
    if (originalAppend)
        originalAppend(container, sourceMatrix);
}

void hookedGrouped(std::uintptr_t proxy, std::uintptr_t* context)
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
    if (originalGrouped)
        originalGrouped(proxy, context);
    if (current.valid && firstCount && host && host->publishArrayMapping)
    {
        GlassArrayMappingEntry entry;
        entry.proxy = current.proxy;
        entry.outputStart = current.outputStart;
        entry.count = firstCount;
        entry.frame = ++publishedFrames;
        for (unsigned i = 0; i < firstCount && i < 64; ++i)
            entry.indices[i] = firstIndices[i];
        host->publishArrayMapping(&entry);
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
            trace(text);
        }
    }
    current.valid = false;
}
} // namespace

extern "C" __declspec(dllexport) bool GlassPluginAttach(const GlassPluginApi* api)
{
    if (!api || api->version != 1 || !api->trace)
        return false;
    host = api;
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
    GlassFg::DetourThreads threads;
    if (!threads.gather() || DetourTransactionBegin() != NO_ERROR)
    {
        trace("ARRAY_MAP rejected=1 reason=transaction_begin");
        return false;
    }
    const bool detached = DetourAttach(reinterpret_cast<PVOID*>(&originalGrouped),
                                       reinterpret_cast<PVOID>(&hookedGrouped)) == NO_ERROR &&
                          DetourAttach(reinterpret_cast<PVOID*>(&originalAppend),
                                       reinterpret_cast<PVOID>(&hookedAppend)) == NO_ERROR;
    if (!detached || !threads.enlist() || DetourTransactionCommit() != NO_ERROR)
    {
        DetourTransactionAbort();
        originalGrouped = nullptr;
        originalAppend = nullptr;
        trace("ARRAY_MAP rejected=1 reason=transaction_commit");
        return false;
    }
    trace("ARRAY_MAP attached=1");
    return true;
}

extern "C" __declspec(dllexport) void GlassPluginDetach()
{
    if (originalGrouped || originalAppend)
    {
        GlassFg::DetourThreads threads;
        threads.gather();
        DetourTransactionBegin();
        if (originalGrouped)
            DetourDetach(reinterpret_cast<PVOID*>(&originalGrouped), reinterpret_cast<PVOID>(&hookedGrouped));
        if (originalAppend)
            DetourDetach(reinterpret_cast<PVOID*>(&originalAppend), reinterpret_cast<PVOID>(&hookedAppend));
        threads.enlist();
        DetourTransactionCommit();
    }
    originalGrouped = nullptr;
    originalAppend = nullptr;
    trace("ARRAY_MAP detached=1");
    host = nullptr;
}

BOOL WINAPI DllMain(HINSTANCE, DWORD, LPVOID)
{
    return TRUE;
}
