#include "pch.h"
#include "CyberpunkGroups.h"
#include "CyberpunkLayout.h"
#include "DetourThreads.h"
#include "GlassArrayMapping.h"
#include "GlassHookProbe.h"
#include <atomic>
#include <cstring>
#include <mutex>

namespace GlassFg
{
namespace
{
using GroupedFn = void (*)(void*, void*);
using AppendFn = std::uintptr_t (*)(void*, std::uintptr_t);
GroupedFn originalGrouped = nullptr;
AppendFn originalAppend = nullptr;

// The published table holds 64 elements per array (GlassArrayMappingEntry). A
// longer array still publishes the slots it recorded; the tail keeps the
// engine's own motion instead of mixing identities. The cap is applied to the
// recording index only and never to the array extent, or an array longer than
// the cap would abort its whole update on the first slot past 64.
constexpr unsigned MaxElements = 64;

struct ThreadCapture
{
    std::uintptr_t proxy = 0, base = 0;
    std::uint32_t count = 0, outputStart = 0, next = 0, recorded = 0;
    std::uint32_t member[MaxElements] {};
    bool active = false;
};
// The grouped update calls the element append synchronously on the same
// thread, so one capture per thread is the whole state. A nested update does
// not occur in the audited body; the guard below still fails closed rather
// than publishing a mixed list.
thread_local ThreadCapture capture;

std::atomic<std::uint64_t> groupedCalls { 0 }, stagedCount { 0 }, publishedCount { 0 }, abortedCount { 0 };
std::atomic<std::uint64_t> appendCalls { 0 }, appendInRange { 0 }, appendOutside { 0 }, appendDropped { 0 };
std::atomic<std::uint64_t> sampleCalls { 0 }, sampleCycles { 0 }, sampleMaxCycles { 0 };

// Owner-thread relaxed increments: the counters are diagnostics only and the
// report thread only reads them.
inline void bump(std::atomic<std::uint64_t>& value) noexcept
{
    value.store(value.load(std::memory_order_relaxed) + 1, std::memory_order_relaxed);
}

// Array-owner header of the grouped update. These are the fields the engine
// itself reads in the same function (audited in
// glass-decompile/group-owner-record-layout-20260917.json): +0xea flag 0x2000
// selects the grouped branch, +0x108 is the 48-byte element array, +0x110 its
// count and +0x114 the pool start the copy uses.
bool readOwnerHeader(std::uintptr_t proxy, ThreadCapture& value) noexcept
{
    __try
    {
        const auto* bytes = reinterpret_cast<const unsigned char*>(proxy);
        std::uint16_t flags = 0;
        std::uintptr_t base = 0;
        std::uint32_t count = 0, outputStart = 0;
        std::memcpy(&flags, bytes + 0xea, sizeof(flags));
        std::memcpy(&base, bytes + 0x108, sizeof(base));
        std::memcpy(&count, bytes + 0x110, sizeof(count));
        std::memcpy(&outputStart, bytes + 0x114, sizeof(outputStart));
        if (!(flags & 0x2000) || !base || !count || outputStart == 0xffffffffu)
            return false;
        value.base = base;
        // The header count is the array's real extent and bounds the append
        // range check: the count*0x30 span is what tells whether an append came
        // from this array. The 64-element recording cap is applied per appended
        // element below, so it never truncates the span.
        value.count = count;
        value.outputStart = outputStart;
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
}

void grouped(void* proxyArg, void* context)
{
    if (DrawHooksIdle())
    {
        originalGrouped(proxyArg, context);
        return;
    }
    ThreadCapture& state = capture;
    if (state.active)
    {
        // Re-entry without the inner update finishing: drop the outer capture
        // instead of publishing a list that mixes two updates.
        state.active = false;
        bump(abortedCount);
    }
    const auto proxy = reinterpret_cast<std::uintptr_t>(proxyArg);
    if (proxy && readOwnerHeader(proxy, state))
    {
        state.proxy = proxy;
        state.next = state.outputStart;
        state.recorded = 0;
        state.active = true;
        bump(groupedCalls);
    }
    originalGrouped(proxyArg, context);
    if (!state.active)
        return;
    state.active = false;
    if (!state.recorded)
        return;
    bump(stagedCount);
    GlassArrayMappingEntry entry;
    entry.proxy = state.proxy;
    entry.outputStart = state.outputStart;
    entry.count = state.recorded;
    for (unsigned i = 0; i < state.recorded; ++i)
        entry.indices[i] = state.member[i];
    try
    {
        PublishArrayMapping(entry);
        bump(publishedCount);
    }
    catch (...)
    {
        bump(abortedCount);
    }
}

std::uintptr_t append(void* container, std::uintptr_t source)
{
    ThreadCapture& state = capture;
    if (!state.active)
        return originalAppend(container, source);
    const bool timed = HookCostSampleDue();
    const std::uint64_t begin = timed ? __rdtsc() : 0;
    bump(appendCalls);
    const std::uintptr_t base = state.base;
    const std::uintptr_t span = std::uintptr_t(state.count) * 0x30;
    const auto index = state.next - state.outputStart;
    ++state.next;
    if (source < base || source - base >= span)
    {
        // The append did not come from this array, so the captured base cannot
        // be trusted for the rest of the update either.
        bump(appendOutside);
        bump(abortedCount);
        state.active = false;
        state.recorded = 0;
    }
    else if (index < MaxElements)
    {
        state.member[index] = static_cast<std::uint32_t>((source - base) / 0x30);
        if (index + 1 > state.recorded)
            state.recorded = index + 1;
        bump(appendInRange);
    }
    else
        bump(appendDropped);
    const auto cycles = timed ? __rdtsc() - begin : 0;
    const auto result = originalAppend(container, source);
    if (timed)
    {
        bump(sampleCalls);
        sampleCycles.store(sampleCycles.load(std::memory_order_relaxed) + cycles, std::memory_order_relaxed);
        if (cycles > sampleMaxCycles.load(std::memory_order_relaxed))
            sampleMaxCycles.store(cycles, std::memory_order_relaxed);
    }
    return result;
}
} // namespace

GroupedArrayStats ReadGroupedArrayStats() noexcept
{
    GroupedArrayStats value;
    value.groupedCalls = groupedCalls.load(std::memory_order_relaxed);
    value.staged = stagedCount.load(std::memory_order_relaxed);
    value.published = publishedCount.load(std::memory_order_relaxed);
    value.aborted = abortedCount.load(std::memory_order_relaxed);
    value.appendCalls = appendCalls.load(std::memory_order_relaxed);
    value.appendInRange = appendInRange.load(std::memory_order_relaxed);
    value.appendOutside = appendOutside.load(std::memory_order_relaxed);
    value.appendDropped = appendDropped.load(std::memory_order_relaxed);
    value.sampleCalls = sampleCalls.load(std::memory_order_relaxed);
    value.sampleCycles = sampleCycles.load(std::memory_order_relaxed);
    value.sampleMaxCycles = sampleMaxCycles.load(std::memory_order_relaxed);
    return value;
}

bool InitializeCyberpunkGroups(HMODULE executable) noexcept
{
    try
    {
        static std::once_flag once;
        static bool ready = false;
        std::call_once(once,
                       [executable]
                       {
                           const auto* layout = GetCyberpunkLayout(executable);
                           if (!layout)
                               return;
                           auto* base = reinterpret_cast<unsigned char*>(executable);
                           HMODULE resident = nullptr;
                           if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                                       GET_MODULE_HANDLE_EX_FLAG_PIN,
                                                   reinterpret_cast<LPCWSTR>(&InitializeCyberpunkGroups),
                                                   &resident))
                               return;
                           DetourThreads threads;
                           if (!threads.gather() || DetourTransactionBegin() != NO_ERROR)
                               return;
                           // Detours writes the trampoline into the pointer that
                           // is passed to DetourAttach, so the attach has to
                           // target the globals the hook bodies call. Assigning
                           // them after the commit would leave a window where a
                           // patched call jumps to a null original.
                           originalGrouped = reinterpret_cast<GroupedFn>(
                               base + layout->functions[CyberpunkLayout::GroupedUpdate]);
                           originalAppend = reinterpret_cast<AppendFn>(
                               base + layout->functions[CyberpunkLayout::GroupAppend]);
                           bool attached =
                               DetourAttach(reinterpret_cast<PVOID*>(&originalGrouped),
                                            reinterpret_cast<PVOID>(&grouped)) == NO_ERROR;
                           attached = attached &&
                                      DetourAttach(reinterpret_cast<PVOID*>(&originalAppend),
                                                   reinterpret_cast<PVOID>(&append)) == NO_ERROR;
                           if (!attached || !threads.enlist() || DetourTransactionCommit() != NO_ERROR)
                           {
                               DetourTransactionAbort();
                               originalGrouped = nullptr;
                               originalAppend = nullptr;
                               return;
                           }
                           ready = true;
                       });
        return ready;
    }
    catch (...)
    {
        return false;
    }
}
} // namespace GlassFg
