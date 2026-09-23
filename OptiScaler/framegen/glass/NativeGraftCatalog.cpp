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
// index.bin: "GGRAFT02", u32 count, u32 reserved (0), then count records of
// { u8 sha256[32]; u32 currentOutput; u32 previousOutput; u32 supplyClass;
//   u32 cameraCurrentOutput; u32 cameraPreviousOutput; } (little endian).
// supplyClass: bit 0 root-only, bit 1 skinning, bit 2 preskinned. Camera outputs
// are NoOutput when the camera-only variant <sha>.camera.dxil is absent. Root
// outputs are NoOutput for a camera-only record (no <sha>.dxil), which then
// requires the camera outputs.
constexpr char IndexMagic[8] = { 'G', 'G', 'R', 'A', 'F', 'T', '0', '2' };
constexpr std::size_t IndexHeaderBytes = 16, IndexRecordBytes = 52;
constexpr std::uint32_t NoOutput = 0xFFFFFFFFu;
// The exported catalog holds several hundred records; the bound keeps a corrupt count from
// sizing an allocation. Shader bytes follow the compiler's own 2 MiB limit.
constexpr std::uint32_t MaxGrafts = 4096;
constexpr std::uintmax_t MaxGraftBytes = 2 * 1024 * 1024;

struct Graft
{
    std::array<std::uint8_t, 32> sha {};
    unsigned currentOutput = 0, previousOutput = 0, supplyClass = 0;
    unsigned cameraCurrentOutput = NoOutput, cameraPreviousOutput = NoOutput;
    // Written once under loadMutex; never resized afterwards, so a returned
    // pointer stays valid for the process lifetime.
    std::vector<std::byte> bytes, cameraBytes;
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
            std::uint32_t current = 0, previous = 0, supplyClass = 0, cameraCurrent = 0, cameraPrevious = 0;
            std::memcpy(&current, record + 32, 4);
            std::memcpy(&previous, record + 36, 4);
            std::memcpy(&supplyClass, record + 40, 4);
            std::memcpy(&cameraCurrent, record + 44, 4);
            std::memcpy(&cameraPrevious, record + 48, 4);
            const bool noRoot = current == NoOutput && previous == NoOutput;
            const bool noCamera = cameraCurrent == NoOutput && cameraPrevious == NoOutput;
            if ((noRoot && noCamera) || supplyClass > 7)
                return;
            if (!noRoot && (current == previous || current >= 32 || previous >= 32))
                return;
            if (!noCamera && (cameraCurrent == cameraPrevious || cameraCurrent >= 32 || cameraPrevious >= 32))
                return;
            graft.currentOutput = current;
            graft.previousOutput = previous;
            graft.supplyClass = supplyClass;
            graft.cameraCurrentOutput = cameraCurrent;
            graft.cameraPreviousOutput = cameraPrevious;
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

bool readContainer(const std::filesystem::path& path, std::vector<std::byte>& out) noexcept
{
    try
    {
        std::error_code error;
        const auto size = std::filesystem::file_size(path, error);
        if (error || size < 32 || size > MaxGraftBytes)
            return false;
        std::vector<std::byte> bytes(static_cast<std::size_t>(size));
        std::ifstream file(path, std::ios::binary);
        if (!file || !file.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(size)) ||
            std::memcmp(bytes.data(), "DXBC", 4) != 0)
            return false;
        out = std::move(bytes);
        return true;
    }
    catch (...)
    {
        out.clear();
        return false;
    }
}

// First hit only; a failed read is remembered so the files are not retried. The
// camera-only variant is optional beside a root graft: its absence leaves
// cameraBytes empty. A camera-only record needs its camera variant.
bool loadBytes(Graft& graft) noexcept
{
    const bool root = graft.currentOutput != NoOutput;
    std::lock_guard lock(catalog.loadMutex);
    if (graft.attempted)
        return root ? !graft.bytes.empty() : !graft.cameraBytes.empty();
    graft.attempted = true;
    try
    {
        const auto name = hex(graft.sha);
        if (root && !readContainer(catalog.directory / (name + L".dxil"), graft.bytes))
            return false;
        if (graft.cameraCurrentOutput != NoOutput &&
            !readContainer(catalog.directory / (name + L".camera.dxil"), graft.cameraBytes))
            graft.cameraBytes.clear();
        return root || !graft.cameraBytes.empty();
    }
    catch (...)
    {
        graft.bytes.clear();
        graft.cameraBytes.clear();
        return false;
    }
}
} // namespace

bool HashShaderSha256(const void* bytes, std::size_t size, std::array<std::uint8_t, 32>& digest) noexcept
{
    digest = {};
    // The provider is opened with the index, so the first hash also loads it.
    std::call_once(catalog.indexOnce, loadIndex);
    if (!bytes || !size || size > ULONG_MAX || !catalog.sha256)
        return false;
    if (BCRYPT_SUCCESS(BCryptHash(catalog.sha256, nullptr, 0, static_cast<PUCHAR>(const_cast<void*>(bytes)),
                                  static_cast<ULONG>(size), digest.data(), static_cast<ULONG>(digest.size()))))
        return true;
    digest = {};
    return false;
}

std::optional<NativeGraft> FindNativeGraft(const void* vertexShader, std::size_t size,
                                           std::array<std::uint8_t, 32>* hash) noexcept
{
    std::array<std::uint8_t, 32> digest {};
    const bool hashed = HashShaderSha256(vertexShader, size, digest);
    if (hash)
        *hash = digest;
    if (!hashed)
        return std::nullopt;
    const auto found = std::lower_bound(catalog.grafts.begin(), catalog.grafts.end(), digest,
                                        [](const Graft& graft, const auto& key) { return graft.sha < key; });
    if (found == catalog.grafts.end() || found->sha != digest || !loadBytes(*found))
        return std::nullopt;
    const bool root = !found->bytes.empty(), camera = !found->cameraBytes.empty();
    return NativeGraft { root ? found->bytes.data() : nullptr,
                         found->bytes.size(),
                         root ? found->currentOutput : 0,
                         root ? found->previousOutput : 0,
                         found->supplyClass,
                         camera ? found->cameraBytes.data() : nullptr,
                         camera ? found->cameraBytes.size() : 0,
                         camera ? found->cameraCurrentOutput : 0,
                         camera ? found->cameraPreviousOutput : 0 };
}

std::size_t NativeGraftCount() noexcept { return catalog.count.load(std::memory_order_acquire); }
} // namespace GlassFg
