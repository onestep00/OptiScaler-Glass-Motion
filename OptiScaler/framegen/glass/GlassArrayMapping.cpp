#include "pch.h"
#include "GlassArrayMapping.h"
#include <atomic>
#include <cstring>
#include <mutex>

namespace GlassFg
{
namespace
{
// Bounded, frame-scoped table. Entries are replaced by proxy, so a live object
// never accumulates duplicates. The scene can show more than 64 grouped arrays
// at once, and an evicted proxy only loses coverage (its elements stay
// unresolved), never correctness.
constexpr unsigned Capacity = 256;
GlassArrayMappingEntry entries[Capacity] {};
std::mutex mutex;
std::atomic<std::uint64_t> published { 0 }, replaced { 0 }, lookups { 0 }, hits { 0 }, misses { 0 }, outOfRange { 0 },
    evictions { 0 };
// First-N probe records, guarded by the same mutex. Bounded and diagnostic only.
unsigned publishProbeCount = 0, lookupProbeCount = 0;
GlassArrayMappingStats::Probe publishProbe[GlassArrayMappingStats::ProbeCapacity] {};
GlassArrayMappingStats::Probe lookupProbe[GlassArrayMappingStats::ProbeCapacity] {};

std::uint32_t lookupUnlocked(std::uintptr_t proxy, std::uint32_t ordinal, bool& entryFound) noexcept
{
    for (const auto& entry : entries)
    {
        if (entry.proxy != proxy || !entry.count)
            continue;
        entryFound = true;
        if (ordinal < entry.outputStart || ordinal >= entry.outputStart + entry.count)
            continue;
        const auto offset = ordinal - entry.outputStart;
        return offset < 64 ? entry.indices[offset] : UINT32_MAX;
    }
    return UINT32_MAX;
}
} // namespace

void PublishArrayMapping(const GlassArrayMappingEntry& source) noexcept
{
    if (!source.proxy || !source.count)
        return;
    GlassArrayMappingEntry entry = source;
    if (entry.count > 64)
        entry.count = 64;
    std::lock_guard lock(mutex);
    if (publishProbeCount < GlassArrayMappingStats::ProbeCapacity)
    {
        auto& probe = publishProbe[publishProbeCount++];
        probe.proxy = entry.proxy;
        probe.ordinal = entry.count;
        probe.value = entry.outputStart;
        probe.result = 3;
    }
    for (auto& slot : entries)
    {
        if (slot.proxy == entry.proxy && slot.count)
        {
            slot = entry;
            ++replaced;
            return;
        }
    }
    for (auto& slot : entries)
    {
        if (!slot.count)
        {
            slot = entry;
            ++published;
            return;
        }
    }
    // Table full: replace the oldest proxy slot (first entry).
    entries[0] = entry;
    ++evictions;
    ++replaced;
}

std::uint32_t LookupArrayMapping(std::uintptr_t proxy, std::uint32_t packetOrdinal) noexcept
{
    ++lookups;
    std::lock_guard lock(mutex);
    bool entryFound = false;
    const auto index = lookupUnlocked(proxy, packetOrdinal, entryFound);
    if (lookupProbeCount < GlassArrayMappingStats::ProbeCapacity)
    {
        auto& probe = lookupProbe[lookupProbeCount++];
        probe.proxy = proxy;
        probe.ordinal = packetOrdinal;
        probe.value = index;
        probe.result = index != UINT32_MAX ? 1u : entryFound ? 2u : 0u;
    }
    if (index != UINT32_MAX)
        ++hits;
    else if (entryFound)
        ++outOfRange;
    else
        ++misses;
    return index;
}

GlassArrayMappingStats ReadArrayMappingStats() noexcept
{
    std::lock_guard lock(mutex);
    GlassArrayMappingStats stats;
    stats.published = published.load();
    stats.replaced = replaced.load();
    stats.lookups = lookups.load();
    stats.hits = hits.load();
    stats.misses = misses.load();
    stats.outOfRange = outOfRange.load();
    stats.evictions = evictions.load();
    for (const auto& entry : entries)
        if (entry.count)
            ++stats.entries;
    stats.publishProbeCount = publishProbeCount;
    stats.lookupProbeCount = lookupProbeCount;
    for (unsigned i = 0; i < publishProbeCount; ++i)
        stats.publishProbe[i] = publishProbe[i];
    for (unsigned i = 0; i < lookupProbeCount; ++i)
        stats.lookupProbe[i] = lookupProbe[i];
    return stats;
}
} // namespace GlassFg
