#include "pch.h"
#include "NativeMotionDeclarations.h"
#include "CyberpunkDeclarationProfile.h"
#include "CyberpunkLayout.h"
#include "DetourThreads.h"
#include "GeometryHealth.h"
#include "MotionDeclarationTable.h"
#include "MotionShaderScope.h"
#include <Util.h>
#include <intrin.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <vector>

namespace GlassFg
{
namespace
{
using GetMetadata = int (*)(void* provider, std::uint64_t key, void** result);
using ResolveStage = bool (*)(void* combination, std::uint8_t stage, void* unused, void** shader,
                              std::uint32_t* mask, void* mergedNames);

// Engine entries until the commit, trampolines after it.
GetMetadata originalMetadata = nullptr;
ResolveStage originalStage = nullptr;
// Return address of the checked provider call inside the stage function: the
// only caller whose metadata result may be replaced.
std::uint64_t stageReturn = 0;
// Set after both tables are configured and before the commit releases the
// suspended threads; never cleared, the detours stay for the process lifetime.
std::atomic<bool> enabled = false;
std::atomic<const char*> status = "not_started";
MotionDeclarationTable declarationTable;
MotionShaderScope shaderScope;

bool read(std::uint64_t address, void* out, std::size_t bytes) noexcept
{
    __try
    {
        if (address < 0x10000 || address > 0x7fffffffffffULL - bytes)
            return false;
        std::memcpy(out, reinterpret_cast<const void*>(address), bytes);
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
}

int metadataCall(void* provider, std::uint64_t key, void** result, std::uint64_t caller)
{
    const auto value = originalMetadata(provider, key, result);
    if (!enabled.load(std::memory_order_acquire))
        return value;
    NoteGeometryGraft(DeclarationSeen);
    if (value != 1 || !result || !*result || !MotionShaderScope::Invocation::allows(key, caller))
        return value;
    try
    {
        using State = MotionDeclarationTable::State;
        const auto rewritten = declarationTable.rewrite(key, *result, read);
        if (rewritten.metadata)
        {
            *result = const_cast<MotionDeclarationTable::Metadata*>(rewritten.metadata);
            NoteGeometryGraft(DeclarationMatched);
        }
        else if (rewritten.state != State::NotSelected)
            NoteGeometryGraft(DeclarationRejected);
    }
    catch (...)
    {
        // The engine keeps its own declaration.
    }
    return value;
}

int hook(void* provider, std::uint64_t key, void** result)
{
    return metadataCall(provider, key, result, reinterpret_cast<std::uint64_t>(_ReturnAddress()));
}

bool stageHook(void* combination, std::uint8_t stage, void* unused, void** shader, std::uint32_t* mask,
               void* mergedNames)
{
    std::uint64_t key = 0;
    if (enabled.load(std::memory_order_acquire))
    {
        NoteGeometryGraft(DeclarationStageSeen);
        // Original stage 0 resolves combination+8 (VS); stage 1 resolves +16
        // (PS). The loaded cache confirms this ordering for every selected pair.
        std::array<std::uint64_t, 3> record {};
        if (stage == 0 && read(reinterpret_cast<std::uint64_t>(combination), record.data(), sizeof(record)))
        {
            key = shaderScope.lookup(record[1], record[2]);
            if (key)
                NoteGeometryGraft(DeclarationStageSelected);
        }
    }
    // Always scoped: an unselected (or nested) stage call suppresses an outer
    // selection until it returns.
    MotionShaderScope::Invocation scope(key, stageReturn);
    return originalStage(combination, stage, unused, shader, mask, mergedNames);
}

bool resolveNativeRoutes(std::uint64_t base, std::uint64_t& providerEntry, std::uint64_t& stageEntry)
{
    IMAGE_DOS_HEADER dos {};
    IMAGE_NT_HEADERS64 nt {};
    if (!read(base, &dos, sizeof(dos)) || dos.e_magic != IMAGE_DOS_SIGNATURE || dos.e_lfanew < 0 ||
        dos.e_lfanew > 4096 || !read(base + dos.e_lfanew, &nt, sizeof(nt)) || nt.Signature != IMAGE_NT_SIGNATURE ||
        nt.OptionalHeader.SizeOfImage < 4096 || nt.OptionalHeader.SizeOfImage > 1024u * 1024u * 1024u)
        return false;
    RelocatableCode image;
    CyberpunkDeclarations::Layout layout;
    if (!image.initialize({ reinterpret_cast<const unsigned char*>(base), nt.OptionalHeader.SizeOfImage }) ||
        !layout.resolve(image))
        return false;
    providerEntry = base + layout.metadata;
    stageEntry = base + layout.stage;
    return true;
}

enum class Load
{
    Ready,
    Missing,
    Rejected
};

Load loadDeclarations(const std::filesystem::path& directory)
{
    std::ifstream declarations(directory / L"motion-declarations.bin", std::ios::binary);
    std::ifstream pairFile(directory / L"motion-shader-pairs.bin", std::ios::binary);
    if (!declarations || !pairFile)
        return Load::Missing;
    using Table = MotionDeclarationTable;
    using Scope = MotionShaderScope;
    char magic[8] {};
    std::uint32_t count = 0, nameCount = 0;
    declarations.read(magic, sizeof(magic));
    declarations.read(reinterpret_cast<char*>(&count), sizeof(count));
    declarations.read(reinterpret_cast<char*>(&nameCount), sizeof(nameCount));
    if (!declarations || std::memcmp(magic, "GMDPLAN1", sizeof(magic)) || !count || count > Table::MaxDeclarations ||
        nameCount > Table::MaxNames)
        return Load::Rejected;
    std::vector<Table::Plan> plans(count);
    std::vector<Table::Name> names(nameCount);
    declarations.read(reinterpret_cast<char*>(plans.data()), count * sizeof(Table::Plan));
    declarations.read(reinterpret_cast<char*>(names.data()), nameCount * sizeof(Table::Name));
    if (!declarations || declarations.peek() != std::char_traits<char>::eof())
        return Load::Rejected;
    std::uint32_t pairCount = 0, reserved = 0;
    pairFile.read(magic, sizeof(magic));
    pairFile.read(reinterpret_cast<char*>(&pairCount), sizeof(pairCount));
    pairFile.read(reinterpret_cast<char*>(&reserved), sizeof(reserved));
    if (!pairFile || std::memcmp(magic, "GMSPAIR1", sizeof(magic)) || reserved || !pairCount ||
        pairCount > Scope::MaxPairs)
        return Load::Rejected;
    std::vector<Scope::Pair> pairs(pairCount);
    pairFile.read(reinterpret_cast<char*>(pairs.data()), pairCount * sizeof(Scope::Pair));
    if (!pairFile || pairFile.peek() != std::char_traits<char>::eof())
        return Load::Rejected;
    // A pair file from another declaration set must not select keys this
    // table does not declare.
    for (const auto& pair : pairs)
    {
        const auto declares = [&](const Table::Plan& plan) { return plan.key == pair.metadata; };
        if (std::none_of(plans.begin(), plans.end(), declares))
            return Load::Rejected;
    }
    return declarationTable.configure(plans, names) && shaderScope.configure(pairs) ? Load::Ready : Load::Rejected;
}

// Sets step to the token of each step before running it, so a failure (or an
// exception) leaves the token of the step that stopped installation.
bool install(HMODULE executable, const char*& step)
{
    step = "not_cyberpunk";
    if (!IsCyberpunkExecutable(executable))
        return false;
    step = "native_layout_rejected";
    std::uint64_t providerEntry = 0, stageEntry = 0;
    if (!resolveNativeRoutes(reinterpret_cast<std::uint64_t>(executable), providerEntry, stageEntry))
        return false;
    step = "declaration_file_missing";
    const auto loaded = loadDeclarations(Util::DllPath().parent_path() / L"Glass");
    if (loaded != Load::Ready)
    {
        if (loaded == Load::Rejected)
            step = "declaration_profile_rejected";
        return false;
    }
    step = "hook_attach_failed";
    // The detours and the returned declaration copies live in this module.
    HMODULE pinned = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
                            reinterpret_cast<LPCWSTR>(&InstallNativeMotionDeclarations), &pinned))
        return false;
    originalMetadata = reinterpret_cast<GetMetadata>(providerEntry);
    originalStage = reinterpret_cast<ResolveStage>(stageEntry);
    stageReturn = stageEntry + CyberpunkDeclarations::StageMetadataReturn;
    DetourThreads threads;
    if (!threads.gather() || DetourTransactionBegin() != NO_ERROR)
        return false;
    if (DetourAttach(reinterpret_cast<PVOID*>(&originalMetadata), reinterpret_cast<PVOID>(&hook)) != NO_ERROR ||
        DetourAttach(reinterpret_cast<PVOID*>(&originalStage), reinterpret_cast<PVOID>(&stageHook)) != NO_ERROR ||
        !threads.enlist())
    {
        DetourTransactionAbort();
        return false;
    }
    enabled.store(true, std::memory_order_release);
    if (DetourTransactionCommit() != NO_ERROR)
    {
        enabled.store(false, std::memory_order_release);
        return false;
    }
    NoteGeometryGraft(DeclarationHookInstalled);
    return true;
}
} // namespace

void InstallNativeMotionDeclarations() noexcept
{
    static std::once_flag once;
    try
    {
        std::call_once(once,
                       []
                       {
                           const char* step = "not_cyberpunk";
                           try
                           {
                               if (install(GetModuleHandleW(nullptr), step))
                                   step = "installed";
                           }
                           catch (...)
                           {
                           }
                           status.store(step, std::memory_order_release);
                       });
    }
    catch (...)
    {
    }
}

bool TryNativeMotionDeclarationCounters(NativeMotionDeclarationStats& stats) noexcept
{
    stats.seen = ReadGeometryGraft(DeclarationSeen);
    stats.matched = ReadGeometryGraft(DeclarationMatched);
    stats.rejected = ReadGeometryGraft(DeclarationRejected);
    stats.stageSeen = ReadGeometryGraft(DeclarationStageSeen);
    stats.stageSelected = ReadGeometryGraft(DeclarationStageSelected);
    stats.status = status.load(std::memory_order_acquire);
    return ReadGeometryGraft(DeclarationHookInstalled) != 0;
}
} // namespace GlassFg
