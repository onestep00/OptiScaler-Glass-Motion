#pragma once
#include <array>
#include <cstdint>
#include <mutex>

namespace GlassFg
{
// Diagnostic scalar provenance, not a history lease or permission to draw MV.
struct DiagnosticInstanceSource
{
    std::uint64_t proxy = 0, mesh = 0, renderer = 0, scene = 0;
    std::uint32_t frame = 0, globalStart = 0, count = 0, originalCount = 0, ownerSlot = 0;
    bool linear = false;
    std::array<std::uint16_t, 64> indices {};
    bool operator==(const DiagnosticInstanceSource&) const = default;
};
template<unsigned Sets = 1024, unsigned Ways = 4> class DiagnosticInstanceLookup
{
    static_assert(Sets && (Sets & (Sets - 1)) == 0 && Ways);
    struct Entry { DiagnosticInstanceSource source; bool conflict = false; };
    std::array<Entry, Sets * Ways> entries {};
    std::mutex mutex;
    static unsigned bucket(const DiagnosticInstanceSource& s)
    {
        auto hash = s.mesh ^ (std::uint64_t(s.frame) << 32) ^ s.globalStart;
        hash ^= hash >> 33; hash *= 0xff51afd7ed558ccdULL; hash ^= hash >> 33;
        return (static_cast<unsigned>(hash) & (Sets - 1)) * Ways;
    }
    static bool sameKey(const DiagnosticInstanceSource& a, const DiagnosticInstanceSource& b)
    { return a.frame == b.frame && a.mesh == b.mesh && a.globalStart == b.globalStart && a.count == b.count; }
  public:
    bool publish(const DiagnosticInstanceSource& source)
    {
        if (!source.frame || !source.proxy || !source.mesh || !source.renderer || !source.scene ||
            !source.count || !source.originalCount || source.originalCount > 65536 ||
            source.count > source.originalCount || (!source.linear && source.count > source.indices.size()) ||
            std::uint64_t(source.globalStart) + source.count > (std::uint64_t {1} << 32) ||
            (source.linear && source.count != source.originalCount)) return false;
        if (!source.linear)
            for (unsigned i = 0; i < source.count; ++i)
                if (source.indices[i] >= source.originalCount) return false;
        std::lock_guard lock(mutex);
        Entry* reusable = nullptr;
        const auto begin = bucket(source);
        for (unsigned i = 0; i < Ways; ++i)
        {
            auto& entry = entries[begin + i];
            if (sameKey(entry.source, source))
            {
                if (entry.source != source) entry.conflict = true;
                return !entry.conflict;
            }
            // Never evict an observation from this or a newer engine frame.
            if (!entry.source.frame || entry.source.frame < source.frame) reusable = &entry;
        }
        if (!reusable) return false;
        *reusable = {source, false}; return true;
    }
    bool query(std::uint32_t frame, std::uint64_t mesh, std::uint32_t start, std::uint32_t count,
               DiagnosticInstanceSource& snapshot)
    {
        snapshot = {};
        DiagnosticInstanceSource key; key.frame = frame; key.mesh = mesh; key.globalStart = start; key.count = count;
        if (!frame || !mesh || !count) return false;
        std::lock_guard lock(mutex);
        const auto begin = bucket(key);
        for (unsigned i = 0; i < Ways; ++i)
        {
            const auto& entry = entries[begin + i];
            if (sameKey(entry.source, key))
            {
                if (entry.conflict) return false;
                snapshot = entry.source; return true;
            }
        }
        return false;
    }
    void clear()
    { std::lock_guard lock(mutex); for (auto& entry : entries) entry = {}; }
};
} // namespace GlassFg
