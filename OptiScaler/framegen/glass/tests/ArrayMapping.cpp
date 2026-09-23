#include "../GlassArrayMapping.h"
#include <cstdio>
#include <source_location>
#include <stdexcept>
#include <string>
static void check(bool value, std::source_location where = std::source_location::current())
{
    if (!value)
        throw std::runtime_error("array mapping contract failed at line " + std::to_string(where.line()));
}
// Hash-index case: pointer-like proxies (aligned like the engine's heap
// objects), each with its own disjoint slice of 1 to 8 elements and unique
// source indices, so a lookup can only resolve through its own entry.
static GlassFg::GlassArrayMappingEntry liveEntry(unsigned i)
{
    GlassFg::GlassArrayMappingEntry entry;
    entry.proxy = 0x40000000 + std::uintptr_t(i) * 0x1c0;
    entry.outputStart = 0x10000 + i * 8;
    entry.count = 1 + i % 8;
    for (unsigned lane = 0; lane < entry.count; ++lane)
        entry.indices[lane] = 100000 + i * 8 + lane;
    return entry;
}
// Every lane of `entry`, queried through `proxy` with the entry's own packet range.
static bool resolves(const GlassFg::GlassArrayMappingEntry& entry, std::uintptr_t proxy)
{
    for (unsigned lane = 0; lane < entry.count; ++lane)
        if (GlassFg::LookupArrayMapping(proxy, entry.outputStart + lane, entry.outputStart, entry.count) !=
            entry.indices[lane])
            return false;
    return true;
}
// Residency case: one lane per fresh proxy, each with its own pool slice and
// source index, disjoint from every other publication in this test, so a
// lookup can only resolve through its own entry for any capacity below 0x10000.
static GlassFg::GlassArrayMappingEntry freshEntry(unsigned batch, unsigned i)
{
    GlassFg::GlassArrayMappingEntry entry;
    entry.proxy = 0x20000000 + std::uintptr_t(batch) * 0x100000 + std::uintptr_t(i) * 0x10;
    entry.outputStart = 0x100000 + batch * 0x10000 + i;
    entry.count = 1;
    entry.indices[0] = 0x200000 + batch * 0x10000 + i;
    return entry;
}
static std::uint32_t lookupFresh(unsigned batch, unsigned i)
{
    const auto entry = freshEntry(batch, i);
    return GlassFg::LookupArrayMapping(entry.proxy, entry.outputStart, entry.outputStart, entry.count);
}
int main()
{
    try
    {
        GlassFg::GlassArrayMappingEntry entry;
        entry.proxy = 0x1000;
        entry.outputStart = 10;
        entry.count = 3;
        entry.indices[0] = 7;
        entry.indices[1] = 9;
        entry.indices[2] = 42;
        GlassFg::PublishArrayMapping(entry);
        check(GlassFg::LookupArrayMapping(0x1000, 10, 10, 3) == 7);
        check(GlassFg::LookupArrayMapping(0x1000, 11, 10, 3) == 9);
        check(GlassFg::LookupArrayMapping(0x1000, 12, 10, 3) == 42);
        check(GlassFg::LookupArrayMapping(0x1000, 13, 10, 3) == UINT32_MAX);   // Outside the queried packet range.
        // The grouped update and the draw packet can report different proxy
        // pointers for the same allocation. The pool-ordinal slice still
        // identifies it, because the ranged allocator hands each grouped update
        // its own [transformIndex, +count) range.
        check(GlassFg::LookupArrayMapping(0x2000, 10, 10, 3) == 7);
        entry.indices[1] = 11;
        GlassFg::PublishArrayMapping(entry);                            // Same proxy replaces.
        check(GlassFg::LookupArrayMapping(0x1000, 11, 10, 3) == 11);
        auto stats = GlassFg::ReadArrayMappingStats();
        check(stats.entries == 1 && stats.published == 1 && stats.replaced == 1);
        check(stats.lookups == 6 && stats.hits == 4 && stats.misses == 1 && stats.rangeHits == 1);
        check(stats.outOfRange == 0 && stats.rangeAmbiguous == 0);
        GlassFg::GlassArrayMappingEntry big;
        big.proxy = 0x3000;
        big.count = 4096;                                               // Clamped to 64 lanes.
        GlassFg::PublishArrayMapping(big);
        check(GlassFg::LookupArrayMapping(0x3000, 0, 0, 64) == 0);
        check(GlassFg::LookupArrayMapping(0x3000, 63, 0, 64) == 0);
        check(GlassFg::LookupArrayMapping(0x3000, 64, 0, 64) == UINT32_MAX);  // Past the clamped packet range.
        GlassFg::PublishArrayMapping(GlassFg::GlassArrayMappingEntry {}); // Rejected.
        stats = GlassFg::ReadArrayMappingStats();
        check(stats.entries == 2 && stats.published == 2);
        // A packet with more elements than the 64-lane recording cap: the entry
        // covers the prefix, so an element past the cap must stay unknown
        // instead of replaying a lane the recording never wrote.
        GlassFg::GlassArrayMappingEntry capped;
        capped.proxy = 0x4000;
        capped.outputStart = 200;
        capped.count = 100;
        for (unsigned lane = 0; lane < 64; ++lane) capped.indices[lane] = 500 + lane;
        GlassFg::PublishArrayMapping(capped);
        check(GlassFg::LookupArrayMapping(0x4000, 200, 200, 100) == 500);
        check(GlassFg::LookupArrayMapping(0x4000, 263, 200, 100) == 563);
        check(GlassFg::LookupArrayMapping(0x4000, 264, 200, 100) == UINT32_MAX);
        stats = GlassFg::ReadArrayMappingStats();
        check(stats.entries == 3 && stats.published == 3);
        check(stats.lookups == 12 && stats.hits == 8 && stats.misses == 2 && stats.outOfRange == 1);
        // A recycled pool slice claimed by two live entries cannot be told
        // apart, so the range-only branch fails closed instead of guessing.
        GlassFg::GlassArrayMappingEntry recycled;
        recycled.proxy = 0x5000;
        recycled.outputStart = 10;
        recycled.count = 3;
        recycled.indices[0] = 70;
        GlassFg::PublishArrayMapping(recycled);
        check(GlassFg::LookupArrayMapping(0x6000, 10, 10, 3) == UINT32_MAX);
        stats = GlassFg::ReadArrayMappingStats();
        check(stats.entries == 4 && stats.published == 4 && stats.rangeAmbiguous == 1);
        check(stats.lookups == 13 && stats.hits == 8 && stats.rangeHits == 1);
        // More arrays resident at once than the old 256-slot table held. A live
        // session had 1,526, and a still camera never republishes them, so each
        // must stay resolvable through its own proxy.
        constexpr unsigned live = 1500;
        for (unsigned i = 0; i < live; ++i) GlassFg::PublishArrayMapping(liveEntry(i));
        std::uint64_t lanes = 0;
        for (unsigned i = 0; i < live; ++i)
        {
            check(resolves(liveEntry(i), liveEntry(i).proxy));
            lanes += liveEntry(i).count;
        }
        stats = GlassFg::ReadArrayMappingStats();
        check(stats.entries == 4 + live && stats.published == 4 + live && stats.evictions == 0);
        // Proxy hits, not range-only ones: every slice here is unique, so the
        // range fallback alone would also answer.
        check(stats.hits == 8 + lanes && stats.rangeHits == 1);
        // Range-only fallback on the large table: a proxy pointer the update
        // never published resolves through the packet's own slice. A packet
        // longer than the entry leaves the lanes the recording never wrote
        // unknown, as the reader cache already did.
        std::uint64_t rangeLanes = 0;
        for (const unsigned i : {0u, 777u, live - 1})
        {
            const auto target = liveEntry(i);
            const std::uintptr_t foreign = 0x70000000 + std::uintptr_t(i) * 0x40;
            check(resolves(target, foreign));
            check(GlassFg::LookupArrayMapping(foreign, target.outputStart + target.count, target.outputStart,
                                              target.count + 1) == UINT32_MAX);
            rangeLanes += target.count;
        }
        stats = GlassFg::ReadArrayMappingStats();
        check(stats.rangeHits == 1 + rangeLanes && stats.outOfRange == 1 + 3 && stats.hits == 8 + lanes);
        // Two resident arrays claim one recycled slice. Only an entry that fits
        // the packet's count is a candidate: the shorter one answers a short
        // packet, and a packet both fit fails closed. The shorter one is
        // published second, so it must take the head of that slice.
        GlassFg::GlassArrayMappingEntry longer, shorter;
        longer.proxy = 0x60000000;
        shorter.proxy = 0x60000040;
        longer.outputStart = shorter.outputStart = 0x8000;
        longer.count = 5;
        shorter.count = 2;
        for (unsigned lane = 0; lane < longer.count; ++lane) longer.indices[lane] = 900 + lane;
        for (unsigned lane = 0; lane < shorter.count; ++lane) shorter.indices[lane] = 950 + lane;
        GlassFg::PublishArrayMapping(longer);
        GlassFg::PublishArrayMapping(shorter);
        check(GlassFg::LookupArrayMapping(0x61000000, 0x8001, 0x8000, 2) == 951);
        check(GlassFg::LookupArrayMapping(0x61000040, 0x8001, 0x8000, 5) == UINT32_MAX);
        // The shorter array moves to another slice. The old slice then belongs
        // to the longer array alone, which a short packet still cannot claim.
        shorter.outputStart = 0x9000;
        GlassFg::PublishArrayMapping(shorter);
        check(GlassFg::LookupArrayMapping(0x61000080, 0x8004, 0x8000, 5) == 904);
        check(GlassFg::LookupArrayMapping(0x610000c0, 0x8001, 0x8000, 2) == UINT32_MAX);
        check(GlassFg::LookupArrayMapping(0x61000100, 0x9001, 0x9000, 2) == 951);
        stats = GlassFg::ReadArrayMappingStats();
        check(stats.entries == 4 + live + 2 && stats.rangeAmbiguous == 2);
        // Past the capacity: round-robin eviction takes the oldest slots, which
        // hold everything published so far. Every newer array still resolves
        // through its own proxy after that many index deletions, and the evicted
        // ones resolve through neither their proxy nor their slice.
        const unsigned capacity = stats.capacity;
        const unsigned residentBefore = stats.entries;
        const auto hitsBefore = stats.hits, rangeHitsBefore = stats.rangeHits;
        for (unsigned i = live; i < live + capacity; ++i) GlassFg::PublishArrayMapping(liveEntry(i));
        lanes = 0;
        for (unsigned i = live; i < live + capacity; ++i)
        {
            check(resolves(liveEntry(i), liveEntry(i).proxy));
            lanes += liveEntry(i).count;
        }
        for (unsigned i = 0; i < live; ++i)
        {
            const auto evicted = liveEntry(i);
            check(GlassFg::LookupArrayMapping(evicted.proxy, evicted.outputStart, evicted.outputStart, evicted.count) ==
                  UINT32_MAX);
        }
        stats = GlassFg::ReadArrayMappingStats();
        check(stats.entries == capacity && stats.evictions == residentBefore);
        check(stats.hits == hitsBefore + lanes && stats.rangeHits == rangeHitsBefore);
        // A full table must keep serving the newest publications. Evicting one
        // fixed slot let each new proxy stay only until the next publication, so
        // every array but the latest one published after the table filled missed.
        const auto evictionsBefore = stats.evictions;
        for (unsigned i = 0; i < capacity; ++i) GlassFg::PublishArrayMapping(freshEntry(0, i));
        for (unsigned i = 0; i < capacity; ++i) check(lookupFresh(0, i) == freshEntry(0, i).indices[0]);
        // A second full batch wraps the eviction cursor again and evicts the
        // first one whole. The evicted batch is queried first, while the reader
        // cache still holds its answers from before the evictions.
        for (unsigned i = 0; i < capacity; ++i) GlassFg::PublishArrayMapping(freshEntry(1, i));
        for (unsigned i = 0; i < capacity; ++i) check(lookupFresh(0, i) == UINT32_MAX);
        for (unsigned i = 0; i < capacity; ++i) check(lookupFresh(1, i) == freshEntry(1, i).indices[0]);
        stats = GlassFg::ReadArrayMappingStats();
        check(stats.entries == capacity && stats.evictions == evictionsBefore + 2 * capacity);
        std::printf("ARRAY_MAPPING publish_lookup_replace_clamp_range=pass hash_index=pass range_fallback=pass "
                    "evict_past_capacity=pass wraparound=pass capacity=%u\n",
                    capacity);
        return 0;
    }
    catch (const std::exception& error)
    {
        std::fprintf(stderr, "%s\n", error.what());
        return 1;
    }
}
