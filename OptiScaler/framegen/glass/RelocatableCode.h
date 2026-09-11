#pragma once
#include <windows.h>
#include <array>
#include <cstdint>
#include <cstring>
#include <span>
#include <algorithm>

namespace GlassFg
{
// Bounded PE inspection, never a disassembler or instruction patcher. Profiles
// mask only audited rel32/RIP address operands; object offsets and control flow
// remain in the hash. Call destinations and referenced sections are validated.
class RelocatableCode
{
  public:
    enum class Target : unsigned char
    {
        Code,
        ReadOnly,
        Writable
    };
    struct Reference
    {
        std::uint16_t offset, next;
        Target kind;
    };
    struct Profile
    {
        std::uint32_t bytes;
        std::uint64_t hash;
        std::span<const Reference> references;
    };

  private:
    std::span<const unsigned char> image;
    std::array<IMAGE_SECTION_HEADER, 96> sections {};
    unsigned sectionCount = 0;
    DWORD exceptionRva = 0, exceptionCount = 0;
    template <class T> bool read(std::uint64_t offset, T& value) const
    {
        if (offset > image.size() || sizeof(value) > image.size() - offset)
            return false;
        std::memcpy(&value, image.data() + offset, sizeof(value));
        return true;
    }
    const IMAGE_SECTION_HEADER* section(std::uint64_t rva, std::uint64_t bytes) const
    {
        if (!bytes || rva > image.size() || bytes > image.size() - rva)
            return nullptr;
        for (unsigned i = 0; i < sectionCount; ++i)
        {
            const auto& s = sections[i];
            if (rva >= s.VirtualAddress && rva + bytes <= std::uint64_t(s.VirtualAddress) + s.Misc.VirtualSize)
                return &s;
        }
        return nullptr;
    }
    bool matches(std::uint32_t rva, const Profile& profile) const
    {
        if (!contains(rva, profile.bytes, Target::Code))
            return false;
        std::uint32_t position = 0;
        std::uint64_t hash = 14695981039346656037ull;
        const auto add = [&](unsigned char value) { hash = (hash ^ value) * 1099511628211ull; };
        for (const auto& ref : profile.references)
        {
            if (ref.offset < position || unsigned(ref.offset) + 4 > profile.bytes ||
                ref.next < unsigned(ref.offset) + 4 || ref.next > profile.bytes)
                return false;
            while (position < ref.offset)
                add(image[rva + position++]);
            for (unsigned i = 0; i < 4; ++i, ++position)
                add(0);
            const auto target = referenced(rva, ref);
            if (!target || !contains(target, 1, ref.kind))
                return false;
        }
        while (position < profile.bytes)
            add(image[rva + position++]);
        return hash == profile.hash;
    }

  public:
    bool initialize(std::span<const unsigned char> bytes)
    {
        image = bytes;
        sectionCount = exceptionCount = 0;
        IMAGE_DOS_HEADER dos {};
        IMAGE_NT_HEADERS64 nt {};
        if (!read(0, dos) || dos.e_magic != IMAGE_DOS_SIGNATURE || dos.e_lfanew < 0 || dos.e_lfanew > 4096 ||
            !read(dos.e_lfanew, nt) || nt.Signature != IMAGE_NT_SIGNATURE ||
            nt.FileHeader.Machine != IMAGE_FILE_MACHINE_AMD64 ||
            nt.OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC ||
            nt.FileHeader.SizeOfOptionalHeader != sizeof(IMAGE_OPTIONAL_HEADER64) ||
            nt.OptionalHeader.NumberOfRvaAndSizes <= IMAGE_DIRECTORY_ENTRY_EXCEPTION ||
            nt.OptionalHeader.SizeOfImage > image.size() || nt.OptionalHeader.SizeOfImage < 4096 ||
            !nt.FileHeader.NumberOfSections || nt.FileHeader.NumberOfSections > sections.size())
            return false;
        image = image.first(nt.OptionalHeader.SizeOfImage);
        const auto headers = std::uint64_t(dos.e_lfanew) + sizeof(nt);
        for (unsigned i = 0; i < nt.FileHeader.NumberOfSections; ++i)
        {
            auto& s = sections[i];
            if (!read(headers + i * sizeof(s), s) || !s.Misc.VirtualSize ||
                std::uint64_t(s.VirtualAddress) + s.Misc.VirtualSize > image.size())
                return false;
            for (unsigned j = 0; j < i; ++j)
                if (std::uint64_t(s.VirtualAddress) <
                        std::uint64_t(sections[j].VirtualAddress) + sections[j].Misc.VirtualSize &&
                    std::uint64_t(sections[j].VirtualAddress) < std::uint64_t(s.VirtualAddress) + s.Misc.VirtualSize)
                    return false;
        }
        sectionCount = nt.FileHeader.NumberOfSections;
        const auto& directory = nt.OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXCEPTION];
        if (!directory.Size || directory.Size % sizeof(RUNTIME_FUNCTION) ||
            directory.Size / sizeof(RUNTIME_FUNCTION) > 1000000 ||
            !contains(directory.VirtualAddress, directory.Size, Target::ReadOnly))
            return false;
        exceptionRva = directory.VirtualAddress;
        exceptionCount = directory.Size / sizeof(RUNTIME_FUNCTION);
        return true;
    }
    bool contains(std::uint32_t rva, std::uint32_t bytes, Target kind) const
    {
        const auto* s = section(rva, bytes);
        if (!s || !(s->Characteristics & IMAGE_SCN_MEM_READ))
            return false;
        const bool code = (s->Characteristics & IMAGE_SCN_MEM_EXECUTE) != 0;
        const bool writable = (s->Characteristics & IMAGE_SCN_MEM_WRITE) != 0;
        return kind == Target::Code ? code : !code && (kind == Target::Writable ? writable : !writable);
    }
    std::uint32_t referenced(std::uint32_t start, const Reference& ref) const
    {
        std::int32_t displacement;
        if (!read(std::uint64_t(start) + ref.offset, displacement))
            return 0;
        const auto target = std::int64_t(start) + ref.next + displacement;
        return target > 0 && std::uint64_t(target) < image.size() ? static_cast<std::uint32_t>(target) : 0;
    }
    std::uint32_t unique(const Profile& profile) const
    {
        if (!exceptionCount || !profile.bytes || profile.bytes > 65536)
            return 0;
        std::uint32_t found = 0, previous = 0;
        for (DWORD i = 0; i < exceptionCount; ++i)
        {
            RUNTIME_FUNCTION f {};
            if (!read(std::uint64_t(exceptionRva) + std::uint64_t(i) * sizeof(f), f) || f.BeginAddress <= previous ||
                f.EndAddress <= f.BeginAddress)
                return 0;
            previous = f.BeginAddress;
            if (f.EndAddress - f.BeginAddress == profile.bytes && matches(f.BeginAddress, profile))
            {
                if (found)
                    return 0;
                found = f.BeginAddress;
            }
        }
        return found;
    }
    std::uint32_t uniqueWindow(std::span<const unsigned char> signature, std::span<const Reference> references) const
    {
        if (signature.empty() || signature.size() > 256 || references.empty() || !references.front().offset ||
            references.front().offset > signature.size())
            return 0;
        std::uint64_t hash = 14695981039346656037ull;
        unsigned position = 0;
        for (const auto& ref : references)
        {
            if (ref.offset < position || unsigned(ref.offset) + 4 > signature.size())
                return 0;
            while (position < ref.offset)
                hash = (hash ^ signature[position++]) * 1099511628211ull;
            for (unsigned i = 0; i < 4; ++i, ++position)
                hash *= 1099511628211ull;
        }
        while (position < signature.size())
            hash = (hash ^ signature[position++]) * 1099511628211ull;
        const Profile profile { static_cast<std::uint32_t>(signature.size()), hash, references };
        const auto anchor = signature.first(references.front().offset);
        std::uint32_t found = 0;
        for (unsigned i = 0; i < sectionCount; ++i)
        {
            const auto& s = sections[i];
            if (!(s.Characteristics & IMAGE_SCN_MEM_EXECUTE))
                continue;
            auto cursor = image.begin() + s.VirtualAddress;
            const auto end = cursor + s.Misc.VirtualSize;
            while (cursor != end)
            {
                cursor = std::search(cursor, end, anchor.begin(), anchor.end());
                if (cursor == end)
                    break;
                const auto rva = static_cast<std::uint32_t>(cursor - image.begin());
                if (matches(rva, profile))
                {
                    if (found)
                        return 0;
                    found = rva;
                }
                ++cursor;
            }
        }
        return found;
    }
};
} // namespace GlassFg
