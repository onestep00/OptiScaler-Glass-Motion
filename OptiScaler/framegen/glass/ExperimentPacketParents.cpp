// Standalone CPU diagnostic. Same-flush scalar provenance, no history or GPU work.
#include "ExperimentSourceAbi.h"
#include "DetourThreads.h"
#include <array>
#include <atomic>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <vector>

namespace
{
using Append = void (*)(void*, void*, uintptr_t, uintptr_t, void*);
using Rigid = void (*)(void*, void*, void*, uint32_t, bool);
using Skinned = void (*)(void*, void*, void*, bool);
Append originalAppend = nullptr;
Rigid originalRigid = nullptr;
Skinned originalSkinned = nullptr;
HMODULE self = nullptr;
uint64_t drawReturn = 0, rendererGlobal = 0;
std::atomic<bool> enabled = false;
std::atomic<unsigned> active = 0, epoch = 1;
std::atomic<uint64_t> appends = 0, parents = 0, flushes = 0, queries = 0, matched = 0, rejected = 0;
std::mutex control;
std::filesystem::path output;
struct Context
{
    uint64_t rigid; uint32_t rigidCapacity, rigidCount;
    uint64_t skinned; uint32_t skinCapacity, skinCount;
    std::array<std::byte, 0x28> unused;
    uint64_t geometry, entry;
};
static_assert(sizeof(Context) == 0x58);
struct Geometry
{
    uint8_t kind; std::array<std::byte, 7> unused0;
    uint64_t mesh; std::array<std::byte, 0x10> unused1;
    uint32_t indices, unused2;
    uint8_t flags, unused3; uint16_t chunk;
};
struct Spans
{
    std::array<GlassExperimentPacketParent, 2048> data;
    uint32_t used = 0, count = 0;
    uint64_t revision = 0;
    bool valid = true;
    void clear() { used = count = 0; valid = ++revision != 0; }
    void append(uint32_t before, uint32_t after, GlassExperimentPacketParent row)
    {
        if (!valid || !row.count || row.count > 32767 || before != count ||
            uint64_t(before) + row.count != after || used == data.size()) { valid = false; return; }
        if (++revision == 0) { valid = false; return; }
        row.first = before; data[used++] = row; count = after;
    }
};
struct Batch { uint64_t context = 0; unsigned epoch = 0; Spans rigid, skinned; };
thread_local Batch batch;
struct Flush { const Spans* spans; Geometry geometry; uint32_t count; unsigned epoch; uint64_t context, revision; };
thread_local const Flush* current = nullptr;
template<class T> bool read(uint64_t address, T& value) noexcept
{
    __try
    {
        if (address < 0x10000 || address > 0x7fffffffffffULL - sizeof(T)) return false;
        memcpy(&value, reinterpret_cast<const void*>(address), sizeof(T)); return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
struct Activity { Activity() { ++active; } ~Activity() { --active; } };
void append(void* transforms, void* packet, uintptr_t c, uintptr_t d, void* context)
{
    if (!enabled.load(std::memory_order_acquire)) { originalAppend(transforms, packet, c, d, context); return; }
    Activity activity;
    Context before {}, after {}; Geometry geometry {};
    std::array<uint64_t, 2> words {};
    const auto address = reinterpret_cast<uint64_t>(context);
    if (batch.epoch != epoch || batch.context != address)
    { batch.epoch = epoch; batch.context = address; batch.rigid.clear(); batch.skinned.clear(); }
    const auto capturedEpoch = batch.epoch;
    bool valid = read(address, before) && read(reinterpret_cast<uint64_t>(packet), words);
    const bool skin = (words[1] & (1ULL << 50)) != 0;
    auto& spans = skin ? batch.skinned : batch.rigid;
    const auto beforeCount = skin ? before.skinCount : before.rigidCount;
    if (!beforeCount) spans.clear();
    GlassExperimentPacketParent row;
    row.count = static_cast<uint32_t>((words[1] >> 18) & 0x7fff);
    row.transformIndex = static_cast<uint32_t>((words[1] >> 33) & 0x1ffff);
    row.globalRange = static_cast<uint32_t>((words[0] >> 59) & 1);
    const auto slot = static_cast<uint32_t>(words[1] & 0x3ffff);
    uint64_t root = 0, renderer = 0, encoded = 0, mesh = 0;
    uint32_t actualSlot = UINT32_MAX;
    struct Array { uint64_t source; uint32_t count, global; } array {};
    if (valid && (words[1] & (1ULL << 51)) && slot < 131072 &&
        read(rendererGlobal, root) && read(root + 0x4628, renderer) && renderer &&
        before.entry == renderer + 0x274248 + uint64_t(slot) * 24 &&
        read(before.entry, encoded) && read(before.geometry, geometry) && geometry.kind == 0)
    {
        const auto proxy = encoded & 0x00ffffffffffffffULL;
        if (read(proxy + 0x98, actualSlot) && actualSlot == slot && read(proxy + 0xd8, mesh) &&
            mesh && mesh == geometry.mesh && read(proxy + 0x108, array))
        {
            row.proxy = proxy; row.mesh = mesh; row.entry = before.entry; row.slot = slot;
            row.sourceArray = array.source; row.arrayCount = array.count; row.arrayGlobalStart = array.global;
            ++parents;
        }
    }
    originalAppend(transforms, packet, c, d, context);
    ++appends;
    valid = valid && batch.context == address && batch.epoch == capturedEpoch && read(address, after) &&
        before.entry == after.entry && before.geometry == after.geometry;
    if (!valid) { batch.rigid.valid = batch.skinned.valid = false; ++rejected; return; }
    spans.append(beforeCount, skin ? after.skinCount : after.rigidCount, row);
}
struct FlushScope
{
    const Flush* previous = current;
    Flush value {};
    Spans* spans = nullptr;
    FlushScope(void* geometry, void* context, bool skin)
    {
        current = nullptr;
        Context data {};
        if (!enabled || batch.epoch != epoch || batch.context != reinterpret_cast<uint64_t>(context) ||
            !read(batch.context, data) || data.geometry != reinterpret_cast<uint64_t>(geometry) ||
            !read(data.geometry, value.geometry) || value.geometry.kind != 0) return;
        spans = skin ? &batch.skinned : &batch.rigid;
        value.count = skin ? data.skinCount : data.rigidCount;
        if (!spans->valid || !value.count || spans->count != value.count) return;
        value.spans = spans; value.epoch = batch.epoch; value.context = batch.context;
        value.revision = spans->revision; current = &value; ++flushes;
    }
    ~FlushScope() { if (spans) spans->clear(); current = previous; }
};
void rigid(void* a, void* b, void* c, uint32_t global, bool half)
{
    if (!enabled.load(std::memory_order_acquire)) { originalRigid(a, b, c, global, half); return; }
    Activity activity; FlushScope scope(b, c, false); originalRigid(a, b, c, global, half);
}
void skinned(void* a, void* b, void* c, bool half)
{
    if (!enabled.load(std::memory_order_acquire)) { originalSkinned(a, b, c, half); return; }
    Activity activity; FlushScope scope(b, c, true); originalSkinned(a, b, c, half);
}
bool install()
{
    if (originalAppend) return true;
    wchar_t name[32768]; if (!GetModuleFileNameW(self, name, 32768)) return false;
    auto profile = std::filesystem::path(name); profile.replace_extension(L".profile");
    std::ifstream file(profile, std::ios::binary);
    std::array<uint64_t, 5> header {};
    const auto image = reinterpret_cast<uint64_t>(GetModuleHandleW(nullptr));
    if (!file.read(reinterpret_cast<char*>(header.data()), sizeof(header)) || header[0] != 0x50504b31 ||
        header[1] != GetCurrentProcessId() || header[2] != image || !header[3] || !header[4]) return false;
    std::array<uint64_t, 3> functions {};
    for (auto& function : functions)
    {
        std::array<uint32_t, 2> entry {};
        if (!file.read(reinterpret_cast<char*>(entry.data()), sizeof(entry)) || !entry[1] || entry[1] > 4096) return false;
        std::vector<char> expected(entry[1]);
        if (!file.read(expected.data(), entry[1])) return false;
        function = image + entry[0];
        for (unsigned i = 0; i < entry[1]; ++i)
        { char actual = 0; if (!read(function + i, actual) || actual != expected[i]) return false; }
    }
    if (file.peek() != std::char_traits<char>::eof()) return false;
    HMODULE pinned = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
        reinterpret_cast<LPCWSTR>(&install), &pinned)) return false;
    GlassFg::DetourThreads threads;
    if (!threads.gather() || DetourTransactionBegin() != NO_ERROR) return false;
    originalAppend = reinterpret_cast<Append>(functions[0]);
    originalRigid = reinterpret_cast<Rigid>(functions[1]); originalSkinned = reinterpret_cast<Skinned>(functions[2]);
    if (DetourAttach(reinterpret_cast<PVOID*>(&originalAppend), append) != NO_ERROR ||
        DetourAttach(reinterpret_cast<PVOID*>(&originalRigid), rigid) != NO_ERROR ||
        DetourAttach(reinterpret_cast<PVOID*>(&originalSkinned), skinned) != NO_ERROR || !threads.enlist())
    { DetourTransactionAbort(); originalAppend = nullptr; return false; }
    if (DetourTransactionCommit() != NO_ERROR) { originalAppend = nullptr; return false; }
    drawReturn = image + header[3]; rendererGlobal = image + header[4]; return true;
}
}
extern "C" __declspec(dllexport) int32_t GlassPacketParentQuery(uint64_t callsite, uint64_t mesh,
    uint32_t chunk, uint32_t instances, uint32_t ordinal, uint32_t first, uint32_t count,
    uint32_t transformIndex, uint32_t globalRange, GlassExperimentPacketParent* result)
{
    if (!result || result->size != sizeof(*result) || result->version != 1) return -1;
    *result = {}; ++queries;
    if (!enabled || !current || current->epoch != epoch || current->epoch != batch.epoch ||
        current->context != batch.context || current->revision != current->spans->revision ||
        callsite != drawReturn ||
        current->geometry.mesh != mesh || current->geometry.chunk != chunk || current->count != instances ||
        ordinal >= current->spans->used) return 0;
    const auto& source = current->spans->data[ordinal];
    if (!source.proxy || source.mesh != mesh || source.first != first || source.count != count ||
        source.transformIndex != transformIndex || source.globalRange != globalRange) return 0;
    *result = source; ++matched; return 1;
}
extern "C" __declspec(dllexport) DWORD WINAPI GlassInstanceStart(void* directory)
{
    try
    {
        std::lock_guard lock(control);
        if (enabled || !directory) return 1;
        const std::filesystem::path next(static_cast<const wchar_t*>(directory));
        if (!next.is_absolute() || std::filesystem::exists(next) || !install()) return 2;
        if (!std::filesystem::create_directory(next)) return 3;
        output = next; ++epoch; appends = parents = flushes = queries = matched = rejected = 0;
        enabled.store(true, std::memory_order_release); return 0;
    }
    catch (...) { return 4; }
}
extern "C" __declspec(dllexport) DWORD WINAPI GlassInstanceSave(void*)
{
    try
    {
        std::lock_guard lock(control); enabled.store(false, std::memory_order_release);
        const auto deadline = GetTickCount64() + 10000;
        while (active && GetTickCount64() < deadline) Sleep(1);
        if (active || output.empty()) return 1;
        std::ofstream file(output / "packet-parents.txt");
        file << "appends=" << appends << "\nparents=" << parents << "\nflushes=" << flushes
             << "\nqueries=" << queries << "\nmatched=" << matched << "\nrejected=" << rejected
             << "\nrecording=0\ngpu_copies=0\nmotion_produced=0\n";
        file.close(); if (!file) return 2; output.clear(); return 0;
    }
    catch (...) { return 3; }
}
BOOL WINAPI DllMain(HINSTANCE module, DWORD reason, LPVOID)
{ if (reason == DLL_PROCESS_ATTACH) self = module; return TRUE; }
