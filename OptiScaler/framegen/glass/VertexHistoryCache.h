#pragma once
#include "GeometryDrawBatch.h"
#include <algorithm>
#include <array>
#include <bit>
#include <cstdint>
#include <limits>

namespace GlassFg
{
// A frame-local draw/instance ordinal, resource address or array position is
// never sufficient identity. The adapter must establish all of these domains.
struct VertexHistoryKey
{
    GeometryDrawIdentity object;
    std::uint64_t view = 0, pipeline = 0, topology = 0;
    std::uint32_t chunk = 0, vertexFactory = 0;
    bool operator==(const VertexHistoryKey& b) const
    {
        return object.proxy == b.object.proxy && object.mesh == b.object.mesh &&
               object.slot == b.object.slot && object.generation == b.object.generation &&
               view == b.view && pipeline == b.pipeline && topology == b.topology &&
               chunk == b.chunk && vertexFactory == b.vertexFactory;
    }
    explicit operator bool() const { return bool(object) && view && pipeline && topology; }
};

struct VertexHistoryAllocation
{
    std::uint32_t base = 0, vertices = 0, capacity = 0, generation = 0;
    explicit operator bool() const { return generation != 0; }
};

// CPU metadata for a fixed GPU vertex arena. No allocation, locks, GPU calls or
// waits after construction. The owner serializes access. Retirement is an
// explicit *contiguous GPU-completed and recording-discarded* frame watermark,
// not the CPU render counter and not a single completed fence from another view.
template<unsigned Sets = 4096, unsigned Ways = 4, unsigned Pages = 4096, unsigned PageVertices = 128>
class VertexHistoryCache
{
    static_assert(std::has_single_bit(Sets) && Ways && std::has_single_bit(Pages));
    static_assert(std::has_single_bit(PageVertices) && std::uint64_t(Pages) * PageVertices <= UINT32_MAX);
    static constexpr unsigned EntryCount = Sets * Ways;
    struct Entry
    {
        VertexHistoryKey key;
        VertexHistoryAllocation allocation;
        std::uint32_t lastFrame = 0;
    };
    std::array<Entry, EntryCount> entries {};
    // Each node records its largest free aligned block, measured in pages.
    std::array<unsigned, Pages * 2> freeTree {};
    unsigned sweepCursor = 0, live = 0, usedPages = 0;
    std::uint32_t currentFrame = 0, retiredFrame = 0, nextGeneration = 0;

    static std::uint64_t hash(const VertexHistoryKey& key)
    {
        std::uint64_t h = 0x9e3779b97f4a7c15ull;
        const auto mix = [&](std::uint64_t word) { h ^= word + 0x9e3779b97f4a7c15ull + (h << 6) + (h >> 2); };
        mix(key.object.proxy); mix(key.object.mesh);
        mix(std::uint64_t(key.object.slot) << 32 | key.object.generation);
        mix(key.view); mix(key.pipeline); mix(key.topology);
        mix(std::uint64_t(key.chunk) << 32 | key.vertexFactory);
        h ^= h >> 30; h *= 0xbf58476d1ce4e5b9ull;
        h ^= h >> 27; h *= 0x94d049bb133111ebull;
        return h ^ (h >> 31);
    }
    bool reclaimable(const Entry& entry) const
    {
        // Preserve N-1 even when its GPU work already completed. Any recorded
        // current-frame reader updates lastFrame before using the allocation.
        return entry.allocation && entry.lastFrame < currentFrame - 1 && entry.lastFrame <= retiredFrame;
    }
    void release(Entry& entry)
    {
        const unsigned pages = entry.allocation.capacity / PageVertices;
        unsigned node = Pages / pages + entry.allocation.base / entry.allocation.capacity;
        unsigned extent = pages;
        freeTree[node] = pages;
        while (node > 1)
        {
            node /= 2;
            const auto left = freeTree[node * 2], right = freeTree[node * 2 + 1];
            freeTree[node] = left == extent && right == extent ? extent * 2 : (std::max)(left, right);
            extent *= 2;
        }
        usedPages -= pages;
        --live;
        entry = {};
        ++stats.reclaimed;
    }
    unsigned reserve(unsigned pages)
    {
        if (freeTree[1] < pages) return UINT32_MAX;
        unsigned node = 1, extent = Pages, base = 0;
        while (extent > pages)
        {
            extent /= 2;
            node *= 2;
            if (freeTree[node] < pages) { ++node; base += extent; }
        }
        freeTree[node] = 0;
        while (node > 1)
        {
            node /= 2;
            freeTree[node] = (std::max)(freeTree[node * 2], freeTree[node * 2 + 1]);
        }
        usedPages += pages;
        return base * PageVertices;
    }

  public:
    struct Stats
    {
        std::uint64_t hits = 0, inserted = 0, reclaimed = 0, rejectedIdentity = 0;
        std::uint64_t rejectedFrame = 0, rejectedTopology = 0, setFull = 0, arenaFull = 0, serialExhausted = 0;
        unsigned lastSweepInspections = 0, maxLookupInspections = 0;
    } stats;

    VertexHistoryCache()
    {
        for (unsigned level = 1, extent = Pages; extent; level *= 2, extent /= 2)
            for (unsigned node = level; node < level * 2; ++node) freeTree[node] = extent;
    }
    // Call once per admitted ordered frame. At most sweepBudget entries are
    // examined, even if the cache is full. Frame-counter wrap requires a drained
    // new owner; it cannot authorize reuse through modular arithmetic.
    bool beginFrame(std::uint32_t frame, std::uint32_t completedThrough, unsigned sweepBudget = 64)
    {
        if (!frame || frame <= currentFrame || completedThrough < retiredFrame || completedThrough >= frame)
        { ++stats.rejectedFrame; return false; }
        currentFrame = frame;
        retiredFrame = completedThrough;
        stats.lastSweepInspections = (std::min)(sweepBudget, EntryCount);
        for (unsigned i = 0; i < stats.lastSweepInspections; ++i)
        {
            auto& entry = entries[sweepCursor];
            sweepCursor = (sweepCursor + 1) % EntryCount;
            if (reclaimable(entry)) release(entry);
        }
        return true;
    }
    VertexHistoryAllocation acquire(const VertexHistoryKey& key, std::uint32_t vertices, std::uint32_t frame)
    {
        if (!key || !vertices || std::uint64_t(vertices) > std::uint64_t(Pages) * PageVertices)
        { ++stats.rejectedIdentity; return {}; }
        if (!frame || frame != currentFrame) { ++stats.rejectedFrame; return {}; }
        const unsigned start = unsigned(hash(key) & (Sets - 1)) * Ways;
        Entry* vacant = nullptr;
        Entry* oldest = nullptr;
        for (unsigned i = 0; i < Ways; ++i)
        {
            stats.maxLookupInspections = (std::max)(stats.maxLookupInspections, i + 1);
            auto& entry = entries[start + i];
            if (!entry.allocation) { if (!vacant) vacant = &entry; continue; }
            if (entry.key == key)
            {
                if (entry.allocation.vertices != vertices)
                { ++stats.rejectedTopology; return {}; }
                entry.lastFrame = frame;
                ++stats.hits;
                return entry.allocation;
            }
            if (reclaimable(entry) && (!oldest || entry.lastFrame < oldest->lastFrame)) oldest = &entry;
        }
        if (!vacant && !oldest) { ++stats.setFull; return {}; }
        if (nextGeneration == UINT32_MAX) { ++stats.serialExhausted; return {}; }
        if (!vacant) { release(*oldest); vacant = oldest; }
        const unsigned pages = std::bit_ceil((vertices + PageVertices - 1) / PageVertices);
        const unsigned base = reserve(pages);
        if (base == UINT32_MAX) { ++stats.arenaFull; return {}; }
        vacant->key = key;
        vacant->allocation = { base, vertices, pages * PageVertices, ++nextGeneration };
        vacant->lastFrame = frame;
        ++live;
        ++stats.inserted;
        return vacant->allocation;
    }
    unsigned liveEntries() const { return live; }
    unsigned reservedVertices() const { return usedPages * PageVertices; }
};
} // namespace GlassFg
