#pragma once
#include "GeometryInstance.h"
#include <span>

namespace GlassFg
{
// Caller supplies current, conservative geometry bounds in the capture viewport.
// This packs masks only; it neither derives bounds nor admits temporal identity.
struct GeometryCoverageRegion
{
    std::uint32_t left = 0, top = 0, width = 0, height = 0, generation = 0;
};

// No allocation. On failure output and usedWords remain unchanged. Inactive
// instances are exact-zero regions and preserve their place in the draw.
inline bool PackGeometryCoverage(std::span<const GeometryCoverageRegion> regions,
                                 std::span<GeometryInstance> output,
                                 std::uint32_t viewportWidth, std::uint32_t viewportHeight,
                                 std::uint64_t capacityWords, std::uint32_t& usedWords)
{
    if (output.size() != regions.size() || !viewportWidth || !viewportHeight ||
        viewportWidth > 32768 || viewportHeight > 32768 || capacityWords > UINT32_MAX / 32)
        return false;
    std::uint64_t total = 0;
    for (const auto& r : regions)
    {
        if (!(r.left | r.top | r.width | r.height | r.generation)) continue;
        if (!r.generation || !r.width || !r.height ||
            std::uint64_t(r.left) + r.width > viewportWidth ||
            std::uint64_t(r.top) + r.height > viewportHeight) return false;
        const auto words = 1 + (std::uint64_t(r.width) * r.height + 31) / 32;
        if (words > capacityWords - total) return false;
        total += words;
    }
    std::uint32_t cursor = 0;
    for (std::size_t i = 0; i < regions.size(); ++i)
    {
        const auto& r = regions[i];
        if (!r.generation) { output[i] = {}; continue; }
        output[i] = { 0, 0, 0, r.generation, r.left, r.top, r.width, r.height,
                      (cursor + 1) * 32, r.width, std::uint32_t(total * 32), cursor };
        cursor += std::uint32_t(1 + (std::uint64_t(r.width) * r.height + 31) / 32);
    }
    usedWords = std::uint32_t(total);
    return true;
}
} // namespace GlassFg
