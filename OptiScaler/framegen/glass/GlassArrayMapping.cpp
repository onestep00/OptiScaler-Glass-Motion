#include "pch.h"
#include "GlassArrayMapping.h"
#include <atomic>
#include <cstring>
#include <mutex>

namespace GlassFg
{
namespace
{
// Entries are replaced by proxy, so a live object never accumulates
// duplicates, and an evicted proxy only loses coverage (its elements stay
// unresolved), never correctness. The table must hold the whole live
// population: a 2026-09-23 session had 1,526 live grouped arrays with a pool
// start, and the grouped update republishes an array only when its selection
// changes (about 1 of 580 updates per frame), so a still camera never
// republishes an array that lost its slot. With 256 slots the order of the
// post-load burst (300-400 new arrays) decided whether the spawn view stayed
// resident, and the same scene hit 3% or 99.9% between otherwise identical
// builds.
constexpr unsigned Capacity = 4096;
GlassArrayMappingEntry entries[Capacity] {};
// Slots are replaced but never freed, so the occupied ones are [0, occupied).
unsigned occupied = 0;
// Round-robin eviction cursor, used only once every slot is occupied. Evicting
// a fixed slot kept whichever proxies filled the table first for the whole
// session; a scene whose live arrays were published later then missed every
// lookup (2026-09-23, hits=0 with 1,259 evictions per 1,801 publishes).
unsigned evictionCursor = 0;
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

// Key -> slot indexes over `entries`, guarded by the same mutex, so neither
// the proxy lookup nor the range-only fallback scans the table. Power-of-two
// buckets, at most half full, linear probing. Deletion shifts the rest of the
// probe run back instead of leaving a tombstone, so runs stay as short as the
// resident keys alone make them, however long the eviction churn has run. A
// bucket holds slot + 1 (0 = empty, so the arrays start zeroed); the key is
// read back from the slot's entry.
constexpr unsigned IndexSize = 2 * Capacity, IndexMask = IndexSize - 1;
static_assert((IndexSize & IndexMask) == 0 && Capacity < 0xffff);

// The module's usual 64-bit mix (as in GeometrySourceOwners.h). Heap pointers
// with a fixed allocation stride and dense pool ordinals both spread like
// random keys; plain Fibonacci hashing clustered on some strides (a 0x1c0
// stride averaged 9 probes per hit at 4,096 keys).
unsigned home(std::uint64_t key) noexcept
{
    key ^= key >> 33;
    key *= 0xff51afd7ed558ccdull;
    key ^= key >> 33;
    return static_cast<unsigned>(key) & IndexMask;
}
std::uint64_t proxyKey(unsigned slot) noexcept { return entries[slot].proxy; }
std::uint64_t startKey(unsigned slot) noexcept { return entries[slot].outputStart; }

template <std::uint64_t (*KeyOf)(unsigned)>
struct SlotIndex
{
    std::uint16_t buckets[IndexSize] {};

    // Bucket holding `key`, or IndexSize when no indexed slot has it. The run
    // always ends, because at least half of the buckets are empty.
    unsigned find(std::uint64_t key) const noexcept
    {
        for (unsigned i = home(key); buckets[i]; i = (i + 1) & IndexMask)
            if (KeyOf(buckets[i] - 1u) == key)
                return i;
        return IndexSize;
    }
    unsigned slotAt(unsigned bucket) const noexcept { return buckets[bucket] - 1u; }
    // The slot's key must not be indexed yet.
    void insert(unsigned slot) noexcept
    {
        unsigned i = home(KeyOf(slot));
        while (buckets[i])
            i = (i + 1) & IndexMask;
        buckets[i] = static_cast<std::uint16_t>(slot + 1);
    }
    void erase(unsigned hole) noexcept
    {
        for (unsigned i = (hole + 1) & IndexMask; buckets[i]; i = (i + 1) & IndexMask)
        {
            // A later key moves into the hole when the hole lies on its probe
            // run, i.e. its home bucket is not after the hole.
            if (((i - home(KeyOf(buckets[i] - 1u))) & IndexMask) >= ((i - hole) & IndexMask))
            {
                buckets[hole] = buckets[i];
                hole = i;
            }
        }
        buckets[hole] = 0;
    }
};
SlotIndex<proxyKey> byProxy;
// Keyed by pool start; the bucket holds the head of that start's list below.
SlotIndex<startKey> byStart;
// Entries that claim one pool start, ascending by count. The ranged allocator
// recycles slices, so a newer array can claim the start of an entry that is
// still resident. The range-only candidates for a packet (count <= its count)
// are a prefix of this order, so the fallback reads at most two nodes. Values
// are slot + 1, 0 = none.
std::uint16_t startNext[Capacity] {}, startPrev[Capacity] {};

// The slot's entry must already be written.
void linkStart(unsigned slot) noexcept
{
    const auto& entry = entries[slot];
    const unsigned bucket = byStart.find(entry.outputStart);
    unsigned prev = 0, next = 0;
    if (bucket == IndexSize)
        byStart.insert(slot);
    else
    {
        next = byStart.buckets[bucket];
        while (next && entries[next - 1].count <= entry.count)
        {
            prev = next;
            next = startNext[next - 1];
        }
        if (!prev)
            byStart.buckets[bucket] = static_cast<std::uint16_t>(slot + 1);
    }
    startPrev[slot] = static_cast<std::uint16_t>(prev);
    startNext[slot] = static_cast<std::uint16_t>(next);
    if (prev)
        startNext[prev - 1] = static_cast<std::uint16_t>(slot + 1);
    if (next)
        startPrev[next - 1] = static_cast<std::uint16_t>(slot + 1);
}

// Must run while the slot still holds the entry it was linked with.
void unlinkStart(unsigned slot) noexcept
{
    const unsigned prev = startPrev[slot], next = startNext[slot];
    if (next)
        startPrev[next - 1] = static_cast<std::uint16_t>(prev);
    if (prev)
    {
        startNext[prev - 1] = static_cast<std::uint16_t>(next);
        return;
    }
    const unsigned bucket = byStart.find(entries[slot].outputStart);
    if (next)
        byStart.buckets[bucket] = static_cast<std::uint16_t>(next);
    else
        byStart.erase(bucket);
}

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

// Source index of `ordinal` in a published slice, UINT32_MAX outside the lanes
// the recording wrote (count never exceeds 64, see PublishArrayMapping). The
// locked and cached paths share it, so a cached entry answers exactly as the
// table did.
std::uint32_t mappedIndex(std::uint32_t start, std::uint32_t count, const std::uint32_t* indices,
                          std::uint32_t ordinal) noexcept
{
    return ordinal >= start && ordinal - start < count ? indices[ordinal - start] : UINT32_MAX;
}

// A published entry resolved for one query. `entryFound` is true when an entry
// owns the query, through the consumer's proxy or the range-only branch, even
// if the ordinal fell outside its published count; `rangeOnly` marks a match
// that came from the pool-ordinal range alone.
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
    const unsigned proxyBucket = byProxy.find(proxy);
    if (proxyBucket != IndexSize)
    {
        const auto& entry = entries[byProxy.slotAt(proxyBucket)];
        match.entryFound = true;
        if (ordinal >= entry.outputStart && ordinal - entry.outputStart < entry.count)
        {
            match.entry = &entry;
            return match;
        }
    }
    // The ranged allocator gives every grouped update its own ordinal slice, so
    // a query whose proxy pointer matches nothing can still be identified by an
    // entry whose slice starts at the packet's own transformIndex. The entry may
    // cover only a prefix of the packet when more than 64 elements were
    // appended (the recording cap), so any count up to the packet's is still
    // the same allocation. Two such entries mean the range was recycled and the
    // lookup cannot tell which array owns it, so it fails closed.
    const unsigned startBucket = byStart.find(packetStart);
    if (startBucket == IndexSize)
        return match;
    const unsigned head = byStart.slotAt(startBucket);
    if (entries[head].count > packetCount)
        return match;
    const unsigned second = startNext[head];
    if (second && entries[second - 1].count <= packetCount)
    {
        match.rangeAmbiguous = true;
        return match;
    }
    match.entry = &entries[head];
    match.entryFound = true;
    match.rangeOnly = true;
    return match;
}
} // namespace

void PublishArrayMapping(const GlassArrayMappingEntry& source) noexcept
{
    if (!source.proxy || !source.count)
        return;
    const std::uint32_t count = source.count < 64 ? source.count : 64;
    std::lock_guard lock(mutex);
    if (publishProbeCount < GlassArrayMappingStats::ProbeCapacity)
    {
        auto& probe = publishProbe[publishProbeCount++];
        probe.proxy = source.proxy;
        probe.ordinal = count;
        probe.value = source.outputStart;
        probe.result = 3;
    }
    // Pick the slot and detach it from the indexes it is in: the proxy's own
    // slot keeps its proxy bucket, a free slot has none, an evicted one loses
    // both.
    unsigned slot;
    const unsigned proxyBucket = byProxy.find(source.proxy);
    if (proxyBucket != IndexSize)
    {
        slot = byProxy.slotAt(proxyBucket);
        unlinkStart(slot);
        ++replaced;
    }
    else if (occupied < Capacity)
    {
        slot = occupied++;
        ++published;
    }
    else
    {
        // Table full: replace the slot under the round-robin cursor, so every
        // resident proxy is evicted in turn and the live scene's arrays become
        // resident within one cycle of the table.
        slot = evictionCursor;
        evictionCursor = (evictionCursor + 1) % Capacity;
        byProxy.erase(byProxy.find(entries[slot].proxy));
        unlinkStart(slot);
        ++evictions;
        ++replaced;
    }
    entries[slot] = source;
    entries[slot].count = count;
    if (proxyBucket == IndexSize)
        byProxy.insert(slot);
    linkStart(slot);
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
            const auto index = mappedIndex(cache.entryStart, cache.entryCount, cache.indices, packetOrdinal);
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
    const std::uint32_t index =
        match.entry ? mappedIndex(match.entry->outputStart, match.entry->count, match.entry->indices, packetOrdinal)
                    : UINT32_MAX;
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
    stats.entries = occupied;
    stats.capacity = Capacity;
    stats.publishProbeCount = publishProbeCount;
    stats.lookupProbeCount = lookupProbeCount;
    for (unsigned i = 0; i < publishProbeCount; ++i)
        stats.publishProbe[i] = publishProbe[i];
    for (unsigned i = 0; i < lookupProbeCount; ++i)
        stats.lookupProbe[i] = lookupProbe[i];
    return stats;
}
} // namespace GlassFg
