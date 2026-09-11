// Standalone, process-resident CPU diagnostic. Never linked into OptiScaler.
// Exports run outside DllMain. Stop disables recording; hooks/DLL stay resident.
#include "CyberpunkInstanceSelection.h"
#include "DetourThreads.h"
#include <atomic>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <vector>
#include <intrin.h>

namespace
{
using Selection = GlassFg::CyberpunkInstanceSelection;
using Outer = unsigned char (*)(void*, void*, void*, std::uint64_t);
using Select = void (*)(void*, void*, unsigned, void*, void*);
using Packet = unsigned char (*)(void*, void*, void*, std::uint64_t, void*, void*, void*);
Outer originalOuter = nullptr;
Select originalSelect = nullptr;
Packet originalPacket = nullptr;
HMODULE self = nullptr;
std::uint64_t image = 0, groupReturn = 0, packetReturn = 0, linearReturn = 0;
const volatile unsigned* tick = nullptr;
std::atomic<bool> enabled = false;
std::atomic<unsigned> active = 0, used = 0, dropped = 0;
bool installed = false;
std::mutex control;
std::filesystem::path output;
struct Scope
{
    std::uint64_t proxy = 0, group = 0;
    unsigned frame = 0;
    std::uint64_t outerContext = 0, producerContext = 0;
};
thread_local Scope* current = nullptr;
struct Row
{
    std::uint64_t proxy = 0, mesh = 0, group = 0, sourceArray = 0, sourceIndices = 0;
    Selection::Descriptor descriptor {};
    unsigned frame = 0, ownerSlot = 0, originalCount = 0, valid = 0;
    // Raw producer provenance, not an admitted view or submission identity.
    std::uint64_t outerContext = 0, producerContext = 0;
    std::array<std::uint64_t, 3> producerHeader {};
    bool producerHeaderValid = false;
    bool linear = false;
    std::array<unsigned, 64> indices {};
};
std::array<Row, 4096> rows;
bool read(std::uint64_t address, void* out, unsigned bytes) noexcept
{
    __try
    {
        if (address < 0x10000 || address > 0x7fffffffffffull - bytes) return false;
        memcpy(out, reinterpret_cast<const void*>(address), bytes); return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
unsigned char outer(void* a, void* b, void* c, std::uint64_t d)
{
    if (!enabled.load(std::memory_order_acquire))
    {
        auto* previous = current; current = nullptr;
        const auto result = originalOuter(a, b, c, d);
        current = previous; return result;
    }
    active.fetch_add(1, std::memory_order_acq_rel);
    Scope local { reinterpret_cast<std::uint64_t>(a), 0, *tick };
    local.outerContext = reinterpret_cast<std::uint64_t>(b);
    local.producerContext = reinterpret_cast<std::uint64_t>(c);
    auto* previous = current;
    current = enabled.load(std::memory_order_acquire) ? &local : nullptr;
    const auto result = originalOuter(a, b, c, d);
    current = previous;
    active.fetch_sub(1, std::memory_order_release);
    return result;
}
void observeGroup(void* a, std::uint64_t caller)
{
    // This audited call site passes currentGroup + 0x10. No hash-table walk.
    if (current && caller == groupReturn)
        current->group = reinterpret_cast<std::uint64_t>(a) - 0x10;
}
void select(void* a, void* b, unsigned c, void* d, void* e)
{
    observeGroup(a, reinterpret_cast<std::uint64_t>(_ReturnAddress()));
    originalSelect(a, b, c, d, e);
}
void observe(void* owner, void* descriptor, bool linear = false)
{
    if (!current || current->proxy != reinterpret_cast<std::uint64_t>(owner) ||
        (!linear && !current->group) || current->frame != *tick) return;
    Row row;
    row.proxy = current->proxy; row.group = linear ? 0 : current->group; row.frame = current->frame;
    row.linear = linear;
    row.outerContext = current->outerContext; row.producerContext = current->producerContext;
    // Three scalar fields: renderer, destination family and scene context in
    // the audited callers. Do not retain a borrowed stack header for later use.
    if (row.producerContext)
        row.producerHeaderValid = read(row.producerContext, row.producerHeader.data(), sizeof(row.producerHeader));
    if (!row.producerHeaderValid) row.producerHeader = {};
    current->group = 0; // One producer call consumes one explicitly observed group.
    if (!read(reinterpret_cast<std::uint64_t>(descriptor), &row.descriptor, sizeof(row.descriptor)) ||
        !row.descriptor.count || row.descriptor.count > row.indices.size() ||
        !read(row.proxy + 0x98, &row.ownerSlot, 4) || !read(row.proxy + 0xd8, &row.mesh, 8) ||
        !read(row.proxy + 0x108, &row.sourceArray, 8) || !read(row.proxy + 0x110, &row.originalCount, 4)) return;
    Selection selection;
    if (linear)
    {
        unsigned globalStart = UINT32_MAX;
        if (!read(row.proxy + 0x114, &globalStart, 4) ||
            !selection.resolveLinear(row.descriptor, row.originalCount, globalStart)) return;
    }
    else if (!selection.resolve(row.group, row.descriptor, row.originalCount, read)) return;
    row.sourceIndices = selection.sourceIndices;
    row.valid = 1;
    for (unsigned i = 0; i < row.descriptor.count; ++i)
        if (!selection.originalIndex(i, row.indices[i], read)) { row.valid = 0; break; }
    const auto index = used.fetch_add(1, std::memory_order_relaxed);
    if (index < rows.size()) rows[index] = row;
    else { ++dropped; enabled.store(false, std::memory_order_release); }
}
unsigned char packet(void* a, void* b, void* c, std::uint64_t d, void* e, void* f, void* g)
{
    const auto caller = reinterpret_cast<std::uint64_t>(_ReturnAddress());
    if (caller == packetReturn) observe(a, g);
    else if (linearReturn && caller == linearReturn) observe(a, g, true);
    return originalPacket(a, b, c, d, e, f, g);
}
bool install()
{
    if (installed) return true;
    wchar_t path[32768];
    const auto length = GetModuleFileNameW(self, path, 32768);
    if (!length || length >= 32768) return false;
    auto profile = std::filesystem::path(path); profile.replace_extension(L".profile");
    std::ifstream file(profile, std::ios::binary);
    // Diagnostic profile stores the three complete audited function bodies.
    // Absolute addresses never cross processes; unknown live bytes reject install.
    std::array<unsigned, 7> header {};
    if (!file.read(reinterpret_cast<char*>(header.data()), sizeof(header)) ||
        (header[0] != 0x49535031 && header[0] != 0x49535032))
        return false;
    image = reinterpret_cast<std::uint64_t>(GetModuleHandleW(nullptr));
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(image);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return false;
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(image + dos->e_lfanew);
    const auto imageBytes = nt->OptionalHeader.SizeOfImage;
    if (header[4] > imageBytes - 4 || header[5] >= imageBytes || header[6] >= imageBytes) return false;
    unsigned linearRva = 0;
    if (header[0] == 0x49535032 &&
        (!file.read(reinterpret_cast<char*>(&linearRva), 4) || !linearRva || linearRva >= imageBytes ||
         linearRva == header[6])) return false;
    for (unsigned i = 0; i < 3; ++i)
    {
        unsigned bytes = 0;
        if (!file.read(reinterpret_cast<char*>(&bytes), 4) || !bytes || bytes > 16384 ||
            header[i + 1] >= imageBytes || bytes > imageBytes - header[i + 1]) return false;
        std::vector<char> expected(bytes), actual(bytes);
        if (!file.read(expected.data(), bytes) || !read(image + header[i + 1], actual.data(), bytes) ||
            expected != actual) return false;
    }
    if (file.peek() != std::char_traits<char>::eof()) return false;
    HMODULE pinned = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
                           reinterpret_cast<LPCWSTR>(&install), &pinned)) return false;
    originalOuter = reinterpret_cast<Outer>(image + header[1]);
    originalSelect = reinterpret_cast<Select>(image + header[2]);
    originalPacket = reinterpret_cast<Packet>(image + header[3]);
    tick = reinterpret_cast<const volatile unsigned*>(image + header[4]);
    groupReturn = image + header[5]; packetReturn = image + header[6];
    linearReturn = linearRva ? image + linearRva : 0;
    GlassFg::DetourThreads threads;
    if (!threads.gather() || DetourTransactionBegin() != NO_ERROR) return false;
    if (DetourAttach(reinterpret_cast<PVOID*>(&originalOuter), outer) != NO_ERROR ||
        DetourAttach(reinterpret_cast<PVOID*>(&originalSelect), select) != NO_ERROR ||
        DetourAttach(reinterpret_cast<PVOID*>(&originalPacket), packet) != NO_ERROR || !threads.enlist())
    { DetourTransactionAbort(); return false; }
    installed = DetourTransactionCommit() == NO_ERROR;
    return installed;
}
}
extern "C" __declspec(dllexport) DWORD WINAPI GlassInstanceStart(void* directory)
{
    try
    {
        std::lock_guard lock(control);
        if (enabled || active || !directory) return 1;
        const std::filesystem::path next(static_cast<const wchar_t*>(directory));
        if (!next.is_absolute() || std::filesystem::exists(next) || !install()) return 2;
        if (!std::filesystem::create_directory(next)) return 3;
        output = next; used = dropped = 0;
        enabled.store(true, std::memory_order_release); return 0;
    }
    catch (...) { return 4; }
}
extern "C" __declspec(dllexport) DWORD WINAPI GlassInstanceSave(void*)
{
    try
    {
        std::lock_guard lock(control);
        enabled.store(false, std::memory_order_release);
        const auto deadline = GetTickCount64() + 10000;
        while (active.load(std::memory_order_acquire) && GetTickCount64() < deadline) Sleep(1);
        if (active || output.empty()) return 1;
        std::ofstream file(output / "source-indices.csv");
        file << "frame,proxy,mesh,owner_slot,group,source_array,source_indices,original_count,global_start,first,count,valid,ordinal,source_index,outer_context,producer_context,producer_0,producer_8,producer_16,producer_header_valid,linear_source\n";
        const auto count = (std::min)(used.load(), unsigned(rows.size()));
        for (unsigned i = 0; i < count; ++i)
        {
            const auto& row = rows[i];
            for (unsigned j = 0; j < row.descriptor.count; ++j)
                file << row.frame << ',' << row.proxy << ',' << row.mesh << ',' << row.ownerSlot << ',' << row.group << ','
                     << row.sourceArray << ',' << row.sourceIndices << ',' << row.originalCount << ','
                     << row.descriptor.globalStart << ',' << row.descriptor.first << ',' << row.descriptor.count << ','
                     << row.valid << ',' << j << ',' << row.indices[j] << ',' << row.outerContext << ','
                     << row.producerContext << ',' << row.producerHeader[0] << ',' << row.producerHeader[1] << ','
                     << row.producerHeader[2] << ',' << row.producerHeaderValid << ',' << row.linear << '\n';
        }
        file.close(); if (!file) return 2;
        std::ofstream(output / "status.txt") << "rows=" << count << "\ndropped=" << dropped.load()
            << "\nresident=1\nrecording=0\ngpu_copies=0\nlifetime_proven=0\nmotion_produced=0\n";
        output.clear(); return 0;
    }
    catch (...) { return 3; }
}
BOOL WINAPI DllMain(HINSTANCE module, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH) self = module;
    return TRUE;
}
