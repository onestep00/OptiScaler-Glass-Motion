#pragma once
#include <cstdint>

namespace GlassFg
{
// Engine-provided element mapping for grouped arrays. The live plugin reads the
// group's 16-bit source-index list while the grouped path builds the draw's
// instance order and publishes it here; the packed identity provider consumes
// it per element. No estimation is involved.
struct GlassArrayMappingEntry
{
    std::uintptr_t proxy = 0;
    std::uint32_t outputStart = 0, count = 0;
    std::uint64_t frame = 0;
    std::uint32_t indices[64] {};
};

void PublishArrayMapping(const GlassArrayMappingEntry& entry) noexcept;
// Packet ordinal -> source element index. UINT32_MAX when unknown.
std::uint32_t LookupArrayMapping(std::uintptr_t proxy, std::uint32_t packetOrdinal) noexcept;
struct GlassArrayMappingStats
{
    std::uint64_t published = 0, replaced = 0, lookups = 0, hits = 0;
    // Split lookup misses: no entry for the proxy versus an ordinal outside the
    // published range. The distinction tells whether the plugin never published
    // this object or published a different output range than the draw uses.
    std::uint64_t misses = 0, outOfRange = 0;
    // Table-full evictions: a live proxy lost its slot to a newly published one.
    std::uint64_t evictions = 0;
    unsigned entries = 0;
};
GlassArrayMappingStats ReadArrayMappingStats() noexcept;
} // namespace GlassFg
