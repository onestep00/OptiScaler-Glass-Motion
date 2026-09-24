#pragma once
#include "VertexHistoryCache.h"

namespace GlassFg
{
// Boundary identity excludes chunk/pipeline/topology: pieces of one object must
// not create false interior contours. The caller supplies proven source keys.
// Object IDs count up from 1. Draw-local IDs (acquireLocal) count down from
// 32767 and are never stored, so no object key can map to one; both stop where
// the two ranges meet.
template<unsigned Sets = 4096, unsigned Ways = 4> class MotionBoundaryIds
{
    static_assert(std::has_single_bit(Sets) && Ways);
    struct Entry
    {
        GeometryDrawIdentity object;
        std::uint64_t view = 0;
        std::uint32_t arrayGeneration = 0, sourceIndex = 0, frame = 0, id = 0;
    };
    std::array<Entry, Sets * Ways> entries {};
    std::uint32_t frame = 0, next = 1, local = 32767;
    static bool same(const Entry& a, const VertexHistoryKey& b)
    {
        return a.object.proxy == b.object.proxy && a.object.mesh == b.object.mesh &&
            a.object.slot == b.object.slot && a.object.generation == b.object.generation &&
            a.view == b.view && a.arrayGeneration == b.arrayGeneration && a.sourceIndex == b.sourceIndex;
    }
  public:
    bool beginFrame(std::uint32_t value)
    { if (!value || value <= frame) return false; frame = value; next = 1; local = 32767; return true; }
    std::uint32_t acquire(const VertexHistoryKey& key)
    {
        if (!frame || !key) return 0;
        auto h = key.object.proxy ^ (key.object.mesh >> 4) ^ key.view;
        h ^= std::uint64_t(key.object.slot) << 32 | key.object.generation;
        h ^= std::uint64_t(key.arrayGeneration) << 32 | key.sourceIndex;
        h ^= h >> 33; h *= 0xff51afd7ed558ccdULL; h ^= h >> 33;
        const auto first = (static_cast<unsigned>(h) & (Sets - 1)) * Ways;
        Entry* available = nullptr;
        for (unsigned i = 0; i < Ways; ++i)
        {
            auto& entry = entries[first + i];
            if (entry.frame == frame) { if (same(entry, key)) return entry.id; }
            else if (!available) available = &entry;
        }
        if (!available || next > local) return 0;
        *available = {key.object, key.view, key.arrayGeneration, key.sourceIndex, frame, next++};
        return available->id;
    }
    std::uint32_t acquireLocal()
    {
        if (!frame || next > local) return 0;
        return local--;
    }
};

struct PackedMotionAllocation
{
    VertexHistoryAllocation history;
    std::uint32_t boundaryId = 0;
    explicit operator bool() const { return bool(history) && boundaryId; }
};

template<unsigned Sets = 4096, unsigned Ways = 4, unsigned Pages = 4096, unsigned PageVertices = 128>
class PackedMotionMappings
{
    VertexHistoryCache<Sets, Ways, Pages, PageVertices> histories;
    MotionBoundaryIds<Sets, Ways> boundaries;
    std::uint32_t current = 0;
  public:
    bool beginFrame(std::uint32_t frame, std::uint32_t completedThrough)
    {
        if (!histories.beginFrame(frame, completedThrough)) return false;
        if (!boundaries.beginFrame(frame)) return false;
        current = frame; return true;
    }
    PackedMotionAllocation acquire(const VertexHistoryKey& key, std::uint32_t vertices, std::uint32_t frame)
    {
        if (!frame || frame != current) return {};
        const auto boundary = boundaries.acquire(key);
        if (!boundary) return {};
        const auto history = histories.acquire(key, vertices, frame);
        return history ? PackedMotionAllocation {history, boundary} : PackedMotionAllocation {};
    }
    // Identity-only mapping for variants whose vertex stage reads no
    // GlassHistory and writes no GlassNext (native graft): the frame-local
    // boundary ID every draw of this object shares, and no arena block, so a
    // full arena cannot reject it. base/vertices/capacity stay zero. The
    // generation is the object's registration generation (nonzero for a valid
    // key, as coverage mappings carry it); the mapped vertex stage only tests
    // it for liveness, and zero vertices make a history reader reject the entry
    // before any history access. Nothing outlives the frame.
    PackedMotionAllocation acquireIdentity(const VertexHistoryKey& key, std::uint32_t frame)
    {
        if (!frame || frame != current) return {};
        const auto boundary = boundaries.acquire(key);
        return boundary ? PackedMotionAllocation {{0, 0, 0, key.object.generation}, boundary}
                        : PackedMotionAllocation {};
    }
    // Draw-local mapping for an element without an engine owner (no span
    // identity and no parent). Only the camera-only graft variant may use it:
    // that variant reads no GlassHistory, writes no GlassNext and reads no
    // MotionMatrix, so the element needs no object key. It takes a boundary ID
    // unique in the frame and a nonzero generation, which the mapped vertex
    // stage tests for liveness only. The object ID table and the history arena
    // are not touched, and nothing outlives the frame.
    PackedMotionAllocation acquireDrawLocal(std::uint32_t frame)
    {
        if (!frame || frame != current) return {};
        const auto boundary = boundaries.acquireLocal();
        return boundary ? PackedMotionAllocation {{0, 0, 0, 1}, boundary} : PackedMotionAllocation {};
    }
    std::uint32_t frame() const { return current; }
    unsigned reservedVertices() const { return histories.reservedVertices(); }
    unsigned arenaPages() const { return histories.capacityPages(); }
    unsigned arenaUsedPages() const { return histories.usedPageCount(); }
    unsigned arenaLargestFreePages() const { return histories.largestFreePages(); }
    // N-1 stability evidence: inserted grows when the element key changes
    // between frames, hits grows when the same key reuses its history.
    const typename VertexHistoryCache<Sets, Ways, Pages, PageVertices>::Stats& historyStats() const
    {
        return histories.stats;
    }
    unsigned liveHistories() const { return histories.liveEntries(); }
    std::uint32_t historyFrame() const { return histories.frameNumber(); }
    std::uint32_t historyRetiredFrame() const { return histories.retiredFrameNumber(); }
    unsigned historyPinnedEntries() const { return histories.pinnedForPreviousFrame(); }
};
} // namespace GlassFg
