// Standalone CPU diagnostic; not linked into OptiScaler. Stop leaves the
// forwarding hook pinned until process exit. No transform arrays/GPU copies.
#include "DetourThreads.h"
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <vector>
#include <intrin.h>

namespace
{
#ifdef GLASS_ARRAY_WRAPPER
using Enqueue = unsigned char (*)(void*, void*, void*);
constexpr unsigned profileMagic = 0x49555032;
#else
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
unsigned char enqueue(void* a, void* b
#ifdef GLASS_ARRAY_WRAPPER
                      , void* bounds
#endif
)
{
    active.fetch_add(1, std::memory_order_acq_rel);
    if (recording.load(std::memory_order_acquire))
    {
        Row row;
        row.input = reinterpret_cast<std::uint64_t>(b);
#ifdef GLASS_ARRAY_WRAPPER
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
    const auto result = original(a, b
#ifdef GLASS_ARRAY_WRAPPER
                                 , bounds
#endif
    );
    active.fetch_sub(1, std::memory_order_release);
    return result;
}
bool install()
{
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
    if (!file.read(expected.data(), header[2]) || file.peek() != std::char_traits<char>::eof() ||
        !read(base + header[1], actual.data(), header[2]) || expected != actual) return false;
    HMODULE pinned = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
                           reinterpret_cast<LPCWSTR>(&install), &pinned)) return false;
    GlassFg::DetourThreads threads;
    if (!threads.gather() || DetourTransactionBegin() != NO_ERROR) return false;
    auto target = reinterpret_cast<Enqueue>(base + header[1]);
    original = target;
    if (DetourAttach(reinterpret_cast<PVOID*>(&original), enqueue) != NO_ERROR || !threads.enlist())
    { DetourTransactionAbort(); original = nullptr; return false; }
    if (DetourTransactionCommit() != NO_ERROR) { original = nullptr; return false; }
    return true;
}
}
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
        output.clear(); return 0;
    }
    catch (...) { return 3; }
}
BOOL WINAPI DllMain(HINSTANCE module, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH) self = module;
    return TRUE;
}
