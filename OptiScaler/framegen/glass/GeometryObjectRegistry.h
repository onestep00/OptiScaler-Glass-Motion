#pragma once
#include <array>
#include <atomic>
#include <bit>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <mutex>
#include <shared_mutex>
#include <stdexcept>
#include <vector>

namespace GlassFg
{
// The packed transform's W lanes contain fixed-point integer position bits;
// do not reinterpret those lanes as floats or compare only world position.
struct GeometryObjectPose
{
    std::array<std::uint32_t, 12> packed {};
    std::array<float, 6> bounds {};
    std::uint32_t frame = 0;
    bool valid() const
    {
        for (unsigned i = 0; i < packed.size(); ++i)
            if (i % 4 != 3 && !std::isfinite(std::bit_cast<float>(packed[i])))
                return false;
        for (unsigned i = 0; i < 3; ++i)
            if (!std::isfinite(bounds[i]) || !std::isfinite(bounds[i + 3]) || bounds[i] > bounds[i + 3])
                return false;
        return true;
    }
};
struct GeometryObjectMatch
{
    std::uint64_t proxy = 0, mesh = 0;
    std::uint32_t slot = 0, generation = 0;
    GeometryObjectPose pose;
    explicit operator bool() const { return proxy && mesh && generation; }
};
struct GeometryObjectStats
{
    std::uint64_t registered = 0, removed = 0, updates = 0, live = 0, ambiguous = 0, invalidations = 0;
};

// Queries never dereference game memory. Default mode stores lifetime slots
// only. Explicit diagnostic mode retains four recent poses per slot to allow an instance upload
// to match an earlier pose. Identity comes from registration/lifetime events,
// not the transform hash. Equal live mesh/pose candidates remain ambiguous.
// Intrusive hash buckets use fixed storage: no allocation on pose updates.
// A unique indexed candidate alone does NOT prove the owner of a game draw:
// the adapter still needs direct provenance or complete mutation coverage.
class GeometryObjectRegistry
{
    static constexpr std::uint32_t History = 4, None = UINT32_MAX;
    struct Node
    {
        GeometryObjectPose pose;
        std::uint32_t previous = None, next = None, bucket = None;
    };
    struct Slot
    {
        std::uint64_t proxy = 0, mesh = 0;
        std::uint32_t generation = 0, cursor = 0;
        std::uint32_t arrayWriters = 0, arrayScope = 0;
        // Registration/recreation serial. Unlike generation it is not advanced
        // by array-setter updates, so temporal history can survive per-frame
        // array submissions while still resetting when the owner is recreated.
        std::uint32_t lifetime = 0;
        // Reader-visible version for the lock-free ticket below. Even = the
        // slot is complete, odd = a writer is between the two bumps.
        std::uint32_t version = 0;
    };
    mutable std::shared_mutex mutex;
    std::vector<Slot> slots;
    std::vector<Node> nodes;
    std::vector<std::uint32_t> buckets;
    GeometryObjectStats counters;
    mutable std::atomic<std::uint64_t> ambiguous = 0;
    static std::uint64_t hash(std::uint64_t mesh, const std::array<std::uint32_t, 12>& packed)
    {
        std::uint64_t h = mesh ^ 0x9e3779b97f4a7c15ull;
        for (unsigned i = 0; i < packed.size(); i += 2)
        {
            h ^= std::uint64_t(packed[i]) | (std::uint64_t(packed[i + 1]) << 32);
            h = std::rotl(h, 27) * 0x94d049bb133111ebull;
        }
        return h ^ (h >> 31);
    }
    void unlink(std::uint32_t index)
    {
        auto& n = nodes[index];
        if (n.bucket == None)
            return;
        if (n.previous == None)
            buckets[n.bucket] = n.next;
        else
            nodes[n.previous].next = n.next;
        if (n.next != None)
            nodes[n.next].previous = n.previous;
        n.bucket = n.previous = n.next = None;
    }
    // Reader side of the slot lifetime ticket.
    //
    // The ticket is the hottest query in the mod: every engine packet asks for
    // two of them and every captured draw validates one per element under a
    // unique lock, so the shared_lock that used to guard the read cost more
    // than the four fields it protected. Writers still hold the unique lock;
    // the version makes the read a seqlock, so a reader can never combine a
    // proxy from one registration with a generation from another and a setter
    // that ran during the read is detected instead of accepted.
    struct SlotSnapshot
    {
        std::uint64_t proxy = 0;
        std::uint32_t generation = 0, lifetime = 0, arrayWriters = 0;
    };
    bool snapshot(std::uint32_t index, SlotSnapshot& value) const noexcept
    {
        const auto& slot = slots[index];
        const std::atomic_ref<const std::uint32_t> version(slot.version);
        const auto first = version.load(std::memory_order_acquire);
        if (first & 1u)
            return false; // A writer is between the bumps.
        value.proxy = slot.proxy;
        value.generation = slot.generation;
        value.lifetime = slot.lifetime;
        value.arrayWriters = slot.arrayWriters;
        std::atomic_thread_fence(std::memory_order_acquire);
        return version.load(std::memory_order_relaxed) == first;
    }
    // Marks one slot mutation. RAII, so an early return cannot leave the odd
    // marker behind; nested marks stay correct because readers only require an
    // unchanged version, not an even one.
    struct Mutation
    {
        Slot& slot;
        explicit Mutation(Slot& value) noexcept : slot(value)
        {
            std::atomic_ref<std::uint32_t>(slot.version).fetch_add(1, std::memory_order_relaxed);
            std::atomic_thread_fence(std::memory_order_release);
        }
        Mutation(const Mutation&) = delete;
        Mutation& operator=(const Mutation&) = delete;
        ~Mutation() noexcept
        {
            std::atomic_thread_fence(std::memory_order_release);
            std::atomic_ref<std::uint32_t>(slot.version).fetch_add(1, std::memory_order_relaxed);
        }
    };
    void erase(std::uint32_t index)
    {
        auto& slot = slots[index];
        Mutation mark(slot);
        for (unsigned i = 0; !nodes.empty() && i < History; ++i)
            unlink(index * History + i);
        if (slot.proxy)
            --counters.live;
        slot.proxy = slot.mesh = 0;
        slot.cursor = 0;
        slot.arrayWriters = slot.arrayScope = 0;
    }
    bool activate(std::uint64_t proxy, std::uint32_t index, std::uint64_t mesh)
    {
        auto& slot = slots[index];
        erase(index);
        Mutation mark(slot);
        if (slot.generation == UINT32_MAX || slot.lifetime == UINT32_MAX)
            return false; // Never wrap into an old identity.
        ++slot.generation;
        ++slot.lifetime;
        slot.proxy = proxy;
        slot.mesh = mesh;
        ++counters.registered;
        ++counters.live;
        return true;
    }
    void append(std::uint32_t index, const GeometryObjectPose& pose)
    {
        if (nodes.empty()) return;
        auto& slot = slots[index];
        if (slot.cursor)
        {
            auto& n = nodes[index * History + (slot.cursor - 1) % History];
            if (n.bucket != None && n.pose.packed == pose.packed && n.pose.bounds == pose.bounds)
                return; // The unchanged pose remains valid; avoid redundant hash mutations.
        }
        const auto nodeIndex = index * History + slot.cursor++ % History;
        auto& node = nodes[nodeIndex];
        unlink(nodeIndex);
        node.pose = pose;
        node.bucket = static_cast<std::uint32_t>(hash(slot.mesh, pose.packed) & (buckets.size() - 1));
        node.next = buckets[node.bucket];
        if (node.next != None)
            nodes[node.next].previous = nodeIndex;
        buckets[node.bucket] = nodeIndex;
    }

  public:
    // Pose matching is retained only for explicit offline diagnostics. Runtime
    // draw ownership uses direct provenance plus lifetime tickets, not old poses.
    explicit GeometryObjectRegistry(std::uint32_t capacity = 131072, bool indexPoses = false)
    {
        if (!capacity || capacity > 131072)
            throw std::invalid_argument("Invalid engine registry capacity");
        slots.resize(capacity);
        if (indexPoses)
        {
            nodes.resize(std::size_t(capacity) * History);
            buckets.assign(std::bit_ceil(capacity * 2), None);
        }
    }
    bool registered(std::uint64_t proxy, std::uint32_t index, std::uint64_t mesh, const GeometryObjectPose& pose)
    {
        if (!proxy || !mesh || index >= slots.size() || !pose.valid())
            return false;
        std::unique_lock lock(mutex);
        auto& slot = slots[index];
        if ((slot.proxy != proxy || slot.mesh != mesh) && !activate(proxy, index, mesh))
            return false;
        append(index, pose);
        return true;
    }
    std::uint32_t ticket(std::uint64_t proxy, std::uint32_t index) const
    {
        if (!proxy || index >= slots.size())
            return 0;
        SlotSnapshot value;
        if (!snapshot(index, value))
            return 0;
        return value.proxy == proxy && !value.arrayWriters ? value.generation : 0;
    }
    // Registration lifetime only. Safe to use as a temporal identity across
    // array-setter updates; 0 means the slot is not this proxy or is mid-update.
    std::uint32_t lifetime(std::uint64_t proxy, std::uint32_t index) const
    {
        if (!proxy || index >= slots.size())
            return 0;
        SlotSnapshot value;
        if (!snapshot(index, value))
            return 0;
        return value.proxy == proxy && !value.arrayWriters ? value.lifetime : 0;
    }
    // Invalidate BEFORE the actual setter writes or reallocates the source.
    // The shared object generation now also covers observed array replacement.
    // A scope stamp survives nested pose invalidation but not slot retirement.
    std::uint32_t beginArrayUpdate(std::uint64_t proxy, std::uint32_t index)
    {
        if (!proxy || index >= slots.size()) return 0;
        std::unique_lock lock(mutex);
        auto& slot = slots[index];
        Mutation mark(slot);
        if (slot.proxy != proxy) return 0;
        if (slot.generation == UINT32_MAX || slot.arrayWriters == UINT32_MAX)
        { erase(index); return 0; }
        for (unsigned i = 0; !nodes.empty() && i < History; ++i)
            unlink(index * History + i);
        slot.cursor = 0;
        ++slot.generation;
        if (!slot.arrayWriters) slot.arrayScope = slot.generation;
        ++slot.arrayWriters;
        ++counters.invalidations;
        return slot.arrayScope;
    }
    bool endArrayUpdate(std::uint64_t proxy, std::uint32_t index, std::uint32_t scope)
    {
        if (!proxy || !scope || index >= slots.size()) return false;
        std::unique_lock lock(mutex);
        auto& slot = slots[index];
        Mutation mark(slot);
        if (slot.proxy != proxy || slot.arrayScope != scope || !slot.arrayWriters) return false;
        if (!--slot.arrayWriters) slot.arrayScope = 0;
        return true;
    }
    bool update(std::uint64_t proxy, std::uint32_t index, std::uint32_t generation, std::uint64_t mesh,
                const GeometryObjectPose& pose)
    {
        if (!proxy || !mesh || !generation || index >= slots.size() || !pose.valid())
            return false;
        std::unique_lock lock(mutex);
        auto& slot = slots[index];
        Mutation mark(slot);
        if (slot.proxy != proxy || slot.generation != generation)
            return false;
        if (slot.mesh != mesh && !activate(proxy, index, mesh))
            return false;
        append(index, pose);
        ++counters.updates;
        return true;
    }
    void removed(std::uint64_t proxy, std::uint32_t index)
    {
        if (!proxy || index >= slots.size())
            return;
        std::unique_lock lock(mutex);
        if (slots[index].proxy == proxy)
        {
            erase(index);
            ++counters.removed;
        }
    }
    void invalidate(std::uint64_t proxy, std::uint32_t index, std::uint32_t generation)
    {
        if (!proxy || !generation || index >= slots.size())
            return;
        std::unique_lock lock(mutex);
        if (slots[index].proxy == proxy && slots[index].generation == generation)
        {
            auto& slot = slots[index];
            Mutation mark(slot);
            for (unsigned i = 0; !nodes.empty() && i < History; ++i)
                unlink(index * History + i);
            slot.cursor = 0;
            if (slot.generation == UINT32_MAX)
                erase(index);
            else
                ++slot.generation;
            ++counters.invalidations;
        }
    }
    // Successful engine registration proves the mesh-proxy lifetime even if a
    // concurrent frame change prevents a stable pose snapshot. Keep it pending
    // so a later verified update can recover without a registry rescan.
    void pending(std::uint64_t proxy, std::uint32_t index)
    {
        if (!proxy || index >= slots.size())
            return;
        std::unique_lock lock(mutex);
        activate(proxy, index, 0);
        ++counters.invalidations;
    }
    GeometryObjectMatch find(std::uint64_t mesh, const std::array<std::uint32_t, 12>& packed, std::uint32_t frame) const
    {
        if (!mesh || nodes.empty())
            return {};
        std::shared_lock lock(mutex);
        GeometryObjectMatch result;
        auto index = buckets[hash(mesh, packed) & (buckets.size() - 1)];
        while (index != None)
        {
            const auto& n = nodes[index];
            const auto slotIndex = index / History;
            const auto& slot = slots[slotIndex];
            if (!slot.arrayWriters && slot.mesh == mesh && n.pose.packed == packed &&
                std::int32_t(frame - n.pose.frame) >= 0)
            {
                if (result && result.slot != slotIndex)
                {
                    ambiguous.fetch_add(1, std::memory_order_relaxed);
                    return {};
                }
                if (!result || std::int32_t(n.pose.frame - result.pose.frame) > 0)
                    result = { slot.proxy, slot.mesh, slotIndex, slot.generation, n.pose };
            }
            index = n.next;
        }
        return result;
    }
    GeometryObjectStats stats() const
    {
        std::shared_lock lock(mutex);
        auto result = counters;
        result.ambiguous = ambiguous.load(std::memory_order_relaxed);
        return result;
    }
    // Allocation sizes are immutable after construction; excludes vector/lock
    // bookkeeping and allocator overhead. No engine memory is included.
    std::size_t storageBytes() const
    {
        return slots.capacity() * sizeof(Slot) + nodes.capacity() * sizeof(Node) +
               buckets.capacity() * sizeof(std::uint32_t);
    }
};
} // namespace GlassFg
