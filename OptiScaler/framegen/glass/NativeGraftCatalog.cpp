#include "pch.h"
#include "NativeGraftCatalog.h"
#include <Util.h>
#include <bcrypt.h>
#include <algorithm>
#include <atomic>
#include <cstring>
#include <fstream>
#include <mutex>
#include <vector>

#pragma comment(lib, "bcrypt.lib")

namespace GlassFg
{
namespace
{
// index.bin: "GGRAFT01", u32 count, u32 reserved (0), then count records of
// { u8 sha256[32]; u32 currentOutput; u32 previousOutput; u32 supplyClass; }
// (little endian). supplyClass: bit 0 root-only, bit 1 skinning, bit 2 preskinned.
constexpr char IndexMagic[8] = { 'G', 'G', 'R', 'A', 'F', 'T', '0', '1' };
constexpr std::size_t IndexHeaderBytes = 16, IndexRecordBytes = 44;
// The exported catalog holds 240 grafts; the bound keeps a corrupt count from
// sizing an allocation. Shader bytes follow the compiler's own 2 MiB limit.
constexpr std::uint32_t MaxGrafts = 4096;
constexpr std::uintmax_t MaxGraftBytes = 2 * 1024 * 1024;

struct Graft
{
    std::array<std::uint8_t, 32> sha {};
    unsigned currentOutput = 0, previousOutput = 0, supplyClass = 0;
    // Written once under loadMutex; never resized afterwards, so a returned
    // pointer stays valid for the process lifetime.
    std::vector<std::byte> bytes;
    bool attempted = false;
};

struct Catalog
{
    std::once_flag indexOnce;
    std::mutex loadMutex;
    std::vector<Graft> grafts; // Sorted by sha; immutable after indexOnce.
    BCRYPT_ALG_HANDLE sha256 = nullptr;
    std::filesystem::path directory;
    std::atomic<std::size_t> count { 0 };
};
Catalog catalog;

void loadIndex() noexcept
{
    try
    {
        if (!BCRYPT_SUCCESS(BCryptOpenAlgorithmProvider(&catalog.sha256, BCRYPT_SHA256_ALGORITHM, nullptr, 0)))
        {
            catalog.sha256 = nullptr;
            return;
        }
        catalog.directory = Util::DllPath().parent_path() / L"Glass" / L"grafts";
        std::ifstream file(catalog.directory / L"index.bin", std::ios::binary);
        if (!file)
            return;
        char header[IndexHeaderBytes] {};
        if (!file.read(header, sizeof(header)) || std::memcmp(header, IndexMagic, sizeof(IndexMagic)) != 0)
            return;
        std::uint32_t count = 0, reserved = 0;
        std::memcpy(&count, header + 8, 4);
        std::memcpy(&reserved, header + 12, 4);
        if (!count || count > MaxGrafts || reserved)
            return;
        std::vector<Graft> grafts(count);
        for (auto& graft : grafts)
        {
            unsigned char record[IndexRecordBytes] {};
            if (!file.read(reinterpret_cast<char*>(record), sizeof(record)))
                return;
            std::memcpy(graft.sha.data(), record, 32);
            std::uint32_t current = 0, previous = 0, supplyClass = 0;
            std::memcpy(&current, record + 32, 4);
            std::memcpy(&previous, record + 36, 4);
            std::memcpy(&supplyClass, record + 40, 4);
            if (current == previous || current >= 32 || previous >= 32 || supplyClass > 7)
                return;
            graft.currentOutput = current;
            graft.previousOutput = previous;
            graft.supplyClass = supplyClass;
        }
        // Exact size: trailing bytes mean a different format.
        if (file.peek() != std::char_traits<char>::eof())
            return;
        std::sort(grafts.begin(), grafts.end(), [](const Graft& a, const Graft& b) { return a.sha < b.sha; });
        if (std::adjacent_find(grafts.begin(), grafts.end(), [](const Graft& a, const Graft& b) {
                return a.sha == b.sha;
            }) != grafts.end())
            return;
        catalog.grafts = std::move(grafts);
        catalog.count.store(catalog.grafts.size(), std::memory_order_release);
    }
    catch (...)
    {
        catalog.grafts.clear();
        catalog.count.store(0, std::memory_order_release);
    }
}

std::wstring hex(const std::array<std::uint8_t, 32>& sha)
{
    static constexpr wchar_t digits[] = L"0123456789abcdef";
    std::wstring text(64, L'0');
    for (std::size_t i = 0; i < sha.size(); ++i)
    {
        text[i * 2] = digits[sha[i] >> 4];
        text[i * 2 + 1] = digits[sha[i] & 15];
    }
    return text;
}

// First hit only; a failed read is remembered so the file is not retried.
bool loadBytes(Graft& graft) noexcept
{
    std::lock_guard lock(catalog.loadMutex);
    if (graft.attempted)
        return !graft.bytes.empty();
    graft.attempted = true;
    try
    {
        const auto path = catalog.directory / (hex(graft.sha) + L".dxil");
        std::error_code error;
        const auto size = std::filesystem::file_size(path, error);
        if (error || size < 32 || size > MaxGraftBytes)
            return false;
        std::vector<std::byte> bytes(static_cast<std::size_t>(size));
        std::ifstream file(path, std::ios::binary);
        if (!file || !file.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(size)) ||
            std::memcmp(bytes.data(), "DXBC", 4) != 0)
            return false;
        graft.bytes = std::move(bytes);
        return true;
    }
    catch (...)
    {
        graft.bytes.clear();
        return false;
    }
}
} // namespace

std::optional<NativeGraft> FindNativeGraft(const void* vertexShader, std::size_t size,
                                           std::array<std::uint8_t, 32>* hash) noexcept
{
    std::array<std::uint8_t, 32> digest {};
    if (hash)
        *hash = digest;
    std::call_once(catalog.indexOnce, loadIndex);
    if (!vertexShader || !size || size > ULONG_MAX || !catalog.sha256)
        return std::nullopt;
    if (!BCRYPT_SUCCESS(BCryptHash(catalog.sha256, nullptr, 0,
                                   static_cast<PUCHAR>(const_cast<void*>(vertexShader)), static_cast<ULONG>(size),
                                   digest.data(), static_cast<ULONG>(digest.size()))))
        return std::nullopt;
    if (hash)
        *hash = digest;
    const auto found = std::lower_bound(catalog.grafts.begin(), catalog.grafts.end(), digest,
                                        [](const Graft& graft, const auto& key) { return graft.sha < key; });
    if (found == catalog.grafts.end() || found->sha != digest || !loadBytes(*found))
        return std::nullopt;
    return NativeGraft { found->bytes.data(), found->bytes.size(), found->currentOutput, found->previousOutput,
                        found->supplyClass };
}

std::size_t NativeGraftCount() noexcept { return catalog.count.load(std::memory_order_acquire); }
} // namespace GlassFg
