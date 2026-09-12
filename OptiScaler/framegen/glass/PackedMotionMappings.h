#pragma once
#include "VertexHistoryCache.h"

namespace GlassFg
{
// Boundary identity excludes chunk/pipeline/topology: pieces of one object must
// not create false interior contours. The caller supplies proven source keys.
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
    std::uint32_t frame = 0, next = 1;
    static bool same(const Entry& a, const VertexHistoryKey& b)
    {
        return a.object.proxy == b.object.proxy && a.object.mesh == b.object.mesh &&
            a.object.slot == b.object.slot && a.object.generation == b.object.generation &&
            a.view == b.view && a.arrayGeneration == b.arrayGeneration && a.sourceIndex == b.sourceIndex;
    }
  public:
    bool beginFrame(std::uint32_t value)
    { if (!value || value <= frame) return false; frame = value; next = 1; return true; }
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
        if (!available || next > 32767) return 0;
        *available = {key.object, key.view, key.arrayGeneration, key.sourceIndex, frame, next++};
        return available->id;
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
    std::uint32_t frame() const { return current; }
    unsigned reservedVertices() const { return histories.reservedVertices(); }
};
} // namespace GlassFg
