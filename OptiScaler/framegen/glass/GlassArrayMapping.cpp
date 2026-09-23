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
    evictions { 0 }, rangeHits { 0 }, rangeAmbiguous { 0 };
// Bumped on every table mutation. The table is immutable while the generation
// is unchanged, which lets the reader path below skip the lock.
std::atomic<std::uint64_t> publishGeneration { 0 };
// First-N probe records, guarded by the same mutex. Bounded and diagnostic only.
unsigned publishProbeCount = 0, lookupProbeCount = 0;
GlassArrayMappingStats::Probe publishProbe[GlassArrayMappingStats::ProbeCapacity] {};
GlassArrayMappingStats::Probe lookupProbe[GlassArrayMappingStats::ProbeCapacity] {};

// One grouped array is queried once per element draw, so a per-thread copy of
// the last few looked-up arrays removes almost all lock acquisitions from the
// identity path. The copy is only trusted while the publish generation is
// unchanged; a stale copy can never be read as a newer mapping.
struct ReaderCache
{
    std::uint64_t generation = 0;
    std::uintptr_t proxy = 0;
    // The published slice the copied indices belong to, and the packet range
    // the query was validated for. A proxy match does not require the two to be
    // equal, so both are kept.
    std::uint32_t entryStart = 0, entryCount = 0;
    std::uint32_t packetStart = 0, packetCount = 0;
    std::uint32_t indices[64] {};
    bool rangeOnly = false;
};
constexpr unsigned ReaderCacheWays = 8;
thread_local ReaderCache readerCache[ReaderCacheWays];

std::uint32_t cachedIndex(const ReaderCache& cache, std::uint32_t ordinal) noexcept
{
    if (ordinal < cache.entryStart || ordinal - cache.entryStart >= cache.entryCount)
        return UINT32_MAX;
    const auto offset = ordinal - cache.entryStart;
    return offset < 64 ? cache.indices[offset] : UINT32_MAX;
}

// A published entry resolved for one query. `entryFound` stays true when the
// consumer's proxy was published at all, even if the ordinal fell outside its
// range; `rangeOnly` marks a match that came from the pool-ordinal range alone.
struct Match
{
    const GlassArrayMappingEntry* entry = nullptr;
    bool entryFound = false;
    bool rangeOnly = false;
    bool rangeAmbiguous = false;
};

Match findUnlocked(std::uintptr_t proxy, std::uint32_t ordinal, std::uint32_t packetStart,
                   std::uint32_t packetCount) noexcept
{
    Match match;
    for (const auto& entry : entries)
    {
        if (entry.proxy != proxy || !entry.count)
            continue;
        match.entryFound = true;
        if (ordinal < entry.outputStart || ordinal >= entry.outputStart + entry.count)
            continue;
        match.entry = &entry;
        return match;
    }
    // The ranged allocator gives every grouped update its own ordinal slice, so
    // a query whose proxy pointer matches nothing can still be identified by an
    // entry whose slice equals the packet's own [transformIndex, +count) range
    // exactly. Two such entries mean the range was recycled and the lookup
    // cannot tell which array owns it, so it fails closed.
    if (packetCount == 0)
        return match;
    for (const auto& entry : entries)
    {
        if (!entry.count)
            continue;
        // The entry may cover only a prefix of the packet when more than 64
        // elements were appended (the recording cap), so a subset is still the
        // same allocation. A recycled slice that two entries claim fails closed
        // through the ambiguity branch below.
        if (entry.outputStart != packetStart || entry.count > packetCount)
            continue;
        if (match.entry)
        {
            match.entry = nullptr;
            match.rangeAmbiguous = true;
            return match;
        }
        match.entry = &entry;
        match.rangeOnly = true;
    }
    return match;
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
            publishGeneration.fetch_add(1, std::memory_order_release);
            return;
        }
    }
    for (auto& slot : entries)
    {
        if (!slot.count)
        {
            slot = entry;
            ++published;
            publishGeneration.fetch_add(1, std::memory_order_release);
            return;
        }
    }
    // Table full: replace the oldest proxy slot (first entry).
    entries[0] = entry;
    ++evictions;
    ++replaced;
    publishGeneration.fetch_add(1, std::memory_order_release);
}

std::uint32_t LookupArrayMapping(std::uintptr_t proxy, std::uint32_t packetOrdinal, std::uint32_t packetStart,
                                 std::uint32_t packetCount) noexcept
{
    lookups.fetch_add(1, std::memory_order_relaxed);
    if (packetOrdinal < packetStart || packetOrdinal - packetStart >= packetCount)
    {
        // The caller passed a packet it cannot own; never answer from a cache
        // that was filled for a different range.
        misses.fetch_add(1, std::memory_order_relaxed);
        return UINT32_MAX;
    }
    const std::uint64_t generation = publishGeneration.load(std::memory_order_acquire);
    if (generation != 0)
    {
        auto& cache = readerCache[(proxy >> 4) & (ReaderCacheWays - 1)];
        if (cache.generation == generation && cache.proxy == proxy && cache.packetStart == packetStart &&
            cache.packetCount == packetCount)
        {
            const auto index = cachedIndex(cache, packetOrdinal);
            if (index != UINT32_MAX)
            {
                if (cache.rangeOnly)
                    rangeHits.fetch_add(1, std::memory_order_relaxed);
                else
                    hits.fetch_add(1, std::memory_order_relaxed);
            }
            else
                outOfRange.fetch_add(1, std::memory_order_relaxed);
            return index;
        }
    }
    std::lock_guard lock(mutex);
    const auto match = findUnlocked(proxy, packetOrdinal, packetStart, packetCount);
    std::uint32_t index = UINT32_MAX;
    if (match.entry)
    {
        const auto offset = packetOrdinal - match.entry->outputStart;
        index = offset < 64 ? match.entry->indices[offset] : UINT32_MAX;
    }
    if (lookupProbeCount < GlassArrayMappingStats::ProbeCapacity)
    {
        auto& probe = lookupProbe[lookupProbeCount++];
        probe.proxy = proxy;
        probe.ordinal = packetOrdinal;
        probe.value = index;
        probe.result = index != UINT32_MAX ? (match.rangeOnly ? 4u : 1u) : match.entryFound ? 2u : 0u;
    }
    if (index != UINT32_MAX)
    {
        if (match.rangeOnly)
            rangeHits.fetch_add(1, std::memory_order_relaxed);
        else
            hits.fetch_add(1, std::memory_order_relaxed);
    }
    else if (match.rangeAmbiguous)
        rangeAmbiguous.fetch_add(1, std::memory_order_relaxed);
    else if (match.entryFound)
        outOfRange.fetch_add(1, std::memory_order_relaxed);
    else
        misses.fetch_add(1, std::memory_order_relaxed);
    // The publisher bumps the generation while holding this lock, so the value
    // read here belongs to exactly the table state that was just queried.
    const std::uint64_t lockedGeneration = publishGeneration.load(std::memory_order_acquire);
    if (match.entry && lockedGeneration != 0)
    {
        // Cache the entry under the same lock and generation that produced the
        // answer, so the fast path can only replay this exact table state. The
        // cache is keyed by the queried proxy, which may differ from the
        // published one in the range-only case.
        auto& cache = readerCache[(proxy >> 4) & (ReaderCacheWays - 1)];
        cache.generation = lockedGeneration;
        cache.proxy = proxy;
        cache.entryStart = match.entry->outputStart;
        cache.entryCount = match.entry->count;
        cache.packetStart = packetStart;
        cache.packetCount = packetCount;
        cache.rangeOnly = match.rangeOnly;
        std::memcpy(cache.indices, match.entry->indices, sizeof(cache.indices));
    }
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
    stats.rangeHits = rangeHits.load();
    stats.rangeAmbiguous = rangeAmbiguous.load();
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
