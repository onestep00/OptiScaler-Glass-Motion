#include "../GlassArrayMapping.h"
#include <cstdio>
#include <stdexcept>
static void check(bool value) { if (!value) throw std::runtime_error("array mapping contract failed"); }
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
        // Fill past the capacity: the oldest slot is evicted and counted.
        for (unsigned i = 0; i < 260; ++i)
        {
            GlassFg::GlassArrayMappingEntry fill;
            fill.proxy = 0x10000 + i;
            fill.outputStart = i;
            fill.count = 1;
            fill.indices[0] = i;
            GlassFg::PublishArrayMapping(fill);
        }
        stats = GlassFg::ReadArrayMappingStats();
        check(stats.entries == 256 && stats.evictions >= 1);
        check(GlassFg::LookupArrayMapping(0x10000 + 259, 259, 259, 1) == 259); // Newest entry stayed.
        puts("ARRAY_MAPPING publish_lookup_replace_clamp_range=pass");
        return 0;
    }
    catch (const std::exception& error)
    {
        std::fprintf(stderr, "%s\n", error.what());
        return 1;
    }
}
