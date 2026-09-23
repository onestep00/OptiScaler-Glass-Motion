// Live array element mapping hook.
//
// Cyberpunk's grouped-array path (proxy +0xEA flag 0x2000) rebuilds the draw's
// instance order from the group's source-index list:
//   0x1e8778(mesh, previousValid) walks the selected group records and, for every
//   16-bit source index, appends proxy+0x108[index*0x30] through 0x9c19e8 into
//   a container. The packet receives those entries in append order with the
//   output start stored at proxy+0x114.
//
// This plugin attaches both functions at runtime (no process restart) and logs
// the resulting element -> source-index mapping so the resident module can use
// engine data instead of rejecting grouped arrays.
//
// Do not run this plugin next to the module's own engine group hooks
// (CyberpunkGroups.cpp), which attach the same 0x9c19e8 from 2026-09-17 and
// publish to the same table (GlassArrayMapping). Both routes hooking one
// function would produce two entries for one append range. The plugin route is
// kept for sessions that must run without the engine hooks.
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

#ifdef GLASS_PLUGIN_CONTRACT_TEST
#include "../GlassArrayMapping.h"
#endif

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
PVOID vehHandle = nullptr;
FILE* logFile = nullptr;
std::atomic_flag logBusy = ATOMIC_FLAG_INIT;

// Buffered, non-blocking writer. Lines are flushed by the monitor thread every
// 250 ms instead of opening the file once per line on the render thread.
void flushLog() noexcept
{
    if (logFile)
        std::fflush(logFile);
}

bool readableRange(std::uintptr_t address, std::size_t bytes) noexcept
{
    if (!address || bytes == 0)
        return false;
    const std::uintptr_t end = address + bytes;
    if (end < address)
        return false;
    std::uintptr_t current = address & ~static_cast<std::uintptr_t>(0xFFF);
    while (current < end)
    {
        MEMORY_BASIC_INFORMATION info {};
        if (VirtualQuery(reinterpret_cast<const void*>(current), &info, sizeof(info)) != sizeof(info))
            return false;
        if (info.State != MEM_COMMIT || (info.Protect & (PAGE_NOACCESS | PAGE_GUARD)))
            return false;
        const auto next = reinterpret_cast<std::uintptr_t>(info.BaseAddress) + info.RegionSize;
        if (next <= current)
            return false;
        current = next;
    }
    return true;
}
// 0x1e8778 is the mesh vtable slot +0xf0: (mesh, previousTransformValid).
// The second argument is a boolean, not a pointer (see NativeOpaqueMvRoutes.md).
using GroupedPathFn = void (*)(std::uintptr_t mesh, std::uintptr_t previousValid);
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
// The mesh hook runs on several render threads. The per-call fields and the
// append collection must stay thread-local, otherwise thread B overwrites the
// object identity that thread A is about to publish.
thread_local Group current {};
std::atomic<unsigned> appends { 0 };
// The next live window has to separate "the append hook never ran" from "it ran
// but the source was outside the object's element array" and from "it ran before
// the mesh hook published a valid array". The 2026-09-17 02:0x sessions only
// logged ARRAY_MAP appends=0 with empty firstIndices, which fits all three.
std::atomic<unsigned> appendRejects { 0 }, appendInvalidContext { 0 }, appendSamples { 0 };
thread_local std::uint32_t firstIndices[64] {};
thread_local unsigned firstCount = 0;
std::atomic<std::uint64_t> publishedFrames { 0 };

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
    if (!logFile)
        return;
    if (logBusy.test_and_set(std::memory_order_acquire))
        return; // Never block a render thread on the logger.
    char line[2048] {};
    std::va_list args;
    va_start(args, format);
    std::vsnprintf(line, sizeof(line), format, args);
    va_end(args);
    SYSTEMTIME now {};
    GetLocalTime(&now);
    std::fprintf(logFile, "%02u:%02u:%02u.%03u %s\n", now.wHour, now.wMinute, now.wSecond, now.wMilliseconds, line);
    logBusy.clear(std::memory_order_release);
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
void dumpObject(const char* tag, unsigned call, std::uintptr_t pointer) noexcept;
void dumpOwner(const char* tag, unsigned call, std::uintptr_t proxy) noexcept;
struct ArrayMappingDraft
{
    unsigned count = 0;
    std::uint16_t indices[64] {};
};
bool buildMappingFromOwner(std::uintptr_t proxy, std::uint32_t arrayCount, ArrayMappingDraft& draft) noexcept;
std::atomic<unsigned> flaggedDumps { 0 };

// The owner scan is a fallback for objects whose element walk has not run yet.
// It probes engine memory and every accepted candidate costs a readableRange
// (VirtualQuery) round trip, so repeating it on every grouped call burned
// eleven cores on 2026-09-14. Remember each proxy that was already probed and
// retry only after a long call interval. Per render thread, no lock.
constexpr unsigned long long OwnerRetryInterval = 1ull << 16;
struct OwnerAttempt
{
    std::uintptr_t proxy = 0;
    unsigned long long nextCall = 0;
};
thread_local OwnerAttempt ownerAttempts[64] {};
thread_local unsigned ownerAttemptCursor = 0;

bool ownerScanAllowed(std::uintptr_t proxy, unsigned long long call) noexcept
{
    for (const auto& attempt : ownerAttempts)
        if (attempt.proxy == proxy)
            return call >= attempt.nextCall;
    return true;
}

void noteOwnerScan(std::uintptr_t proxy, unsigned long long call) noexcept
{
    for (auto& attempt : ownerAttempts)
        if (attempt.proxy == proxy)
        {
            attempt.nextCall = call + OwnerRetryInterval;
            return;
        }
    auto& slot = ownerAttempts[ownerAttemptCursor++ % std::size(ownerAttempts)];
    slot.proxy = proxy;
    slot.nextCall = call + OwnerRetryInterval;
}

// The fallback must never be able to run on every call again. The 2026-09-17
// 01:58 collapse showed the 64-entry memo alone does not bound it once the
// distinct-proxy population outgrows the table (585 proxies in the 65 s
// window), and the same path had already burned eleven cores on 2026-09-14.
// The monitor thread refills this budget; the render path only spends it, so
// one beat can pay for at most OwnerScanBurst scans no matter the call rate.
// Default is off: the append-derived mapping is the original engine path, and
// the scan produced 26 publishes in a 27-minute session.
constexpr unsigned OwnerScanBurst = 2;
std::atomic<unsigned> ownerScanBudget { 0 };
std::atomic<bool> ownerScanEnabled { false };
std::atomic<unsigned long long> ownerScans { 0 };

bool takeOwnerScanBudget() noexcept
{
    unsigned budget = ownerScanBudget.load(std::memory_order_relaxed);
    while (budget)
    {
        if (ownerScanBudget.compare_exchange_weak(budget, budget - 1, std::memory_order_relaxed))
            return true;
    }
    return false;
}

void hookedAppend(void* container, std::uintptr_t sourceMatrix)
{
    (void)container;
    ++inFlight;
    if (!draining.load(std::memory_order_relaxed))
    {
        const auto call = appendCalls.fetch_add(1, std::memory_order_relaxed) + 1;
        if (call <= 4)
            directLog("append_enter=%u source=%llx valid=%u", call,
                      static_cast<unsigned long long>(sourceMatrix), current.valid ? 1u : 0u);
        const bool inRange = current.valid && sourceMatrix >= current.arrayBase &&
                             sourceMatrix < current.arrayBase + std::uint64_t(current.count) * 0x30;
        if (appendSamples.fetch_add(1, std::memory_order_relaxed) < 8)
            directLog("append_sample=%u source=%llx base=%llx count=%u index=%lld in_range=%u", call,
                      static_cast<unsigned long long>(sourceMatrix),
                      static_cast<unsigned long long>(current.arrayBase), current.count,
                      inRange ? static_cast<long long>((sourceMatrix - current.arrayBase) / 0x30) : -1ll,
                      inRange ? 1u : 0u);
        if (inRange)
        {
            const auto index = std::uint32_t((sourceMatrix - current.arrayBase) / 0x30);
            if (firstCount < std::size(firstIndices))
                firstIndices[firstCount++] = index;
            appends.fetch_add(1, std::memory_order_relaxed);
        }
        else if (current.valid)
            appendRejects.fetch_add(1, std::memory_order_relaxed);
        else
            appendInvalidContext.fetch_add(1, std::memory_order_relaxed);
    }
    --inFlight;
}

void hookedGrouped(std::uintptr_t proxy, std::uintptr_t previousValid)
{
    // The whole entry, including the forwarded original call, is counted so
    // Detach can wait until no thread is inside this DLL before it is unloaded.
    ++inFlight;
    const bool active = !draining.load(std::memory_order_relaxed);
    unsigned call = 0;
    if (active)
    {
        call = groupedCalls.fetch_add(1, std::memory_order_relaxed) + 1;
        if (call <= 4)
            directLog("grouped_enter=%u proxy=%llx prev=%llx", call,
                      static_cast<unsigned long long>(proxy),
                      static_cast<unsigned long long>(previousValid));
        // Unreadable proxy fields must never crash the game: this hook runs on
        // the engine's render path.
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
        // Raw field probe so a wrong base object or a wrong layout shows up as
        // zeros instead of looking like "the path never has elements".
        if (call <= 4 || (call % 16384) == 0)
            probeFields("grouped", call, proxy);
        // Arrays (flag 0x2000) are rare; dump the full object so the element
        // list can be located from live bytes instead of another guess.
        if ((current.flags & 0x2000) && flaggedDumps.fetch_add(1, std::memory_order_relaxed) < 12)
        {
            dumpObject("flagged", call, proxy);
            dumpOwner("flagged_owner", call, proxy);
        }
    }
    if (originalGrouped)
        originalGrouped(proxy, previousValid);
    if (active)
    {
        const auto* api = host.load(std::memory_order_acquire);
        if (current.valid && current.outputStart != 0xFFFFFFFFu && api && api->publishArrayMapping)
        {
            unsigned count = 0;
            unsigned short indices[64] {};
            if (firstCount)
            {
                count = (std::min)(firstCount, 64u);
                for (unsigned i = 0; i < count; ++i)
                    indices[i] = static_cast<unsigned short>(firstIndices[i]);
            }
            else
            {
                ArrayMappingDraft draft;
                // Bounded fallback: at most one probe per proxy per
                // OwnerRetryInterval grouped calls on this thread.
                if (ownerScanAllowed(proxy, call) && takeOwnerScanBudget())
                {
                    ownerScans.fetch_add(1, std::memory_order_relaxed);
                    noteOwnerScan(proxy, call);
                    if (buildMappingFromOwner(proxy, current.count, draft))
                    {
                        count = draft.count;
                        for (unsigned i = 0; i < count; ++i)
                            indices[i] = draft.indices[i];
                    }
                }
            }
            if (count)
            {
                GlassArrayMappingEntry entry;
                entry.proxy = current.proxy;
                entry.outputStart = current.outputStart;
                entry.count = count;
                entry.frame = ++publishedFrames;
                for (unsigned i = 0; i < count; ++i)
                    entry.indices[i] = indices[i];
                api->publishArrayMapping(&entry);
                // The published payload is the only way to correlate a consumer
                // miss with what the plugin actually sent.
                static std::atomic<unsigned> publishLogs { 0 };
                if (publishLogs.fetch_add(1, std::memory_order_relaxed) < 24)
                    directLog("published proxy=%llx output_start=%u count=%u indices=%u,%u,%u,%u,%u,%u",
                              static_cast<unsigned long long>(entry.proxy), entry.outputStart, entry.count,
                              entry.indices[0], entry.indices[1], entry.indices[2], entry.indices[3], entry.indices[4],
                              entry.indices[5]);
            }
        }
        if (current.valid)
        {
            static unsigned reported = 0;
            if (++reported % 60 == 0)
            {
                char text[256] {};
                std::snprintf(text, sizeof(text),
                              "ARRAY_MAP proxy=%llx count=%u output_start=%u flags=%04x appends=%u append=%u "
                              "invalid=%u reject=%u indices=%u,%u,%u,%u,%u,%u",
                              static_cast<unsigned long long>(current.proxy), current.count, current.outputStart,
                              current.flags, appends.load(std::memory_order_relaxed),
                              appendCalls.load(std::memory_order_relaxed),
                              appendInvalidContext.load(std::memory_order_relaxed),
                              appendRejects.load(std::memory_order_relaxed), firstIndices[0], firstIndices[1],
                              firstIndices[2], firstIndices[3], firstIndices[4], firstIndices[5]);
                directLog("%s", text);
            }
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
    std::uintptr_t p18 = 0, p28 = 0, p40 = 0, p108 = 0;
    std::uint32_t p20 = 0, p24 = 0, p2c = 0, p30 = 0, p34 = 0, p48 = 0, p4c = 0, p50 = 0, p54 = 0, p110 = 0,
                  p114 = 0;
    std::uint16_t flags = 0;
    const bool read = readableRange(pointer + 0x18, 0x100);
    if (read)
    {
        p18 = *reinterpret_cast<std::uintptr_t*>(pointer + 0x18);
        p20 = *reinterpret_cast<std::uint32_t*>(pointer + 0x20);
        p24 = *reinterpret_cast<std::uint32_t*>(pointer + 0x24);
        p28 = *reinterpret_cast<std::uintptr_t*>(pointer + 0x28);
        p2c = *reinterpret_cast<std::uint32_t*>(pointer + 0x2c);
        p30 = *reinterpret_cast<std::uint32_t*>(pointer + 0x30);
        p34 = *reinterpret_cast<std::uint32_t*>(pointer + 0x34);
        p40 = *reinterpret_cast<std::uintptr_t*>(pointer + 0x40);
        p48 = *reinterpret_cast<std::uint32_t*>(pointer + 0x48);
        p4c = *reinterpret_cast<std::uint32_t*>(pointer + 0x4c);
        p50 = *reinterpret_cast<std::uint32_t*>(pointer + 0x50);
        p54 = *reinterpret_cast<std::uint32_t*>(pointer + 0x54);
        p108 = *reinterpret_cast<std::uintptr_t*>(pointer + 0x108);
        p110 = *reinterpret_cast<std::uint32_t*>(pointer + 0x110);
        p114 = *reinterpret_cast<std::uint32_t*>(pointer + 0x114);
        flags = *reinterpret_cast<std::uint16_t*>(pointer + 0xea);
    }
    if (p108)
        hitsBase.fetch_add(1, std::memory_order_relaxed);
    if (flags & 0x2000)
        hitsFlag.fetch_add(1, std::memory_order_relaxed);
    directLog("%s=%u ptr=%llx p18=%llx p20=%x p24=%x p28=%llx p2c=%x p30=%x p34=%x p40=%llx p48=%x p4c=%x p50=%x "
              "p54=%x p108=%llx p110=%x p114=%x pea=%04x read=%u",
              tag, call, static_cast<unsigned long long>(pointer), static_cast<unsigned long long>(p18), p20, p24,
              static_cast<unsigned long long>(p28), p2c, p30, p34, static_cast<unsigned long long>(p40), p48, p4c, p50,
              p54, static_cast<unsigned long long>(p108), p110, p114, flags, read ? 1u : 0u);
}

// Full-object hex dump (0x00..0x1F8) used once per rare array object.
void dumpObject(const char* tag, unsigned call, std::uintptr_t pointer) noexcept
{
    unsigned long long words[64] {};
    const bool read = readableRange(pointer, sizeof(words));
    if (read)
    {
        for (unsigned i = 0; i < 64; ++i)
            words[i] = *reinterpret_cast<unsigned long long*>(pointer + i * 8);
    }
    if (!read)
    {
        directLog("%s=%u ptr=%llx dump_fault=1", tag, call, static_cast<unsigned long long>(pointer));
        return;
    }
    char buffer[1500] {};
    int used = 0;
    for (unsigned i = 0; i < 64; ++i)
    {
        if (used >= static_cast<int>(sizeof(buffer)) - 32)
            break;
        used += std::snprintf(buffer + used, sizeof(buffer) - used, " %03x=%llx", i * 8, words[i]);
    }
    directLog("%s=%u ptr=%llx %s", tag, call, static_cast<unsigned long long>(pointer), buffer);
}

// proxy+0x70 is the group owner (NativeOpaqueMvRoutes.md paragraph 198). The
// element list lives at owner+0x18, the element count at owner+0x3c and the
// output start at owner+0x50. Log both interpretations of +0x18 (inline 16-bit
// entries or a pointer to them) so the next run needs no extra guess.
struct OwnerCandidate
{
    unsigned offset = 0;
    std::uintptr_t pointer = 0, list = 0;
    unsigned count = 0, outputStart = 0;
};

// Pure search so the offline self-test can assert on the same code the live
// probe uses. Never dereferences an unvalidated pointer.
unsigned findOwnerCandidates(std::uintptr_t proxy, OwnerCandidate* out, unsigned max) noexcept
{
    if (!out || !max || !readableRange(proxy, 0x200))
        return 0;
    unsigned found = 0;
    for (unsigned offset = 0; offset < 0x200 && found < max; offset += 8)
    {
        const auto candidate = *reinterpret_cast<std::uintptr_t*>(proxy + offset);
        if (candidate < 0x10000ull || candidate > 0x7FFFFFFFFFFFull || !readableRange(candidate, 0x60))
            continue;
        const auto list = *reinterpret_cast<std::uintptr_t*>(candidate + 0x18);
        const auto count = *reinterpret_cast<std::uint32_t*>(candidate + 0x3c);
        const auto outputStart = *reinterpret_cast<std::uint32_t*>(candidate + 0x50);
        if (count == 0 || count > 0x4000)
            continue;
        out[found].offset = offset;
        out[found].pointer = candidate;
        out[found].list = list;
        out[found].count = count;
        out[found].outputStart = outputStart;
        ++found;
    }
    return found;
}

// Turns the engine's own element list into the packet mapping. Accepts a
// candidate only when its element count matches the object's source-array count
// and every entry is a valid source index, so unverified memory never becomes a
// published mapping.
bool buildMappingFromOwner(std::uintptr_t proxy, std::uint32_t arrayCount, ArrayMappingDraft& draft) noexcept
{
    if (arrayCount == 0 || arrayCount > 0x4000)
        return false;
    OwnerCandidate candidates[8] {};
    const auto found = findOwnerCandidates(proxy, candidates, 8);
    const auto wanted = (std::min)(arrayCount, 64u);
    for (unsigned index = 0; index < found; ++index)
    {
        const auto& candidate = candidates[index];
        if (candidate.count != arrayCount || candidate.list < 0x10000ull ||
            candidate.list > 0x7FFFFFFFFFFFull || !readableRange(candidate.list, wanted * 2u))
            continue;
        ArrayMappingDraft candidateDraft;
        bool valid = true;
        for (unsigned i = 0; i < wanted; ++i)
        {
            const auto entry = *reinterpret_cast<unsigned short*>(candidate.list + i * 2);
            if (entry >= arrayCount)
            {
                valid = false;
                break;
            }
            candidateDraft.indices[i] = entry;
        }
        if (!valid)
            continue;
        // A grouped-array selection lists each element once. Duplicates mean the
        // candidate is not the object's own element list, so publishing it would
        // attach the wrong source index to every repeated element.
        bool duplicate = false;
        for (unsigned i = 0; i < wanted && !duplicate; ++i)
            for (unsigned j = i + 1; j < wanted; ++j)
                if (candidateDraft.indices[i] == candidateDraft.indices[j])
                {
                    duplicate = true;
                    break;
                }
        if (duplicate)
            continue;
        candidateDraft.count = wanted;
        draft = candidateDraft;
        return true;
    }
    return false;
}

void dumpOwner(const char* tag, unsigned call, std::uintptr_t proxy) noexcept
{
    // The documented +0x70 lookup is not a plain pointer in practice
    // (0x13d300000337e on 2026-09-14), so scan for the documented group shape.
    OwnerCandidate candidates[8] {};
    const auto found = findOwnerCandidates(proxy, candidates, 8);
    if (!found)
        directLog("%s=%u proxy=%llx owner_candidates=0", tag, call, static_cast<unsigned long long>(proxy));
    for (unsigned index = 0; index < found; ++index)
    {
        const auto& candidate = candidates[index];
        directLog("%s=%u candidate_off=%03x candidate=%llx count=%u out_start=%u p18=%llx", tag, call, candidate.offset,
                  static_cast<unsigned long long>(candidate.pointer), candidate.count, candidate.outputStart,
                  static_cast<unsigned long long>(candidate.list));
        if (candidate.list < 0x10000ull || candidate.list > 0x7FFFFFFFFFFFull || !readableRange(candidate.list, 32))
            continue;
        unsigned short entries[16] {};
        for (unsigned i = 0; i < 16; ++i)
            entries[i] = *reinterpret_cast<unsigned short*>(candidate.list + i * 2);
        char text[220] {};
        int used = 0;
        for (unsigned i = 0; i < 16; ++i)
            used += std::snprintf(text + used, sizeof(text) - used, "%u,", entries[i]);
        directLog("%s=%u entries=%s", tag, call, text);
    }
    std::uintptr_t owner = 0;
    const bool readOwner = readableRange(proxy + 0x70, 8);
    if (readOwner)
        owner = *reinterpret_cast<std::uintptr_t*>(proxy + 0x70);
    if (!readOwner || !owner || owner < 0x10000ull || owner > 0x7FFFFFFFFFFFull || !readableRange(owner, 0x60))
    {
        directLog("%s=%u proxy=%llx owner=%llx read=%u", tag, call, static_cast<unsigned long long>(proxy),
                  static_cast<unsigned long long>(owner), readOwner ? 1u : 0u);
        return;
    }
    char inlineText[160] {};
    int inlineUsed = 0;
    unsigned short inlineEntries[8] {};
    unsigned count = 0, outputStart = 0;
    std::uintptr_t list = 0;
    const bool read = true; // owner range already validated above
    if (read)
    {
        for (unsigned i = 0; i < 8; ++i)
            inlineEntries[i] = *reinterpret_cast<unsigned short*>(owner + 0x18 + i * 2);
        list = *reinterpret_cast<std::uintptr_t*>(owner + 0x18);
        count = *reinterpret_cast<std::uint32_t*>(owner + 0x3c);
        outputStart = *reinterpret_cast<std::uint32_t*>(owner + 0x50);
    }
    if (!read)
    {
        directLog("%s=%u owner=%llx owner_fault=1", tag, call, static_cast<unsigned long long>(owner));
        return;
    }
    for (unsigned i = 0; i < 8; ++i)
        inlineUsed += std::snprintf(inlineText + inlineUsed, sizeof(inlineText) - inlineUsed, "%u,", inlineEntries[i]);
    directLog("%s=%u proxy=%llx owner=%llx count=%u out_start=%u p18=%llx inline16=%s", tag, call,
              static_cast<unsigned long long>(proxy), static_cast<unsigned long long>(owner), count, outputStart,
              static_cast<unsigned long long>(list), inlineText);
    if (list < 0x10000ull || list > 0x7FFFFFFFFFFFull)
        return;
    unsigned short entries[16] {};
    const bool readList = readableRange(list, sizeof(entries));
    if (readList)
    {
        for (unsigned i = 0; i < 16; ++i)
            entries[i] = *reinterpret_cast<unsigned short*>(list + i * 2);
    }
    if (!readList)
    {
        directLog("%s=%u list=%llx list_fault=1", tag, call, static_cast<unsigned long long>(list));
        return;
    }
    char listText[220] {};
    int listUsed = 0;
    for (unsigned i = 0; i < 16; ++i)
        listUsed += std::snprintf(listText + listUsed, sizeof(listText) - listUsed, "%u,", entries[i]);
    directLog("%s=%u list=%llx ptr16=%s", tag, call, static_cast<unsigned long long>(list), listText);
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
        flushLog();
        if (ownerScanEnabled.load(std::memory_order_relaxed))
            ownerScanBudget.store(OwnerScanBurst, std::memory_order_relaxed);
        directLog("beat=%u grouped=%u append=%u invalid=%u reject=%u basehits=%u flaghits=%u ownerscan=%u scans=%u",
                  ++beat,
                  groupedCalls.load(std::memory_order_relaxed), appendCalls.load(std::memory_order_relaxed),
                  appendInvalidContext.load(std::memory_order_relaxed),
                  appendRejects.load(std::memory_order_relaxed),
                  hitsBase.load(std::memory_order_relaxed), hitsFlag.load(std::memory_order_relaxed),
                  ownerScanEnabled.load(std::memory_order_relaxed) ? 1u : 0u,
                  static_cast<unsigned>(ownerScans.load(std::memory_order_relaxed)));
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
    if (!ownLogPath.empty() && !logFile)
    {
        logFile = _wfopen(ownLogPath.c_str(), L"a");
        if (logFile)
            std::setvbuf(logFile, nullptr, _IOFBF, 64 * 1024);
    }
    directLog("attach_begin exe=%p grouped=%p append=%p", GetModuleHandleW(nullptr),
              reinterpret_cast<void*>(reinterpret_cast<std::byte*>(GetModuleHandleW(nullptr)) + 0x1e8778),
              reinterpret_cast<void*>(reinterpret_cast<std::byte*>(GetModuleHandleW(nullptr)) + 0x9c19e8));
    // The vectored handler is diagnostic only: it writes from inside exception
    // dispatch, which is not safe to keep on by default.
    if (markerPresent(L"plugin-veh.on"))
    {
        vehHandle = AddVectoredExceptionHandler(1, &faultHandler);
        if (!vehHandle)
            directLog("veh_failed=1");
    }
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
    const bool ownerScan = markerPresent(L"plugin-ownerscan.on");
    ownerScanEnabled.store(ownerScan, std::memory_order_relaxed);
    directLog("attach switches grouped=%u append=%u passthrough=%u ownerscan=%u", skipGrouped ? 0u : 1u,
              skipAppend ? 0u : 1u, passThrough ? 1u : 0u, ownerScan ? 1u : 0u);
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
    // A stale vectored handler points into this DLL. Once the caller unmaps it
    // the next process exception would run unmapped code, so remove it first.
    if (vehHandle)
    {
        RemoveVectoredExceptionHandler(vehHandle);
        vehHandle = nullptr;
        directLog("veh_removed=1");
    }
    // A hook body can still be running on a render thread when the module calls
    // FreeLibrary. Unloading straight away crashed the game on 2026-09-14
    // 17:48:55. Drain until the in-flight count stays at zero, then detach.
    draining.store(true, std::memory_order_relaxed);
    for (unsigned stable = 0, spins = 0; stable < 8 && spins < 400; ++spins)
    {
        if (inFlight.load(std::memory_order_relaxed) == 0)
            ++stable;
        else
            stable = 0;
        Sleep(5);
    }
    Sleep(200);
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
    // The detour transaction only fixes threads inside the patched region; give
    // anything still inside our hook body time to leave before the caller
    // unmaps the DLL.
    for (unsigned stable = 0, spins = 0; stable < 8 && spins < 400; ++spins)
    {
        if (inFlight.load(std::memory_order_relaxed) == 0)
            ++stable;
        else
            stable = 0;
        Sleep(5);
    }
    Sleep(200);
    originalGrouped = nullptr;
    originalAppend = nullptr;
    glassArrayMapAppendTarget = nullptr;
    glassArrayMapGroupedTarget = nullptr;
    directLog("detached");
    trace("ARRAY_MAP detached=1");
    host.store(nullptr, std::memory_order_release);
    draining.store(false, std::memory_order_relaxed);
    flushLog();
    if (logFile)
    {
        std::fclose(logFile);
        logFile = nullptr;
    }
}

BOOL WINAPI DllMain(HINSTANCE instance, DWORD, LPVOID)
{
    selfModule = instance;
    return TRUE;
}

#ifdef GLASS_PLUGIN_SELFTEST
// Offline verification of the exact probe code that runs on the render thread.
// Built and run by build-plugin.ps1 before the DLL is deployed.
int main()
{
    int failures = 0;
    alignas(16) unsigned char buffer[0x200] {};
    failures += !readableRange(reinterpret_cast<std::uintptr_t>(buffer), 0x100);
    failures += readableRange(0ull, 8);
    failures += readableRange(0x13d300000337eull, 8); // exact live value that crashed the game
    alignas(8) unsigned char object[0x200] {};
    OwnerCandidate candidates[8] {};
    *reinterpret_cast<std::uintptr_t*>(object + 0x70) = 0x13d300000337eull;
    auto found = findOwnerCandidates(reinterpret_cast<std::uintptr_t>(object), candidates, 8);
    if (found != 0)
        ++failures;
    dumpOwner("selftest_bogus", 1, reinterpret_cast<std::uintptr_t>(object));
    alignas(8) unsigned short list[64] {};
    for (unsigned i = 0; i < 40; ++i)
        list[i] = static_cast<unsigned short>((i + 3u) % 40u);
    alignas(8) unsigned char group[0x60] {};
    *reinterpret_cast<std::uintptr_t*>(group + 0x18) = reinterpret_cast<std::uintptr_t>(list);
    *reinterpret_cast<std::uint32_t*>(group + 0x3c) = 40;
    *reinterpret_cast<std::uint32_t*>(group + 0x50) = 0x5300;
    *reinterpret_cast<std::uintptr_t*>(object + 0xd8) = reinterpret_cast<std::uintptr_t>(group);
    found = findOwnerCandidates(reinterpret_cast<std::uintptr_t>(object), candidates, 8);
    if (found != 1)
        ++failures;
    else
    {
        failures += candidates[0].offset != 0xd8;
        failures += candidates[0].count != 40;
        failures += candidates[0].outputStart != 0x5300;
        failures += candidates[0].list != reinterpret_cast<std::uintptr_t>(list);
    }
    dumpOwner("selftest_group", 2, reinterpret_cast<std::uintptr_t>(object));
    ArrayMappingDraft draft;
    const bool mapped = buildMappingFromOwner(reinterpret_cast<std::uintptr_t>(object), 40, draft);
    if (!mapped || draft.count != 40 || draft.indices[0] != 3 || draft.indices[1] != 4 || draft.indices[2] != 5)
        ++failures;
    // An owner whose count does not match the object's array count must be
    // rejected instead of published.
    ArrayMappingDraft rejected;
    if (buildMappingFromOwner(reinterpret_cast<std::uintptr_t>(object), 41, rejected))
        ++failures;
    // Out-of-range entries must also be rejected.
    list[0] = 40; // arrayCount is 40, so index 40 is invalid
    ArrayMappingDraft outOfRange;
    if (buildMappingFromOwner(reinterpret_cast<std::uintptr_t>(object), 40, outOfRange))
        ++failures;
    // A repeated source index means this is not the object's own element list.
    list[0] = 3;
    list[1] = 3;
    ArrayMappingDraft duplicate;
    if (buildMappingFromOwner(reinterpret_cast<std::uintptr_t>(object), 40, duplicate))
        ++failures;
    // The next live window separates "hook never entered" (append=0) from
    // "entered but out of range" (reject>0) and from "entered before the mesh
    // hook published a valid array" (invalid>0), so the counters must move
    // exactly once per append.
    current.proxy = 0x1000;
    current.arrayBase = reinterpret_cast<std::uintptr_t>(object);
    current.count = 2;
    current.outputStart = 0;
    current.valid = true;
    const auto entersBefore = appendCalls.load();
    const auto rejectsBefore = appendRejects.load();
    const auto invalidBefore = appendInvalidContext.load();
    hookedAppend(nullptr, current.arrayBase);
    hookedAppend(nullptr, current.arrayBase + 2 * 0x30);
    if (appendCalls.load() != entersBefore + 2 || appendRejects.load() != rejectsBefore + 1 ||
        appendInvalidContext.load() != invalidBefore)
        ++failures;
    current.valid = false;
    hookedAppend(nullptr, 0);
    if (appendInvalidContext.load() != invalidBefore + 1 || appendCalls.load() != entersBefore + 3)
        ++failures;
    current.proxy = 0;
    current.arrayBase = 0;
    current.count = 0;
    current.outputStart = 0;
    std::printf("PLUGIN_SELFTEST failures=%d bogus_value_ignored=1 group_shape_found=%u mapping_built=%d "
                "append_counters_checked=1\n",
                failures, found, mapped ? 1 : 0);
    return failures ? 1 : 0;
}
#endif

#ifdef GLASS_PLUGIN_CONTRACT_TEST
// End-to-end contract test for the live plugin -> module interface: build a
// mapping from a synthetic engine object, publish it through the module table,
// and consume it the way GlassMotionIdentity does (outputStart + ordinal).
int main()
{
    int failures = 0;
    alignas(8) unsigned char object[0x200] {};
    alignas(8) unsigned char group[0x60] {};
    alignas(8) unsigned short list[64] {};
    constexpr unsigned kCount = 16;
    for (unsigned i = 0; i < kCount; ++i)
        list[i] = static_cast<unsigned short>(kCount - 1 - i); // reversed selection
    *reinterpret_cast<std::uintptr_t*>(group + 0x18) = reinterpret_cast<std::uintptr_t>(list);
    *reinterpret_cast<std::uint32_t*>(group + 0x3c) = kCount;
    *reinterpret_cast<std::uint32_t*>(group + 0x50) = 0x100;
    *reinterpret_cast<std::uintptr_t*>(object + 0xd8) = reinterpret_cast<std::uintptr_t>(group);

    ArrayMappingDraft draft;
    if (!buildMappingFromOwner(reinterpret_cast<std::uintptr_t>(object), kCount, draft))
        ++failures;
    else
    {
        GlassFg::GlassArrayMappingEntry entry;
        entry.proxy = reinterpret_cast<std::uintptr_t>(object);
        entry.outputStart = 0x100;
        entry.count = draft.count;
        for (unsigned i = 0; i < draft.count && i < 64; ++i)
            entry.indices[i] = draft.indices[i];
        GlassFg::PublishArrayMapping(entry);
        for (unsigned ordinal = 0; ordinal < kCount; ++ordinal)
        {
            const auto mapped = GlassFg::LookupArrayMapping(entry.proxy, entry.outputStart + ordinal);
            if (mapped != list[ordinal])
                ++failures;
        }
        if (GlassFg::LookupArrayMapping(entry.proxy, entry.outputStart + kCount) != UINT32_MAX)
            ++failures;
    }
    std::printf("PLUGIN_CONTRACT_TEST failures=%d reversed_selection_preserved=1\n", failures);
    return failures ? 1 : 0;
}
#endif

#ifdef GLASS_PLUGIN_HOOKBENCH
// Offline cost of the exact hook bodies that run on the render threads.
//
// On 2026-09-17 01:58 the live log recorded ~19,400 grouped calls per second
// (7,760 per engine frame) while the engine frame rate collapsed from 23 fps
// to 2.5 fps. This bench answers whether the body itself can pay for that, so
// "hook body cost" and "detour patch cost" can be separated. The trampoline is
// replaced by a stub that walks the same element appends the engine's grouped
// path performs, and the logger is the same 64 KiB buffered FILE* the live
// build opens.
#include <chrono>
#include <thread>
#include <vector>

namespace
{
// Live rates from the 01:58 window: grouped calls per second and per engine
// frame at 2.5 engine fps.
constexpr unsigned long long LiveGroupedCallsPerSecond = 19400;
constexpr unsigned long long LiveGroupedCallsPerEngineFrame = 7760;

std::atomic<unsigned long long> benchPublished { 0 };
std::atomic<unsigned long long> benchTrampoline { 0 };
unsigned long long benchElementsPerCall = 0;
std::uintptr_t benchElementBase = 0;
std::uintptr_t benchProxyTable[1024] {};
unsigned benchProxyCount = 1;

void benchPublish(const GlassArrayMappingEntry*) noexcept
{
    benchPublished.fetch_add(1, std::memory_order_relaxed);
}

// Stands in for the original grouped path: the engine walks the selected
// elements and calls the append function once per element.
void benchTrampolineBody(std::uintptr_t, std::uintptr_t) noexcept
{
    benchTrampoline.fetch_add(1, std::memory_order_relaxed);
    for (unsigned long long i = 0; i < benchElementsPerCall; ++i)
        hookedAppend(nullptr, benchElementBase + i * 0x30);
}

struct HookBenchResult
{
    double nsPerCall = 0.0;
    unsigned long long calls = 0;
    unsigned long long scans = 0;
};

HookBenchResult runHookPattern(const char* name, unsigned threadCount, unsigned long long callsPerThread,
                               std::uintptr_t proxy)
{
    std::atomic<bool> start { false };
    std::vector<std::thread> threads;
    for (unsigned id = 0; id < threadCount; ++id)
    {
        threads.emplace_back(
            [&]()
            {
                while (!start.load(std::memory_order_acquire))
                    std::this_thread::yield();
                for (unsigned long long i = 0; i < callsPerThread; ++i)
                    hookedGrouped(benchProxyCount > 1 ? benchProxyTable[i % benchProxyCount] : proxy, 1);
            });
    }
    const auto scansBegin = ownerScans.load(std::memory_order_relaxed);
    const auto begin = std::chrono::steady_clock::now();
    start.store(true, std::memory_order_release);
    for (auto& thread : threads)
        thread.join();
    const auto elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - begin).count();
    const auto calls = callsPerThread * threadCount;
    const auto scans = ownerScans.load(std::memory_order_relaxed) - scansBegin;
    const double perCall = calls ? elapsed * 1.0e9 / double(calls) : 0.0;
    std::printf("HOOKBENCH_RESULT %s threads=%u calls=%llu ns_per_call=%.1f "
                "ms_per_second_at_%llu=%.3f ms_per_engine_frame_at_%llu=%.4f scans=%llu scans_per_call=%.3f\n",
                name, threadCount, calls, perCall, LiveGroupedCallsPerSecond,
                perCall * double(LiveGroupedCallsPerSecond) / 1.0e6, LiveGroupedCallsPerEngineFrame,
                perCall * double(LiveGroupedCallsPerEngineFrame) / 1.0e6, scans,
                calls ? double(scans) / double(calls) : 0.0);
    std::fflush(stdout);
    return { perCall, calls, scans };
}
} // namespace

int main()
{
    int failures = 0;
    // Synthetic proxy: +0x108 element array, +0x110 count, +0x114 output start,
    // +0xEA flags. Same layout the probe reads in game.
    alignas(16) unsigned char elementStorage[64 * 0x30] {};
    alignas(16) unsigned char proxy[0x200] {};
    *reinterpret_cast<std::uintptr_t*>(proxy + 0x108) = reinterpret_cast<std::uintptr_t>(elementStorage);
    *reinterpret_cast<std::uint32_t*>(proxy + 0x110) = 8;
    *reinterpret_cast<std::uint32_t*>(proxy + 0x114) = 0x100;
    *reinterpret_cast<std::uint16_t*>(proxy + 0xea) = 0;
    const auto proxyAddress = reinterpret_cast<std::uintptr_t>(proxy);
    benchElementBase = reinterpret_cast<std::uintptr_t>(elementStorage);

    hostStorage.version = 1;
    hostStorage.log = nullptr;
    hostStorage.moduleDirectory = nullptr;
    hostStorage.engineUpdate = nullptr;
    hostStorage.publishArrayMapping = &benchPublish;
    hostStorage.trace = nullptr;
    host.store(&hostStorage, std::memory_order_release);
    originalGrouped = &benchTrampolineBody;
    draining.store(false, std::memory_order_relaxed);

    // Same buffered logger the live build uses, so the beat probe measures the
    // real flush cost instead of a guessed one.
    wchar_t logPath[MAX_PATH] {};
    if (GetTempPathW(MAX_PATH, logPath) != 0)
    {
        std::wstring path { logPath };
        path += L"glass-plugin-hookbench.log";
        logFile = _wfopen(path.c_str(), L"a");
        if (logFile)
            std::setvbuf(logFile, nullptr, _IOFBF, 64 * 1024);
    }

    // Warm up the thread_local state and the logger so the first call does not
    // pay for the owner scan inside the measured window.
    benchElementsPerCall = 0;
    for (unsigned i = 0; i < 1000; ++i)
        hookedGrouped(proxyAddress, 1);

    const auto groupedOnly = runHookPattern("grouped-only", 1, 2000000, proxyAddress);

    benchElementsPerCall = 8;
    const auto groupedAppendPublish = runHookPattern("grouped+8-append+publish", 1, 1000000, proxyAddress);
    const auto groupedAppendPublishMt = runHookPattern("grouped+8-append+publish", 4, 250000, proxyAddress);
    benchElementsPerCall = 0;
    const auto groupedOnlyMt = runHookPattern("grouped-only", 4, 500000, proxyAddress);

    // Owner-scan fallback under a live-sized proxy population. The 01:58 window
    // recorded 585 distinct proxies in 65 s against a 64-entry per-thread memo,
    // so the fallback re-armed on nearly every call while append=0 kept
    // firstCount at zero. Reproduce that shape: 1024 rotating proxies, each
    // carrying readable but mismatching owner candidates so every scan performs
    // real VirtualQuery work. The first phase keeps the old unbounded budget,
    // the second uses the shipped default (off) and must stop the scans.
    {
        constexpr unsigned kThrashProxies = 1024;
        std::vector<unsigned char> pool(static_cast<std::size_t>(kThrashProxies) * 0x200 + 0x100);
        alignas(8) unsigned char thrashOwner[0x60] {};
        *reinterpret_cast<std::uintptr_t*>(thrashOwner + 0x18) =
            reinterpret_cast<std::uintptr_t>(thrashOwner);
        *reinterpret_cast<std::uint32_t*>(thrashOwner + 0x3c) = 4; // != arrayCount 8
        *reinterpret_cast<std::uint32_t*>(thrashOwner + 0x50) = 0x200;
        for (unsigned i = 0; i < kThrashProxies; ++i)
        {
            auto* p = pool.data() + static_cast<std::size_t>(i) * 0x200;
            *reinterpret_cast<std::uintptr_t*>(p + 0x108) = reinterpret_cast<std::uintptr_t>(elementStorage);
            *reinterpret_cast<std::uint32_t*>(p + 0x110) = 8;
            *reinterpret_cast<std::uint32_t*>(p + 0x114) = 0x100;
            *reinterpret_cast<std::uintptr_t*>(p + 0xd8) = reinterpret_cast<std::uintptr_t>(thrashOwner);
            *reinterpret_cast<std::uintptr_t*>(p + 0x118) = reinterpret_cast<std::uintptr_t>(thrashOwner);
            *reinterpret_cast<std::uintptr_t*>(p + 0x1c8) = reinterpret_cast<std::uintptr_t>(thrashOwner);
            *reinterpret_cast<std::uintptr_t*>(p + 0x1d0) = reinterpret_cast<std::uintptr_t>(thrashOwner);
            benchProxyTable[i] = reinterpret_cast<std::uintptr_t>(p);
        }
        benchProxyCount = kThrashProxies;

        ownerScanEnabled.store(true, std::memory_order_relaxed);
        ownerScanBudget.store(~0u, std::memory_order_relaxed); // 01:58 behaviour
        const auto unbounded = runHookPattern("owner-scan-thrash-1024-unbounded", 1, 100000, proxyAddress);
        ownerScanBudget.store(0, std::memory_order_relaxed); // shipped default
        const auto budgeted = runHookPattern("owner-scan-thrash-1024-budget-off", 1, 100000, proxyAddress);

        // Dense variant: a real engine object holds pointers in most of its 64
        // qwords, so findOwnerCandidates pays one readableRange (VirtualQuery)
        // per slot before the count check rejects the candidate. The pointers
        // go to distinct pages whose +0x3c count is zero, so the search walks
        // every slot instead of stopping at the eighth accepted candidate.
        std::vector<unsigned char> rejectPool(static_cast<std::size_t>(64) * 0x1000);
        for (unsigned i = 0; i < kThrashProxies; ++i)
        {
            auto* p = pool.data() + static_cast<std::size_t>(i) * 0x200;
            unsigned slot = 0;
            for (unsigned offset = 0; offset < 0x200; offset += 8)
            {
                if (offset == 0xe8 || offset == 0x108 || offset == 0x110)
                    continue;
                *reinterpret_cast<std::uintptr_t*>(p + offset) =
                    reinterpret_cast<std::uintptr_t>(rejectPool.data() +
                                                     static_cast<std::size_t>(slot % 64) * 0x1000);
                ++slot;
            }
        }
        ownerScanBudget.store(~0u, std::memory_order_relaxed);
        const auto dense = runHookPattern("owner-scan-thrash-1024-dense-unbounded", 1, 20000, proxyAddress);
        benchProxyCount = 1;
        ownerScanEnabled.store(false, std::memory_order_relaxed);

        if (unbounded.scans < unbounded.calls / 2)
            ++failures; // the live thrash shape must reproduce here
        if (budgeted.scans != 0)
            ++failures; // the render-path budget must stop the fallback
        if (dense.scans != dense.calls)
            ++failures; // dense pointer slots must scan on every call
    }

    // Beat probe: one monitor tick is a flush plus one formatted line. The
    // live loop sleeps 250 ms between ticks, so four ticks per second.
    double beatNs = 0.0;
    constexpr unsigned beats = 2000;
    {
        const auto begin = std::chrono::steady_clock::now();
        for (unsigned i = 0; i < beats; ++i)
        {
            flushLog();
            directLog("beat=%u grouped=%u append=%u", i, 1u, 1u);
        }
        flushLog();
        beatNs = std::chrono::duration<double>(std::chrono::steady_clock::now() - begin).count() * 1.0e9 / double(beats);
    }
    std::printf("HOOKBENCH_RESULT beat-thread ns_per_beat=%.1f ms_per_second_at_4Hz=%.4f\n", beatNs, beatNs * 4.0 / 1.0e6);

    if (benchPublished.load(std::memory_order_relaxed) == 0)
        ++failures; // the publish path must actually run in the append phases
    if (benchTrampoline.load() < groupedOnly.calls)
        ++failures;
    std::printf("PLUGIN_HOOKBENCH_FAILURES=%d\n", failures);
    std::fflush(stdout);
    if (logFile)
    {
        std::fclose(logFile);
        logFile = nullptr;
    }
    std::printf("PLUGIN_HOOKBENCH_OK\n");
    return failures ? 1 : 0;
}
#endif
