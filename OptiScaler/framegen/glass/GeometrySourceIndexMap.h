#pragma once
#include <array>
#include <bit>
#include <cstdint>
#include <span>

namespace GlassFg
{
// Decode packing only when its source layout changes. Later renderer selection
// composes with this table in constant work. No engine pointers or generations.
// A caller must bind the map to the applied update and a proven owner lifetime.
template<unsigned Capacity = 256> class GeometrySourceIndexMap
{
    static_assert(Capacity > 0 && Capacity <= 65536);
    std::array<std::uint16_t, Capacity> indices; // Only admitted entries are read.
    std::uint32_t first = 0, count = 0;
    bool compacted = false;

  public:
    void reset() noexcept { first = count = 0; compacted = false; }
    bool linear(std::uint32_t sourceFirst, std::uint32_t sourceCount) noexcept
    {
        reset();
        if (!sourceCount || std::uint64_t(sourceFirst) + sourceCount > (std::uint64_t {1} << 32)) return false;
        first = sourceFirst; count = sourceCount; return true;
    }
    bool compact(std::uint32_t sourceFirst, std::uint32_t sourceCount,
                 std::span<const std::uint64_t> selectionMask) noexcept
    {
        reset();
        if (!sourceCount || sourceCount > Capacity || selectionMask.size() < (sourceCount + 63u) / 64u ||
            std::uint64_t(sourceFirst) + sourceCount > (std::uint64_t {1} << 32)) return false;
        unsigned written = 0;
        for (unsigned word = 0; word < (sourceCount + 63u) / 64u; ++word)
        {
            auto bits = selectionMask[word];
            const unsigned remaining = sourceCount - word * 64;
            if (remaining < 64) bits &= (std::uint64_t {1} << remaining) - 1;
            while (bits)
            {
                indices[written++] = static_cast<std::uint16_t>(word * 64 + std::countr_zero(bits));
                bits &= bits - 1;
            }
        }
        if (!written) return false;
        first = sourceFirst; count = written; compacted = true; return true;
    }
    std::uint32_t packedCount() const noexcept { return count; }
    bool originalIndex(std::uint32_t packedIndex, std::uint32_t& output) const noexcept
    {
        output = UINT32_MAX;
        if (packedIndex >= count) return false;
        output = first + (compacted ? indices[packedIndex] : packedIndex);
        return true;
    }
    template<class Selection, class Read>
    bool selectedIndex(const Selection& selection, std::uint32_t ordinal,
                       std::uint32_t& output, Read read) const
    {
        output = UINT32_MAX;
        if (!count || selection.sourceCount != count) return false;
        std::uint32_t packedIndex = UINT32_MAX;
        return selection.originalIndex(ordinal, packedIndex, read) && originalIndex(packedIndex, output);
    }
};
} // namespace GlassFg
