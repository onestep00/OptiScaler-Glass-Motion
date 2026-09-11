#pragma once
#include <array>
#include <bit>
#include <cstdint>

namespace GlassFg
{
// Synchronously ordered creation/destruction events only. The adapter must
// observe destruction before engine memory is released. No borrowed pointer is
// dereferenced here. This cache does not discover or certify those events.
template <std::uint32_t Capacity> class GeometrySourceOwners
{
    static_assert(Capacity > 0 && Capacity <= 131072);
    static constexpr auto None = UINT32_MAX;
    static constexpr auto Buckets = std::bit_ceil(Capacity * 2);
  public:
    struct Source
    {
        std::uint64_t node = 0, buffer = 0, mesh = 0;
        std::uint32_t first = 0, count = 0;
        bool operator==(const Source&) const = default;
        explicit operator bool() const
        {
            return node && buffer && mesh && count &&
                   std::uint64_t(first) + count <= (std::uint64_t {1} << 32);
        }
    };
    struct Match
    {
        Source source;
        std::uint32_t generation = 0;
        explicit operator bool() const { return generation != 0; }
    };
  private:
    struct Entry
    {
        Source source;
        std::uint64_t proxy = 0, handle = 0;
        std::uint32_t generation = 0, next = None;
    };
    std::array<Entry, Capacity> entries {};
    std::array<std::uint32_t, Buckets> buckets;
    std::uint32_t free = 0, sequence = 0, live = 0;
    static std::uint32_t bucket(std::uint64_t proxy)
    {
        proxy ^= proxy >> 33; proxy *= 0xff51afd7ed558ccdULL;
        proxy ^= proxy >> 33;
        return static_cast<std::uint32_t>(proxy) & (Buckets - 1);
    }
  public:
    GeometrySourceOwners()
    {
        buckets.fill(None);
        for (std::uint32_t i = 0; i < Capacity; ++i) entries[i].next = i + 1;
        entries.back().next = None;
    }
    // Independent of registry-slot assignment: supports delayed registration.
    std::uint32_t created(std::uint64_t handle, std::uint64_t proxy, Source source)
    {
        if (!proxy) return 0;
        const auto b = bucket(proxy);
        auto index = buckets[b];
        while (index != None && entries[index].proxy != proxy) index = entries[index].next;
        if (!handle || !source || sequence == UINT32_MAX)
        {
            if (index != None) destroyed(entries[index].handle, proxy);
            return 0;
        }
        if (index == None)
        {
            if (free == None) return 0; // Never evict another live source.
            index = free; free = entries[index].next;
            entries[index].next = buckets[b]; buckets[b] = index; ++live;
        }
        auto& entry = entries[index];
        // Every observed creation has a new serial, including identical reused
        // addresses and identical source values. Equality cannot prove lifetime.
        entry.source = source; entry.proxy = proxy; entry.handle = handle;
        entry.generation = ++sequence;
        return entry.generation;
    }
    void destroyed(std::uint64_t handle, std::uint64_t proxy)
    {
        auto* link = &buckets[bucket(proxy)];
        while (*link != None)
        {
            const auto index = *link;
            auto& entry = entries[index];
            if (entry.proxy == proxy)
            {
                if (entry.handle != handle) return;
                *link = entry.next; entry = {}; entry.next = free; free = index; --live;
                return;
            }
            link = &entry.next;
        }
    }
    Match find(std::uint64_t proxy, std::uint64_t renderMesh) const
    {
        auto index = buckets[bucket(proxy)];
        while (index != None)
        {
            const auto& entry = entries[index];
            if (entry.proxy == proxy)
                return entry.source.mesh == renderMesh ? Match {entry.source, entry.generation} : Match {};
            index = entry.next;
        }
        return {};
    }
    std::uint32_t size() const { return live; }
};
} // namespace GlassFg
