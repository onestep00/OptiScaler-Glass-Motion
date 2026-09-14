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
// never accumulates duplicates.
constexpr unsigned Capacity = 64;
GlassArrayMappingEntry entries[Capacity] {};
std::mutex mutex;
std::atomic<std::uint64_t> published { 0 }, replaced { 0 }, lookups { 0 }, hits { 0 }, misses { 0 }, outOfRange { 0 };

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
    ++replaced;
}

std::uint32_t LookupArrayMapping(std::uintptr_t proxy, std::uint32_t packetOrdinal) noexcept
{
    ++lookups;
    std::lock_guard lock(mutex);
    bool entryFound = false;
    const auto index = lookupUnlocked(proxy, packetOrdinal, entryFound);
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
    for (const auto& entry : entries)
        if (entry.count)
            ++stats.entries;
    return stats;
}
} // namespace GlassFg
