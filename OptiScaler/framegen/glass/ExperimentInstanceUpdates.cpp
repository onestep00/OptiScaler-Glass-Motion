// Standalone CPU diagnostic; not linked into OptiScaler. Stop leaves the
// forwarding hook pinned until process exit. No transform arrays/GPU copies.
#include "DetourThreads.h"
#include "GeometrySourceSlots.h"
#ifdef GLASS_NODE_LIFETIME
#define GLASS_NODE_GROUP
#include "GeometrySourceOwners.h"
#include "ExperimentSourceAbi.h"
#endif
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <vector>
#include <intrin.h>

namespace
{
#if (defined(GLASS_ARRAY_WRAPPER) + defined(GLASS_TRANSFORM_RANGE) + defined(GLASS_NODE_GROUP)) > 1
#error Select one diagnostic ABI
#endif
#ifdef GLASS_NODE_GROUP
using EnqueueResult = void;
using Enqueue = void (*)(void*, float, void*, void*);
#ifdef GLASS_NODE_LIFETIME
constexpr unsigned profileMagic = 0x49555035;
using Destroy = void (*)(void*);
Destroy originalDestroy = nullptr;
GlassFg::GeometrySourceOwners<131072> sourceOwners;
std::mutex sourceMutex;
std::atomic<std::uint64_t> sourceCreated = 0, sourceDestroyed = 0, sourceRejected = 0;
std::atomic<bool> sourceReady = false, sourceHealthy = true;
#else
constexpr unsigned profileMagic = 0x49555034;
#endif
#elif defined(GLASS_TRANSFORM_RANGE)
using EnqueueResult = void*;
using Enqueue = EnqueueResult (*)(void*, void*);
constexpr unsigned profileMagic = 0x49555033;
#elif defined(GLASS_ARRAY_WRAPPER)
using EnqueueResult = unsigned char;
using Enqueue = unsigned char (*)(void*, void*, void*);
constexpr unsigned profileMagic = 0x49555032;
#else
using EnqueueResult = unsigned char;
using Enqueue = unsigned char (*)(void*, void*);
constexpr unsigned profileMagic = 0x49555031;
#endif
Enqueue original = nullptr;
HMODULE self = nullptr;
std::atomic<bool> recording = false;
std::atomic<unsigned> active = 0, used = 0;
std::atomic<bool> arraysOnly = false;
std::atomic<std::uint64_t> filtered = 0, malformed = 0;
std::mutex control;
std::filesystem::path output;
struct Row
{
    std::uint64_t caller = 0, context = 0, input = 0;
    std::array<std::uint64_t, 18> header {};
    unsigned thread = 0;
    bool valid = false;
};
std::array<Row, 4096> rows;
bool read(std::uint64_t address, void* data, unsigned bytes) noexcept
{
    __try
    {
        if (address < 0x10000 || address > 0x7fffffffffffULL - bytes) return false;
        memcpy(data, reinterpret_cast<const void*>(address), bytes); return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
#ifdef GLASS_NODE_GROUP
#ifdef GLASS_NODE_LIFETIME
// Invalidate before the original destructor releases its retained proxy.
// Never hold the cache lock while executing engine code (which may reenter).
void destroy(void* handle)
{
    std::uint64_t proxy = 0;
    const auto address = reinterpret_cast<std::uint64_t>(handle);
    if (address >= 0x10000 && address <= 0x7fffffffffffULL - 24 && read(address + 16, &proxy, 8))
    {
        std::lock_guard lock(sourceMutex);
        sourceOwners.destroyed(address, proxy);
        ++sourceDestroyed;
    }
    else
    {
        ++sourceRejected;
        sourceHealthy.store(false, std::memory_order_release);
    }
    originalDestroy(handle);
}
#endif
// Audited outer creation ABI: RCX=node instance, XMM1=distance parameter,
// R8=source span, R9=bounds. This is not the quarantined leaf accessor.
EnqueueResult enqueue(void* a, float distance, void* b, void* bounds)
{
    active.fetch_add(1, std::memory_order_acq_rel);
    const bool capture = recording.load(std::memory_order_acquire);
#ifdef GLASS_NODE_LIFETIME
    // Lifetimes must keep tracking when the bounded CSV recording stops.
    const bool observe = true;
#else
    const bool observe = capture;
#endif
    Row row;
    std::array<std::uint64_t, 2> before {}, after {};
    const auto instance = reinterpret_cast<std::uint64_t>(a);
    row.context = instance;
    row.input = reinterpret_cast<std::uint64_t>(b);
    if (observe)
    {
        row.caller = reinterpret_cast<std::uint64_t>(_ReturnAddress());
        row.valid = instance >= 0x10000 && instance <= 0x7fffffffffffULL - 0xf8 &&
                    read(instance + 0x60, &row.header[0], 16) &&
                    read(instance + 0xb8, &row.header[7], 8) &&
                    read(instance + 0xe8, before.data(), 16) &&
                    read(row.input, &row.header[12], 16) &&
                    row.header[0] >= 0x10000 && row.header[0] <= 0x7fffffffffffULL - 0x50 &&
                    read(row.header[0] + 0x38, &row.header[2], 24) &&
                    row.header[2] >= 0x10000 && row.header[2] <= 0x7fffffffffffULL - 0x3c &&
                    read(row.header[2] + 0x30, &row.header[5], 8) &&
                    read(row.header[2] + 0x38, &row.header[6], 4);
        row.header[11] = before[1] >> 32;
    }
    original(a, distance, b, bounds);
    if (observe)
    {
        row.valid = row.valid && read(instance + 0xe8, after.data(), 16);
        row.header[14] = after[1] >> 32;
        row.header[16] = after[0];
        const auto range = GlassFg::GeometrySourceSpan::resolve(
            row.header[5], row.header[6], static_cast<std::uint32_t>(row.header[4]),
            static_cast<std::uint32_t>(row.header[4] >> 32), row.header[12], row.header[13], 48);
        row.header[8] = std::uint64_t(range.first) | (std::uint64_t(range.count) << 32);
        // No append means the renderer rejected the group. Never advance a
        // persistent source ordinal from the compacted destination array.
        row.valid = row.valid && range && row.header[14] == row.header[11] + 1 &&
                    row.header[14] <= static_cast<std::uint32_t>(after[1]) &&
                    after[0] >= 0x10000 && after[0] <= 0x7fffffffffffULL - row.header[11] * 16 - 16 &&
                    read(after[0] + row.header[11] * 16, &row.header[9], 8) &&
                    row.header[9] >= 0x10000 && row.header[9] <= 0x7fffffffffffULL - 24 &&
                    read(row.header[9] + 16, &row.header[10], 8) && row.header[10];
        if (row.valid)
        {
#ifdef GLASS_NODE_LIFETIME
            std::uint64_t renderMesh = 0, proxyMesh = 0;
            const auto mesh = row.header[7], proxy = row.header[10];
            const bool typed = mesh >= 0x10000 && mesh <= 0x7fffffffffffULL - 0x1f8 &&
                proxy >= 0x10000 && proxy <= 0x7fffffffffffULL - 0xe0 &&
                read(mesh + 0x1f0, &renderMesh, 8) && read(proxy + 0xd8, &proxyMesh, 8) &&
                renderMesh && renderMesh == proxyMesh;
            {
                std::lock_guard lock(sourceMutex);
                row.header[15] = typed ? renderMesh : 0;
                row.header[17] = sourceOwners.created(row.header[9], proxy,
                    {instance, row.header[2], row.header[15], range.first, range.count});
            }
            if (row.header[17]) ++sourceCreated; else ++sourceRejected;
#endif
            if (capture)
            {
            const auto index = used.fetch_add(1, std::memory_order_relaxed);
            if (index < rows.size()) { row.thread = GetCurrentThreadId(); rows[index] = row; }
            else recording.store(false, std::memory_order_release);
            }
        }
        else if (capture) malformed.fetch_add(1, std::memory_order_relaxed);
    }
    active.fetch_sub(1, std::memory_order_release);
}
#else
EnqueueResult enqueue(void* a, void* b
#ifdef GLASS_ARRAY_WRAPPER
                      , void* bounds
#endif
)
{
    active.fetch_add(1, std::memory_order_acq_rel);
#ifdef GLASS_TRANSFORM_RANGE
    // The audited leaf writes its borrowed span to b and returns b.
    const auto result = original(a, b);
#endif
    if (recording.load(std::memory_order_acquire))
    {
        Row row;
        row.input = reinterpret_cast<std::uint64_t>(b);
#ifdef GLASS_TRANSFORM_RANGE
        // Original 24-byte range plus resulting span and one data pointer.
        // No transform data is copied; caller owns the object lifetime.
        row.valid = read(reinterpret_cast<std::uint64_t>(a), row.header.data(), 24) &&
                    read(row.input, &row.header[12], 16);
        if (row.valid && row.header[0])
        {
            row.valid = row.header[0] <= 0x7fffffffffffULL - 56 &&
                        read(row.header[0] + 48, &row.header[3], 8);
        }
#elif defined(GLASS_ARRAY_WRAPPER)
        // Normalized diagnostic fields, not a copy of an engine request.
        // Preserve the real upstream return site. Do not dereference the span.
        const auto handle = reinterpret_cast<std::uint64_t>(a);
        row.valid = handle >= 0x10000 && handle <= 0x7fffffffffffULL - 24 &&
                    read(handle + 16, &row.header[0], 8) &&
                    read(row.input, &row.header[12], 16) &&
                    read(reinterpret_cast<std::uint64_t>(bounds), &row.header[8], 32);
#else
        row.valid = read(row.input, row.header.data(), sizeof(row.header));
#endif
        if (!row.valid) row.header = {};
        const auto begin = row.header[12], end = row.header[13];
        const bool validSpan = row.valid && begin >= 0x10000 && end > begin &&
                               end <= 0x7fffffffffffULL && (end - begin) % 48 == 0;
        if (arraysOnly.load(std::memory_order_relaxed) && !validSpan)
        {
            if (!row.valid || begin != end) malformed.fetch_add(1, std::memory_order_relaxed);
            else filtered.fetch_add(1, std::memory_order_relaxed);
        }
        else
        {
            const auto index = used.fetch_add(1, std::memory_order_relaxed);
            if (index < rows.size())
            {
                row.caller = reinterpret_cast<std::uint64_t>(_ReturnAddress());
                row.context = reinterpret_cast<std::uint64_t>(a);
                row.thread = GetCurrentThreadId();
                rows[index] = row;
            }
            else recording.store(false, std::memory_order_release);
        }
    }
#ifndef GLASS_TRANSFORM_RANGE
    const auto result = original(a, b
#ifdef GLASS_ARRAY_WRAPPER
                                 , bounds
#endif
    );
#endif
    active.fetch_sub(1, std::memory_order_release);
    return result;
}
#endif
bool install()
{
#ifdef GLASS_TRANSFORM_RANGE
    // Quarantined: audited callers keep R10 live across the original leaf.
    // An ordinary C++ detour does not preserve this internal calling contract.
    // Keep the decoder fixture available, but never install this adapter.
    return false;
#else
    if (original) return true;
    wchar_t path[32768];
    const auto length = GetModuleFileNameW(self, path, 32768);
    if (!length || length >= 32768) return false;
    auto profile = std::filesystem::path(path); profile.replace_extension(L".profile");
    std::ifstream file(profile, std::ios::binary);
    std::array<unsigned, 3> header {};
    if (!file.read(reinterpret_cast<char*>(header.data()), sizeof(header)) ||
        header[0] != profileMagic || !header[2] || header[2] > 16384) return false;
    const auto base = reinterpret_cast<std::uint64_t>(GetModuleHandleW(nullptr));
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return false;
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
    const auto size = nt->OptionalHeader.SizeOfImage;
    if (header[1] >= size || header[2] > size - header[1]) return false;
    std::vector<char> expected(header[2]), actual(header[2]);
    if (!file.read(expected.data(), header[2]) ||
        !read(base + header[1], actual.data(), header[2]) || expected != actual) return false;
#ifdef GLASS_NODE_LIFETIME
    std::array<unsigned, 2> destruction {};
    if (!file.read(reinterpret_cast<char*>(destruction.data()), sizeof(destruction)) ||
        !destruction[1] || destruction[1] > 16384 || destruction[0] >= size ||
        destruction[1] > size - destruction[0]) return false;
    std::vector<char> expectedDestroy(destruction[1]), actualDestroy(destruction[1]);
    if (!file.read(expectedDestroy.data(), destruction[1]) ||
        !read(base + destruction[0], actualDestroy.data(), destruction[1]) ||
        expectedDestroy != actualDestroy) return false;
#endif
    if (file.peek() != std::char_traits<char>::eof()) return false;
    HMODULE pinned = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
                           reinterpret_cast<LPCWSTR>(&install), &pinned)) return false;
    GlassFg::DetourThreads threads;
    if (!threads.gather() || DetourTransactionBegin() != NO_ERROR) return false;
    auto target = reinterpret_cast<Enqueue>(base + header[1]);
    original = target;
    bool attached = DetourAttach(reinterpret_cast<PVOID*>(&original), enqueue) == NO_ERROR;
#ifdef GLASS_NODE_LIFETIME
    originalDestroy = reinterpret_cast<Destroy>(base + destruction[0]);
    attached = DetourAttach(reinterpret_cast<PVOID*>(&originalDestroy), destroy) == NO_ERROR && attached;
#endif
    if (!attached || !threads.enlist())
    { DetourTransactionAbort(); original = nullptr; return false; }
    if (DetourTransactionCommit() != NO_ERROR) { original = nullptr; return false; }
#ifdef GLASS_NODE_LIFETIME
    sourceReady.store(true, std::memory_order_release);
#endif
    return true;
#endif
}
}
#ifdef GLASS_NODE_LIFETIME
extern "C" __declspec(dllexport) std::int32_t GlassSourceOwnerQuery(
    std::uint64_t proxy, std::uint64_t mesh, std::uint32_t originalCount,
    GlassExperimentSourceOwner* result)
{
    if (!result || result->size != sizeof(*result) || result->version != 1) return -1;
    *result = {};
    if (!sourceReady.load(std::memory_order_acquire) || !sourceHealthy.load(std::memory_order_acquire)) return 0;
    try
    {
        std::lock_guard lock(sourceMutex);
        const auto owner = sourceOwners.find(proxy, mesh);
        if (!sourceHealthy.load(std::memory_order_acquire) || !owner || !originalCount ||
            owner.source.count != originalCount) return 0;
        result->node = owner.source.node; result->buffer = owner.source.buffer;
        result->first = owner.source.first; result->count = owner.source.count;
        result->generation = owner.generation;
        return 1;
    }
    catch (...)
    {
        sourceHealthy.store(false, std::memory_order_release);
        return 0;
    }
}
#endif
static DWORD start(void* directory, bool onlyArrays)
{
    try
    {
        std::lock_guard lock(control);
        if (!directory || recording || active) return 1;
        const std::filesystem::path next(static_cast<const wchar_t*>(directory));
        if (!next.is_absolute() || std::filesystem::exists(next) || !install()) return 2;
        if (!std::filesystem::create_directory(next)) return 3;
        output = next; used = 0; filtered = 0; malformed = 0; arraysOnly = onlyArrays;
        recording.store(true, std::memory_order_release); return 0;
    }
    catch (...) { return 4; }
}
extern "C" __declspec(dllexport) DWORD WINAPI GlassInstanceStart(void* directory)
{ return start(directory, false); }
extern "C" __declspec(dllexport) DWORD WINAPI GlassInstanceStartArrays48(void* directory)
{ return start(directory, true); }
extern "C" __declspec(dllexport) DWORD WINAPI GlassInstanceSave(void*)
{
    try
    {
        std::lock_guard lock(control);
        recording.store(false, std::memory_order_release);
        const auto deadline = GetTickCount64() + 10000;
        while (active.load(std::memory_order_acquire) && GetTickCount64() < deadline) Sleep(1);
        if (active || output.empty()) return 1;
        const auto count = (std::min)(used.load(), unsigned(rows.size()));
        std::ofstream file(output / "updates.csv");
        file << "caller,context,input,thread,valid";
        for (unsigned i = 0; i < 18; ++i) file << ",q" << i;
        file << '\n';
        for (unsigned i = 0; i < count; ++i)
        {
            const auto& r = rows[i];
            file << r.caller << ',' << r.context << ',' << r.input << ',' << r.thread << ',' << r.valid;
            for (auto q : r.header) file << ',' << q;
            file << '\n';
        }
        file.close(); if (!file) return 2;
        std::ofstream(output / "status.txt") << "rows=" << count << "\nrecording=0\nresident=1\n"
            << "arrays48_only=" << arraysOnly.load() << "\nfiltered_empty=" << filtered.load()
            << "\nmalformed=" << malformed.load() << '\n'
            << "profile_magic=" << profileMagic << '\n';
#ifdef GLASS_NODE_LIFETIME
        {
            std::lock_guard sourceLock(sourceMutex);
            std::ofstream(output / "owners.txt") << "created=" << sourceCreated.load()
                << "\ndestroy_callbacks=" << sourceDestroyed.load() << "\nrejected=" << sourceRejected.load()
                << "\nlive=" << sourceOwners.size() << "\ntracking_continues=1\nobject_motion_produced=0\n";
            std::ofstream(output / "source-query.txt") << "ready=" << sourceReady.load()
                << "\nhealthy=" << sourceHealthy.load() << "\nabi_version=1\n";
        }
#endif
        output.clear(); return 0;
    }
    catch (...) { return 3; }
}
BOOL WINAPI DllMain(HINSTANCE module, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH) self = module;
    return TRUE;
}
