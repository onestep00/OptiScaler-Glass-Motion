#pragma once
#include "CyberpunkLayoutProfile.h"
#include <psapi.h>
#include <mutex>

namespace GlassFg
{
struct CyberpunkLayout
{
    enum Function : unsigned
    {
        Register,
        Remove,
        Update,
        Run,
        Append,
        Rigid,
        Skinned,
        Upload,
        Backend,
        SetArray,
        Count
    };
    std::array<std::uint32_t, Count> functions {};
    std::uint32_t rendererGlobal = 0, tick = 0, drawReturn = 0;

    bool resolve(const RelocatableCode& image)
    {
        *this = {};
        for (unsigned i = 0; i < Count; ++i)
            if (!(functions[i] = image.unique(CyberpunkProfile::functions[i])))
                return false;
        using Target = RelocatableCode::Target;
        const auto target = [&](Function function, std::uint16_t offset, Target kind)
        { return image.referenced(functions[function], { offset, std::uint16_t(offset + 4), kind }); };
        rendererGlobal = target(Register, 0x46, Target::Writable);
        tick = target(Update, 0x1e4, Target::Writable);
        // Cross-function relationships give address operands a meaning. A
        // unique body alone does not establish that it references our registry.
        if ((rendererGlobal & 7) || (tick & 3) || !image.contains(rendererGlobal, 8, Target::Writable) ||
            !image.contains(tick, 4, Target::Writable) || target(Remove, 0x17, Target::Writable) != rendererGlobal ||
            target(SetArray, 0xfd, Target::Writable) != rendererGlobal ||
            target(SetArray, 0x179, Target::Writable) + 8 != tick ||
            target(Run, 0x53, Target::Writable) != rendererGlobal ||
            target(Run, 0x12b, Target::Writable) != rendererGlobal ||
            target(Backend, 0x75, Target::Writable) + 8 != tick ||
            target(Backend, 0xa0, Target::Writable) + 8 != tick ||
            target(Run, 0x663, Target::Code) != functions[Append] ||
            target(Rigid, 0x67, Target::Code) != functions[Backend] ||
            target(Rigid, 0x8d, Target::Code) != functions[Upload] ||
            target(Skinned, 0x5b, Target::Code) != functions[Upload] ||
            target(Skinned, 0x80, Target::Code) != functions[Backend])
            return false;
        drawReturn = functions[Backend] + 0x22a; // Fixed within the validated body.
        return true;
    }
};

inline bool IsCyberpunkExecutable(HMODULE executable)
{
    wchar_t path[32768] {};
    if (!executable || !GetModuleFileNameW(executable, path, 32768))
        return false;
    const auto* filename = wcsrchr(path, L'\\');
    return _wcsicmp(filename ? filename + 1 : path, L"Cyberpunk2077.exe") == 0;
}

inline const CyberpunkLayout* GetCyberpunkLayout(HMODULE executable) noexcept
{
    // Resolve once before installing our engine hooks. The scan is never on a
    // frame/draw path, and later calls do not inspect our own detoured bytes.
    static std::once_flag once;
    static CyberpunkLayout layout;
    static HMODULE admitted = nullptr;
    try
    {
        std::call_once(
            once,
            [executable]
            {
                MODULEINFO info {};
                if (!IsCyberpunkExecutable(executable) ||
                    !K32GetModuleInformation(GetCurrentProcess(), executable, &info, sizeof(info)) ||
                    info.lpBaseOfDll != executable)
                    return;
                RelocatableCode image;
                if (image.initialize({ reinterpret_cast<const unsigned char*>(executable), info.SizeOfImage }) &&
                    layout.resolve(image))
                    admitted = executable;
            });
    }
    catch (...)
    {
        return nullptr;
    }
    return admitted == executable && admitted ? &layout : nullptr;
}
} // namespace GlassFg
