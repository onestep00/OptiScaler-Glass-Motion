#pragma once
#include <cstdint>

namespace GlassFg
{
// Callback-scoped view of the current engine group, obtained by the producer
// callback. Never search group tables on the draw path. A borrowed index is not
// persistent identity without independently verified owner/array lifetime.
struct CyberpunkInstanceSelection
{
    struct Descriptor
    {
        std::uint32_t globalStart, count, first, unused;
        std::uint64_t begin, end;
    };
    static_assert(sizeof(Descriptor) == 32);
    std::uint64_t sourceIndices = 0;
    std::uint32_t count = 0, sourceCount = 0;

    template <typename Read>
    bool resolve(std::uint64_t group, const Descriptor& descriptor,
                 std::uint32_t originalCount, Read read)
    {
        *this = {};
        if (!group || group > UINT64_MAX - 0x58 || !descriptor.count ||
            !originalCount || originalCount > 65536 || !descriptor.begin ||
            descriptor.end < descriptor.begin || (descriptor.end - descriptor.begin) % 48 ||
            std::uint64_t(descriptor.first) + descriptor.count > (descriptor.end - descriptor.begin) / 48)
            return false;
        std::uint64_t indices = 0;
        std::uint32_t length = 0, global = 0;
        if (!read(group + 0x18, &indices, 8) || !read(group + 0x28, &length, 4) ||
            !read(group + 0x50, &global, 4) || !indices || length != descriptor.count ||
            global != descriptor.globalStart || length > originalCount ||
            indices > UINT64_MAX - std::uint64_t(length) * 2)
            return false;
        sourceIndices = indices; count = length; sourceCount = originalCount;
        return true;
    }

    template <typename Read>
    bool originalIndex(std::uint32_t ordinal, std::uint32_t& output, Read read) const
    {
        output = UINT32_MAX;
        if (!sourceIndices || ordinal >= count) return false;
        std::uint16_t index = 0;
        if (!read(sourceIndices + std::uint64_t(ordinal) * 2, &index, 2) || index >= sourceCount)
            return false;
        output = index;
        return true;
    }
};
}
