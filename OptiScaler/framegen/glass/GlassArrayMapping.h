#pragma once
#include <cstdint>

namespace GlassFg
{
// Engine-provided element mapping for grouped arrays. The grouped-update hook
// (CyberpunkGroups.cpp) reads the array's 48-byte element base and pool start,
// and the element append passes the source pointer, so each pool ordinal is
// paired with its original source index without estimation. The motion
// identity provider queries this table for the grouped-array branch
// (orderKind == 2) and keys its history by the recovered source index.
struct GlassArrayMappingEntry
{
    std::uintptr_t proxy = 0;
    std::uint32_t outputStart = 0, count = 0;
    std::uint64_t frame = 0;
    std::uint32_t indices[64] {};
};

void PublishArrayMapping(const GlassArrayMappingEntry& entry) noexcept;
// Packet ordinal -> source element index. UINT32_MAX when unknown.
// `packetStart`/`packetCount` are the draw packet's own pool range
// (transformIndex and count). They are only used when the proxy pointer does
// not match any published entry: an entry whose range equals the packet's
// range exactly is the same array, because the ranged allocator hands each
// grouped update its own slice.
std::uint32_t LookupArrayMapping(std::uintptr_t proxy, std::uint32_t packetOrdinal, std::uint32_t packetStart,
                                 std::uint32_t packetCount) noexcept;
struct GlassArrayMappingStats
{
    std::uint64_t published = 0, replaced = 0, lookups = 0, hits = 0;
    // Hits that only the pool-ordinal range could resolve because the consumer
    // queried a different proxy pointer than the update published. The match
    // requires the entry's slice to equal the packet's own [transformIndex,
    // +count) range exactly, so the counter also shows whether the proxy domain
    // agrees between the hook and the draw packet. rangeAmbiguous counts
    // recycled ranges that two published entries claim, which fail closed.
    std::uint64_t rangeHits = 0, rangeAmbiguous = 0;
    // Split lookup misses: no entry for the proxy versus an ordinal outside the
    // published range. The distinction tells whether the plugin never published
    // this object or published a different output range than the draw uses.
    std::uint64_t misses = 0, outOfRange = 0;
    // Table-full evictions: a live proxy lost its slot to a newly published one.
    std::uint64_t evictions = 0;
    unsigned entries = 0;
    // Bounded diagnostics: the first few published and queried keys, so a live
    // session can show whether the module queries the proxy and ordinal range
    // the plugin actually published. Never grows past these capacities.
    struct Probe
    {
        std::uintptr_t proxy = 0;
        std::uint32_t ordinal = 0, value = 0;
        // 0 = miss (no entry for this proxy), 1 = hit, 2 = ordinal outside the
        // published range, 3 = published entry (value = outputStart, ordinal =
        // count), 4 = hit resolved by the pool-ordinal range alone.
        unsigned result = 0;
    };
    static constexpr unsigned ProbeCapacity = 12;
    unsigned publishProbeCount = 0, lookupProbeCount = 0;
    Probe publishProbe[ProbeCapacity] {};
    Probe lookupProbe[ProbeCapacity] {};
};
GlassArrayMappingStats ReadArrayMappingStats() noexcept;
} // namespace GlassFg
