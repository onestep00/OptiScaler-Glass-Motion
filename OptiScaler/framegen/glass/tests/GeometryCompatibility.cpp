// Read-only PE input. Relocations/corruptions happen in owned, non-executable
// byte vectors; this test never loads, attaches to or modifies a game process.
#include "../CyberpunkLayout.h"
#include "../CyberpunkSurfacePass.h"
#include <filesystem>
#include <fstream>
#include <vector>
#include <stdexcept>
#include <cstdio>

static void require(bool value, const char* message)
{
    if (!value)
        throw std::runtime_error(message);
}
static IMAGE_NT_HEADERS64& headers(std::vector<unsigned char>& bytes)
{
    const auto offset = reinterpret_cast<const IMAGE_DOS_HEADER*>(bytes.data())->e_lfanew;
    return *reinterpret_cast<IMAGE_NT_HEADERS64*>(bytes.data() + offset);
}
static std::vector<unsigned char> map(const std::filesystem::path& path)
{
    const auto size = std::filesystem::file_size(path);
    require(size >= 4096 && size <= 256 * 1024 * 1024, "PE file size");
    std::vector<unsigned char> file(static_cast<size_t>(size));
    std::ifstream input(path, std::ios::binary);
    require(bool(input.read(reinterpret_cast<char*>(file.data()), file.size())), "PE read");
    const auto& dos = *reinterpret_cast<const IMAGE_DOS_HEADER*>(file.data());
    require(dos.e_magic == IMAGE_DOS_SIGNATURE && dos.e_lfanew > 0 && dos.e_lfanew < 1024, "DOS header");
    const auto& nt = headers(file);
    require(nt.Signature == IMAGE_NT_SIGNATURE &&
                nt.FileHeader.SizeOfOptionalHeader == sizeof(IMAGE_OPTIONAL_HEADER64) &&
                nt.FileHeader.NumberOfSections <= 32 && nt.OptionalHeader.SizeOfImage <= 256 * 1024 * 1024 &&
                nt.OptionalHeader.SizeOfImage > 4096,
            "PE header");
    std::vector<unsigned char> result(nt.OptionalHeader.SizeOfImage);
    memcpy(result.data(), file.data(), 4096);
    const auto* sections = IMAGE_FIRST_SECTION(&nt);
    for (unsigned i = 0; i < nt.FileHeader.NumberOfSections; ++i)
    {
        const auto& s = sections[i];
        require(std::uint64_t(s.PointerToRawData) + s.SizeOfRawData <= file.size() &&
                    std::uint64_t(s.VirtualAddress) + s.SizeOfRawData <= result.size(),
                "Section bounds");
        memcpy(result.data() + s.VirtualAddress, file.data() + s.PointerToRawData, s.SizeOfRawData);
    }
    return result;
}
static bool resolve(const std::vector<unsigned char>& bytes, GlassFg::CyberpunkLayout& layout)
{
    GlassFg::RelocatableCode image;
    return image.initialize(bytes) && layout.resolve(image);
}
static void displacement(std::vector<unsigned char>& bytes, UINT start, const GlassFg::RelocatableCode::Reference& ref,
                         UINT target)
{
    const auto delta = std::int64_t(target) - start - ref.next;
    require(delta >= INT32_MIN && delta <= INT32_MAX, "rel32 range");
    const auto value = static_cast<std::int32_t>(delta);
    memcpy(bytes.data() + start + ref.offset, &value, sizeof(value));
}
int wmain(int argc, wchar_t** argv)
{
    try
    {
        require(argc == 2, "GeometryCompatibility path-to-user-owned-Cyberpunk2077.exe");
        auto original = map(argv[1]);
        GlassFg::CyberpunkLayout baseline, actual;
        require(resolve(original, baseline), "Current executable profile resolution");
        GlassFg::RelocatableCode before;
        require(before.initialize(original), "Current PE inspection");
        GlassFg::CyberpunkSurfacePass pass;
        require(pass.resolve(before, original.data()), "Current surface pattern resolution");
        const auto initialCallers = pass.callers();
        auto relocated = original;
        const auto oldSize = static_cast<UINT>(relocated.size());
        relocated.resize(oldSize + 0x10000);
        auto& nt = headers(relocated);
        nt.FileHeader.TimeDateStamp ^= 0x12345678;
        nt.OptionalHeader.CheckSum ^= 0x10203040;
        nt.OptionalHeader.SizeOfImage = static_cast<DWORD>(relocated.size());
        auto* sections = IMAGE_FIRST_SECTION(&nt);
        auto& code = sections[nt.FileHeader.NumberOfSections++];
        code = {};
        code.VirtualAddress = oldSize;
        code.Misc.VirtualSize = 0xe000;
        code.Characteristics = IMAGE_SCN_MEM_READ | IMAGE_SCN_MEM_EXECUTE;
        auto& globals = sections[nt.FileHeader.NumberOfSections++];
        globals = {};
        globals.VirtualAddress = oldSize + 0xe000;
        globals.Misc.VirtualSize = 0x2000;
        globals.Characteristics = IMAGE_SCN_MEM_READ | IMAGE_SCN_MEM_WRITE;

        auto expected = baseline;
        UINT cursor = oldSize;
        for (unsigned i = 0; i < expected.Count; ++i)
        {
            expected.functions[i] = cursor;
            cursor += (GlassFg::CyberpunkProfile::functions[i].bytes + 31) & ~31u;
        }
        expected.rendererGlobal = globals.VirtualAddress;
        expected.tick = globals.VirtualAddress + 16;
        expected.drawReturn = expected.functions[expected.Backend] + 0x22a;
        const auto moveTarget = [&](UINT target)
        {
            for (unsigned i = 0; i < expected.Count; ++i)
                if (target == baseline.functions[i])
                    return expected.functions[i];
            if (target == baseline.rendererGlobal)
                return expected.rendererGlobal;
            if (target == baseline.tick)
                return expected.tick;
            if (target == baseline.tick - 8)
                return expected.tick - 8;
            return target;
        };
        auto& directory = nt.OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXCEPTION];
        auto* functions = reinterpret_cast<RUNTIME_FUNCTION*>(relocated.data() + directory.VirtualAddress);
        const auto functionCount = directory.Size / sizeof(RUNTIME_FUNCTION);
        for (unsigned i = 0; i < expected.Count; ++i)
        {
            const auto& profile = GlassFg::CyberpunkProfile::functions[i];
            memcpy(relocated.data() + expected.functions[i], original.data() + baseline.functions[i], profile.bytes);
            memset(relocated.data() + baseline.functions[i], 0xcc, profile.bytes);
            for (const auto& ref : profile.references)
                displacement(relocated, expected.functions[i], ref,
                             moveTarget(before.referenced(baseline.functions[i], ref)));
            bool replaced = false;
            for (size_t j = 0; j < functionCount; ++j)
                if (functions[j].BeginAddress == baseline.functions[i])
                {
                    functions[j].BeginAddress = expected.functions[i];
                    functions[j].EndAddress = expected.functions[i] + profile.bytes;
                    replaced = true;
                }
            require(replaced, "Unwind entry move");
        }
        std::sort(functions, functions + functionCount,
                  [](const auto& a, const auto& b) { return a.BeginAddress < b.BeginAddress; });
        require(resolve(relocated, actual) && actual.functions == expected.functions &&
                    actual.rendererGlobal == expected.rendererGlobal && actual.tick == expected.tick &&
                    actual.drawReturn == expected.drawReturn,
                "Relocated function/global discovery");

        // Move the actual 64-byte surface signatures independently and retarget
        // every masked call. Existing patterns disappear; new positions remain unique.
        constexpr unsigned rayOffsets[] { 17, 28, 40, 53 }, surfaceOffsets[] { 28, 38, 58 };
        const auto moveWindow = [&](UINT old, std::span<const unsigned> offsets)
        {
            const auto next = cursor;
            cursor += 128;
            memcpy(relocated.data() + next, original.data() + old, 64);
            memset(relocated.data() + old, 0xcc, 64);
            for (const auto offset : offsets)
            {
                const GlassFg::RelocatableCode::Reference ref { static_cast<std::uint16_t>(offset),
                                                                static_cast<std::uint16_t>(offset + 4),
                                                                GlassFg::RelocatableCode::Target::Code };
                displacement(relocated, next, ref, before.referenced(old, ref));
            }
            return next + 32;
        };
        const auto ray = moveWindow(
            static_cast<UINT>(static_cast<const unsigned char*>(initialCallers[0]) - original.data() - 32), rayOffsets);
        const auto surface =
            moveWindow(static_cast<UINT>(static_cast<const unsigned char*>(initialCallers[1]) - original.data() - 32),
                       surfaceOffsets);
        GlassFg::RelocatableCode after;
        require(after.initialize(relocated) && pass.resolve(after, relocated.data()), "Relocated surface discovery");
        void* stack[] { relocated.data() + ray, relocated.data() + surface };
        require(pass.matchesStack(stack, 2), "Relocated ordered consumer stack");
        std::swap(stack[0], stack[1]);
        require(!pass.matchesStack(stack, 2), "Reversed consumer stack admitted");

        const auto field = expected.functions[expected.Register] + 8;
        relocated[field] ^= 4; // The proxy registry field displacement, not a masked address.
        require(!resolve(relocated, actual), "Changed object layout admitted");
        relocated[field] ^= 4;
        const GlassFg::RelocatableCode::Reference link { 0x663, 0x667, GlassFg::RelocatableCode::Target::Code };
        displacement(relocated, expected.functions[expected.Run], link, expected.functions[expected.Remove]);
        require(!resolve(relocated, actual), "Incorrect call relationship admitted");
        displacement(relocated, expected.functions[expected.Run], link, expected.functions[expected.Append]);
        const GlassFg::RelocatableCode::Reference globalLink { 0x17, 0x1b, GlassFg::RelocatableCode::Target::Writable };
        displacement(relocated, expected.functions[expected.Remove], globalLink, expected.rendererGlobal + 8);
        require(!resolve(relocated, actual), "Incorrect registry alias admitted");
        displacement(relocated, expected.functions[expected.Remove], globalLink, expected.rendererGlobal);
        require(resolve(relocated, actual), "Restored compatible image rejected");
        // Copy a normalized function into another existing same-size unwind entry.
        const auto& profile = GlassFg::CyberpunkProfile::functions[expected.Remove];
        bool duplicate = false;
        for (size_t j = 0; j < functionCount; ++j)
            if (functions[j].EndAddress - functions[j].BeginAddress == profile.bytes &&
                functions[j].BeginAddress != expected.functions[expected.Remove])
            {
                const auto at = functions[j].BeginAddress;
                memcpy(relocated.data() + at, relocated.data() + expected.functions[expected.Remove], profile.bytes);
                for (const auto& ref : profile.references)
                    displacement(relocated, at, ref, after.referenced(expected.functions[expected.Remove], ref));
                duplicate = true;
                break;
            }
        require(duplicate && !resolve(relocated, actual), "Ambiguous function admitted");
        printf("PASS current_profile=1 moved_functions=10 moved_globals=2 changed_timestamp=1 changed_image_size=1 "
               "moved_surface_patterns=2 object_layout_rejected=1 call_graph_rejected=1 registry_alias_rejected=1 "
               "ambiguity_rejected=1 executed_game_code=0 game_hooks=0\n");
        return 0;
    }
    catch (const std::exception& error)
    {
        fprintf(stderr, "FAIL %s\n", error.what());
        return 1;
    }
}
