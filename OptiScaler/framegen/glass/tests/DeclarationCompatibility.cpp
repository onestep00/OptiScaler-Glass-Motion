// All relocation and corruption checks use non-executable owned file bytes.
#include "../CyberpunkDeclarationProfile.h"
#include <filesystem>
#include <fstream>
#include <vector>
#include <stdexcept>
#include <cstdio>

static void require(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}
static IMAGE_NT_HEADERS64& headers(std::vector<unsigned char>& bytes) {
    return *reinterpret_cast<IMAGE_NT_HEADERS64*>(bytes.data() +
        reinterpret_cast<const IMAGE_DOS_HEADER*>(bytes.data())->e_lfanew);
}
static std::vector<unsigned char> map(const std::filesystem::path& path) {
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
            nt.OptionalHeader.SizeOfImage > 4096, "PE header");
    std::vector<unsigned char> result(nt.OptionalHeader.SizeOfImage);
    std::memcpy(result.data(), file.data(), 4096);
    const auto* sections = IMAGE_FIRST_SECTION(&nt);
    for (unsigned i = 0; i < nt.FileHeader.NumberOfSections; ++i) {
        const auto& section = sections[i];
        require(uint64_t(section.PointerToRawData) + section.SizeOfRawData <= file.size() &&
                uint64_t(section.VirtualAddress) + section.SizeOfRawData <= result.size(), "PE section bounds");
        std::memcpy(result.data() + section.VirtualAddress, file.data() + section.PointerToRawData, section.SizeOfRawData);
    }
    return result;
}
static bool resolve(const std::vector<unsigned char>& bytes, GlassFg::CyberpunkDeclarations::Layout& layout) {
    GlassFg::RelocatableCode image;
    return image.initialize(bytes) && layout.resolve(image);
}
int wmain(int argc, wchar_t** argv) {
    try {
        require(argc == 2, "Supply a local executable file; no game process is opened");
        auto bytes = map(argv[1]);
        GlassFg::CyberpunkDeclarations::Layout original{}, changed{};
        require(resolve(bytes, original), "original native profiles did not resolve");
        const auto directory = headers(bytes).OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXCEPTION];
        auto* functions = reinterpret_cast<RUNTIME_FUNCTION*>(bytes.data() + directory.VirtualAddress);
        const size_t count = directory.Size / sizeof(RUNTIME_FUNCTION);
        size_t donor = count;
        const auto& profile = GlassFg::CyberpunkDeclarations::metadataProfile;
        for (size_t i = 0; i < count; ++i) {
            const auto rva = functions[i].BeginAddress;
            if (functions[i].EndAddress - rva >= profile.bytes && rva != original.metadata &&
                rva != original.lookup && rva != original.builder && rva != original.supplier && rva != original.uploader &&
                rva != original.stage) {
                donor = i; break;
            }
        }
        require(donor < count, "owned relocation destination");
        const auto target = functions[donor].BeginAddress;
        std::vector<unsigned char> body(profile.bytes);
        std::memcpy(body.data(), bytes.data() + original.metadata, body.size());
        for (const auto& ref : profile.references) {
            int32_t displacement = 0;
            std::memcpy(&displacement, body.data() + ref.offset, 4);
            const auto relative = int64_t(original.metadata) + ref.next + displacement - target - ref.next;
            require(relative >= INT32_MIN && relative <= INT32_MAX, "rel32 range");
            displacement = static_cast<int32_t>(relative);
            std::memcpy(body.data() + ref.offset, &displacement, 4);
        }
        std::memcpy(bytes.data() + target, body.data(), body.size());
        functions[donor].EndAddress = target + profile.bytes;
        require(!resolve(bytes, changed), "ambiguous duplicate profile accepted");
        std::memset(bytes.data() + original.metadata, 0xcc, profile.bytes);
        require(resolve(bytes, changed) && changed.metadata == target && changed.lookup == original.lookup,
                "relocated callback and lookup link");
        const auto& call = profile.references[0];
        int32_t saved = 0;
        std::memcpy(&saved, bytes.data() + target + call.offset, 4);
        const auto wrong = static_cast<int32_t>(int64_t(original.supplier) - target - call.next);
        std::memcpy(bytes.data() + target + call.offset, &wrong, 4);
        require(!resolve(bytes, changed), "wrong referenced helper accepted");
        std::memcpy(bytes.data() + target + call.offset, &saved, 4);
        bytes[target] ^= 1;
        require(!resolve(bytes, changed), "changed non-address code accepted");
        bytes = map(argv[1]);
        const auto& stage = GlassFg::CyberpunkDeclarations::stageProfile;
        functions = reinterpret_cast<RUNTIME_FUNCTION*>(bytes.data() + directory.VirtualAddress);
        donor = count;
        for (size_t i = 0; i < count; ++i) {
            const auto rva = functions[i].BeginAddress;
            if (functions[i].EndAddress - rva >= stage.bytes && rva != original.metadata &&
                rva != original.lookup && rva != original.builder && rva != original.supplier &&
                rva != original.uploader && rva != original.stage) { donor = i; break; }
        }
        require(donor < count, "stage relocation destination");
        const auto stageTarget = functions[donor].BeginAddress;
        body.resize(stage.bytes);
        std::memcpy(body.data(), bytes.data() + original.stage, body.size());
        for (const auto& ref : stage.references) {
            int32_t displacement = 0;
            std::memcpy(&displacement, body.data() + ref.offset, 4);
            const auto delta = int64_t(original.stage) + displacement - stageTarget;
            require(delta >= INT32_MIN && delta <= INT32_MAX, "stage rel32 range");
            displacement = static_cast<int32_t>(delta);
            std::memcpy(body.data() + ref.offset, &displacement, 4);
        }
        std::memcpy(bytes.data() + stageTarget, body.data(), body.size());
        functions[donor].EndAddress = stageTarget + stage.bytes;
        require(!resolve(bytes, changed), "duplicate stage accepted");
        std::memset(bytes.data() + original.stage, 0xcc, stage.bytes);
        require(resolve(bytes, changed) && changed.stage == stageTarget, "relocated stage");
        const auto callSite = stageTarget + GlassFg::CyberpunkDeclarations::StageMetadataReturn - 3;
        require(bytes[callSite] == 0xff && bytes[callSite + 1] == 0x50 && bytes[callSite + 2] == 0x58,
                "metadata scope return site");
        std::printf("{\"native_routes\":6,\"relocated_metadata\":true,\"lookup_link_verified\":true,"
                    "\"relocated_stage\":true,\"stage_return_site_verified\":true,"
                    "\"ambiguous_rejected\":true,\"wrong_helper_rejected\":true,\"code_change_rejected\":true,\"game_attached\":false}\n");
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "%s\n", error.what()); return 1;
    }
}
