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
    bool linear = false;
    std::uint32_t linearFirst = 0;

    // Storage offsets alone are not original indices: the grouped update path
    // repacks source elements into this same allocation in group order.
    static bool globalStorageRange(bool global, std::uint32_t packetStart, std::uint32_t packetCount,
                                   std::uint32_t ownerStart, std::uint32_t originalCount,
                                   std::uint32_t& first)
    {
        first = UINT32_MAX;
        if (!global || !packetCount || packetCount > 32767 || !originalCount || originalCount > 65536 ||
            ownerStart == UINT32_MAX || std::uint64_t(ownerStart) + originalCount > 131072 ||
            packetStart < ownerStart || packetStart - ownerStart >= originalCount ||
            packetCount > originalCount - (packetStart - ownerStart)) return false;
        first = packetStart - ownerStart; return true;
    }

    // The audited non-grouped path preserves source order. Flags must come
    // from the same owned engine input, not a later snapshot of this address.
    // Array replacement/lifetime still needs an independent history lease.
    bool resolveGlobalPacket(bool global, std::uint32_t packetStart, std::uint32_t packetCount,
                             std::uint32_t ownerStart, std::uint32_t originalCount,
                             std::uint32_t ownerFlags)
    {
        *this = {};
        if (ownerFlags > UINT16_MAX || (ownerFlags & 0x2000) ||
            !globalStorageRange(global, packetStart, packetCount, ownerStart, originalCount, linearFirst))
        { *this = {}; return false; }
        linear = true;
        count = packetCount; sourceCount = originalCount; return true;
    }

    // Only at the audited whole-array call site. Zero span is its explicit
    // descriptor form, not permission to guess missing grouped indices.
    bool resolveLinear(const Descriptor& descriptor, std::uint32_t originalCount,
                       std::uint32_t ownerGlobalStart)
    {
        *this = {};
        if (!originalCount || originalCount > 65536 || descriptor.count != originalCount ||
            descriptor.first || descriptor.begin || descriptor.end ||
            ownerGlobalStart == UINT32_MAX || descriptor.globalStart != ownerGlobalStart ||
            std::uint64_t(ownerGlobalStart) + originalCount > (std::uint64_t {1} << 32))
            return false;
        count = sourceCount = originalCount; linear = true;
        return true;
    }

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
        if (linear)
        {
            if (ordinal >= count) return false;
            output = linearFirst + ordinal;
            return true;
        }
        if (!sourceIndices || ordinal >= count) return false;
        std::uint16_t index = 0;
        if (!read(sourceIndices + std::uint64_t(ordinal) * 2, &index, 2) || index >= sourceCount)
            return false;
        output = index;
        return true;
    }
};
}
