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
        check(GlassFg::LookupArrayMapping(0x1000, 10) == 7);
        check(GlassFg::LookupArrayMapping(0x1000, 11) == 9);
        check(GlassFg::LookupArrayMapping(0x1000, 12) == 42);
        check(GlassFg::LookupArrayMapping(0x1000, 13) == UINT32_MAX);   // Past the entry.
        check(GlassFg::LookupArrayMapping(0x2000, 10) == UINT32_MAX);   // Unknown proxy.
        entry.indices[1] = 11;
        GlassFg::PublishArrayMapping(entry);                            // Same proxy replaces.
        check(GlassFg::LookupArrayMapping(0x1000, 11) == 11);
        auto stats = GlassFg::ReadArrayMappingStats();
        check(stats.entries == 1 && stats.published == 1 && stats.replaced == 1 && stats.hits == 4);
        GlassFg::GlassArrayMappingEntry big;
        big.proxy = 0x3000;
        big.count = 4096;                                               // Clamped to 64 lanes.
        GlassFg::PublishArrayMapping(big);
        check(GlassFg::LookupArrayMapping(0x3000, 63) == 0);
        check(GlassFg::LookupArrayMapping(0x3000, 64) == UINT32_MAX);
        GlassFg::PublishArrayMapping(GlassFg::GlassArrayMappingEntry {}); // Rejected.
        stats = GlassFg::ReadArrayMappingStats();
        check(stats.entries == 2 && stats.published == 2);
        // Eight lookups: five hits, one unknown-proxy miss, two out-of-range.
        check(stats.lookups == 8 && stats.hits == 5 && stats.misses == 1 && stats.outOfRange == 2);
        puts("ARRAY_MAPPING publish_lookup_replace_clamp=pass");
        return 0;
    }
    catch (const std::exception& error)
    {
        std::fprintf(stderr, "%s\n", error.what());
        return 1;
    }
}
