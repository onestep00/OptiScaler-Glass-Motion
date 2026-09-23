#include "pch.h"
#include "CyberpunkDraws.h"
#include "CyberpunkObjects.h"
#include "CyberpunkLayout.h"
#include "CyberpunkInstanceSelection.h"
#include "DetourThreads.h"
#include "GlassControls.h"
#include "GlassHookProbe.h"
#include <bit>
#include <atomic>
#include <cmath>
#include <mutex>
#include <type_traits>

namespace GlassFg
{
namespace
{
using Run = void (*)(void*, void*, void*);
using Append = void (*)(void*, void*, std::uintptr_t, std::uintptr_t, void*);
using Rigid = void (*)(void*, void*, void*, std::uint32_t, bool);
using Skinned = void (*)(void*, void*, void*, bool);
using Upload = std::uint32_t (*)(void*, void*, std::uint32_t, void*);
Run originalRun = nullptr;
Append originalAppend = nullptr;
Rigid originalRigid = nullptr;
Skinned originalSkinned = nullptr;
Upload originalUpload = nullptr;

// Owner of one recorded packet as far as its append can establish it: the proxy
// behind the packet's registry entry, the geometry record's mesh and the
// lifetime ticket taken before the engine consumed the packet. The proxy header
// half of the owner check runs when a draw that reads owners consumes the span
// (completeOwners); the other draws never pay for it.
struct PacketOwner
{
    GeometryDrawIdentity parent; // no generation: the append found no owner
    // One instance whose transform is the renderer's own slot for this packet:
    // the part of the single-object rule that needs the append's arguments.
    bool single = false;
};
// GeometryDrawBatch keeps its span capacity private; its size pins it.
constexpr std::size_t kBatchSpans = 2048;
static_assert(sizeof(GeometryDrawBatch) / sizeof(GeometryBatchSpan) == kBatchSpans);
struct Batch
{
    GeometryDrawBatch rigid, skinned;
    // Owner of each recorded span, by span index. Written for every span the
    // batch keeps, so a reused pool slot never lends an older owner.
    std::array<PacketOwner, kBatchSpans> rigidOwners, skinnedOwners;
    std::uint64_t context = 0, renderer = 0;
    std::uint32_t frame = 0;
    // Array order probe run: owners complete at append, because the probe
    // compares the element order right after the engine's append.
    bool eager = false;
};
struct EngineDrawState
{
    std::shared_ptr<GeometryObjectRegistry> registry;
    const volatile std::uint32_t* tick = nullptr;
    const void* rendererGlobal = nullptr;
    const void* drawReturn = nullptr;
    std::array<Batch, 16> pool;
    std::atomic<std::uint32_t> occupied = 0;
    // Every engine append and draw used to bump one shared cache line from
    // every render thread. The locked read-modify-write on those lines was the
    // measured dominant cost of the draw hooks. Hot counters now live in
    // per-thread rows: the owner thread writes its own row with a relaxed
    // load/store pair (no lock, no false sharing with other threads) and the
    // report thread sums the rows. A row has exactly one writer (claimed by
    // thread id; the claim is documented at row()). Counters stay exact.
    struct Row
    {
        alignas(64) std::atomic<std::uint32_t> owner { 0 };
        std::atomic<std::uint64_t> batches = 0, appends = 0, identities = 0, draws = 0, rejected = 0;
        std::atomic<std::uint64_t> parentNoFlag = 0, parentNoEntry = 0, parentNoTicket = 0, parentNoSlot = 0,
                                 parentNoMesh = 0, parentNoHeader = 0, parentNoSelection = 0;
        std::atomic<std::uint64_t> parentNoSelectionGrouped = 0, parentNoSelectionNonGlobal = 0,
                                 parentNoSelectionRange = 0;
        // Split of the packet-local selection rejection (see CyberpunkDraws.h).
        std::atomic<std::uint64_t> parentNonGlobalCount1 = 0, parentNonGlobalCountMore = 0,
                                 parentNonGlobalCountMoreSkin = 0;
        std::atomic<std::uint64_t> parentSeeded = 0;
        // Owner completion (see completeOwners): spans that reused the header
        // read of the previous span's identical owner, and header reads.
        std::atomic<std::uint64_t> parentMemoHits = 0, parentMemoMisses = 0;
    };
    static constexpr unsigned RowCount = 64;
    std::array<Row, RowCount> rows {};
    // The calling thread's row for this state. Never call from the report
    // thread expecting its own totals; use the summed status below.
    Row& row() noexcept;
    // The first row() call of a thread for this state; see its definition.
    Row& claimRow() noexcept;
    std::atomic<std::uint64_t> arrayProbeCompared = 0, arrayProbePermuted = 0, arrayProbeChanged = 0;
    std::atomic<std::uint64_t> arrayProbeSameAddress = 0, arrayProbeDistinctAddress = 0;
    std::atomic<std::uint64_t> arrayProbeGrouped = 0;
    std::atomic<std::uint64_t> arrayProbeLocal = 0, arrayProbeLocalCompared = 0, arrayProbeLocalPermuted = 0,
                             arrayProbeLocalChanged = 0;
    std::atomic<std::uint64_t> arrayProbeLocalSameAddress = 0, arrayProbeLocalDistinctAddress = 0;
    std::atomic<std::uint64_t> arrayProbeLocalUnreadable = 0, arrayProbeLocalBaseMoved = 0;
    std::atomic<std::uint64_t> arrayProbeLocalGatePass = 0, arrayProbeLocalGateCount = 0,
                               arrayProbeLocalGateSkin = 0;
};
std::atomic<EngineDrawState*> activeDrawState = nullptr;
thread_local Batch* currentBatch = nullptr;
thread_local EngineDrawState* rowState = nullptr;
thread_local unsigned rowIndex = 0;

// One row per thread, claimed by thread id. A thread that finds every row
// taken shares row 0; that costs counter accuracy only, never safety, and
// needs more than RowCount threads touching one state to happen.
__declspec(noinline) EngineDrawState::Row& EngineDrawState::claimRow() noexcept
{
    const auto id = static_cast<std::uint32_t>(GetCurrentThreadId());
    unsigned index = 0;
    for (unsigned i = 0; i < RowCount; ++i)
    {
        std::uint32_t expected = 0;
        if (rows[i].owner.compare_exchange_strong(expected, id, std::memory_order_relaxed))
        {
            index = i;
            break;
        }
    }
    rowState = this;
    rowIndex = index;
    return rows[index];
}
// Every hook asks for its row, and the claim above runs once per thread and
// state. With the claim out of line the check is three instructions, which
// MSVC still kept as a call in each hook, so it is forced inline.
__forceinline EngineDrawState::Row& EngineDrawState::row() noexcept
{
    return rowState == this ? rows[rowIndex] : claimRow();
}

// Owner-thread increment of a per-thread row counter. The row has a single
// writer, so a relaxed load/store pair replaces the locked read-modify-write
// the shared counters used. The report thread reads the same atomic.
inline void bump(std::atomic<std::uint64_t>& value) noexcept
{
    value.store(value.load(std::memory_order_relaxed) + 1, std::memory_order_relaxed);
}
inline void add(std::atomic<std::uint64_t>& value, std::uint64_t count) noexcept
{
    value.store(value.load(std::memory_order_relaxed) + count, std::memory_order_relaxed);
}

// The range a guarded read accepts: above the null page and, for the whole
// object, below the user-mode limit.
constexpr bool readable(std::uint64_t address, std::size_t size) noexcept
{
    return address >= 0x10000 && address <= 0x7fffffffffff - size;
}
template <typename T> bool copyAt(std::uint64_t address, T& value) noexcept
{
    static_assert(std::is_trivially_copyable_v<T>);
    __try
    {
        if (!readable(address, sizeof(T)))
            return false;
        memcpy(&value, reinterpret_cast<const void*>(address), sizeof(T));
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
}
// The proxy header fields the packet identity checks read. The layout matches
// the engine's proxy object: slot at 0x98, mesh at 0xd8, flags at 0xea, and the
// instance header at 0x108 (transform array, element count, global start).
struct ProxyFields
{
    std::uint32_t slot = UINT32_MAX;
    std::uint64_t mesh = 0;
    std::uint16_t flags = 0;
    std::uint64_t transforms = 0;
    std::uint32_t instanceCount = 0, globalStart = UINT32_MAX;
};
// One guarded copy of 0x90..0x120 replaced the four separately guarded copies
// the checks below used to make. A guarded copy is a non-inlined call. The
// bytes are the same bytes the individual reads covered.
bool readProxyFields(std::uint64_t proxy, ProxyFields& value) noexcept
{
    std::array<std::byte, 0x90> header {};
    if (!copyAt(proxy + 0x90, header))
        return false;
    std::memcpy(&value.slot, header.data() + 0x08, sizeof(value.slot));
    std::memcpy(&value.mesh, header.data() + 0x48, sizeof(value.mesh));
    std::memcpy(&value.flags, header.data() + 0x5a, sizeof(value.flags));
    std::memcpy(&value.transforms, header.data() + 0x78, sizeof(value.transforms));
    std::memcpy(&value.instanceCount, header.data() + 0x80, sizeof(value.instanceCount));
    std::memcpy(&value.globalStart, header.data() + 0x84, sizeof(value.globalStart));
    return true;
}
// Recover an object the registry never observed (created before the hooks were
// installed or registered through an unaudited path). This is the same verified
// two-read snapshot contract the registration hook uses; it never guesses.
bool seedObject(EngineDrawState& state, std::uint64_t proxy) noexcept
{
    struct Pose
    {
        std::array<std::uint32_t, 12> packed {};
        std::array<float, 6> bounds {};
        bool operator==(const Pose&) const = default;
    };
    try
    {
        Pose first, second;
        std::uint64_t meshFirst = 0, meshSecond = 0;
        std::uint32_t indexFirst = 0, indexSecond = 0;
        if (!copyAt(proxy + 0xd8, meshFirst) || !copyAt(proxy + 0x98, indexFirst) ||
            !copyAt(proxy + 0x18, first.packed) || !copyAt(proxy + 0x50, first.bounds))
            return false;
        MemoryBarrier();
        if (!copyAt(proxy + 0xd8, meshSecond) || !copyAt(proxy + 0x98, indexSecond) ||
            !copyAt(proxy + 0x18, second.packed) || !copyAt(proxy + 0x50, second.bounds))
            return false;
        if (meshFirst != meshSecond || indexFirst != indexSecond || first != second || !meshFirst ||
            indexFirst >= 131072)
            return false;
        for (unsigned i = 0; i < first.packed.size(); ++i)
            if (i % 4 != 3 && !std::isfinite(std::bit_cast<float>(first.packed[i])))
                return false;
        for (unsigned i = 0; i < 3; ++i)
            if (!std::isfinite(first.bounds[i]) || !std::isfinite(first.bounds[i + 3]) ||
                first.bounds[i] > first.bounds[i + 3])
                return false;
        GeometryObjectPose pose;
        pose.packed = first.packed;
        pose.bounds = first.bounds;
        pose.frame = state.tick ? *state.tick : 0;
        if (!pose.valid())
            return false;
        return state.registry->registered(proxy, indexFirst, meshFirst, pose) != 0;
    }
    catch (...)
    {
        return false;
    }
}
// Grouped-array element order probe.
//
// The grouped update path (proxy flag 0x2000) repacks the group's selected
// elements into the packet in the engine's own selection order, so a packet
// ordinal is a usable element key only while that order is unchanged. This
// probe hashes the element bytes each packet received and compares them with
// the previous frame of the same array: an unchanged element set sitting at a
// different ordinal is a permutation, which is exactly the case that pairs an
// element with another element's previous transform. It reads only bytes the
// engine already wrote into the packet and is off unless the live arrayprobe
// switch is on.
constexpr unsigned kArrayProbeElements = 64;
constexpr unsigned kArrayProbeSlots = 6;
constexpr unsigned kArrayProbePerFrame = 2;
// The packet-local family is a minority of the appends but the one the
// packetLocalOrder switch depends on, so it gets its own per-frame budget
// instead of competing with the grouped packets for the two shared samples.
constexpr unsigned kArrayProbeLocalPerFrame = 2;
struct ArrayProbeSlot
{
    std::uint64_t proxy = 0;
    // Element base the previous sample of this key was read from. A moved base
    // means the two samples are not the same array and "changed" cannot be
    // read as an element-set change of one array.
    std::uint64_t base = 0;
    std::uint32_t generation = 0, count = 0;
    // Grouped (kind 2) and packet-local (kinds 3 and 4) packets are different
    // arrays even when they share an owner proxy, element count and lifetime
    // generation. One slot per family keeps a grouped sample from being
    // compared against a packet-local sample, which would report a set change
    // or a moved base that no single array actually had.
    bool grouped = false;
    std::uint64_t hashes[kArrayProbeElements] {};
};
thread_local ArrayProbeSlot arrayProbeSlots[kArrayProbeSlots];
thread_local unsigned arrayProbeCursor = 0, arrayProbeInFrame = 0, arrayProbeLocalInFrame = 0;
thread_local std::uint32_t arrayProbeFrame = UINT32_MAX;

bool arrayElementHash(std::uint64_t address, std::uint64_t& hash) noexcept
{
    std::array<std::byte, 48> bytes {};
    if (!copyAt(address, bytes))
        return false;
    std::uint64_t value = 0xcbf29ce484222325ull;
    for (const auto byte : bytes)
    {
        value ^= std::to_integer<std::uint8_t>(byte);
        value *= 0x100000001b3ull;
    }
    hash = value ? value : 1;
    return true;
}
void probeArrayOrder(EngineDrawState& state, const GeometryBatchSpan& record, std::uint64_t destination,
                     std::uint64_t container) noexcept
{
    if (!ReadControls().arrayProbe)
        return;
    const bool grouped = record.orderKind == 2;
    const bool local = record.orderKind == 3 || record.orderKind == 4;
    if (!grouped && !local)
        return;
    // The audited Append body reads this packet's element bytes itself at
    // first_argument + 48*i (rva 1f1a88 rigid copy loop; instruction-level
    // evidence in batched-element-base.json). The call site's grouped container is
    // that same expression, but the packet-local family is counted apart so a
    // first argument that is not the grouped array address stays visible
    // instead of silently changing the grouped counters.
    (grouped ? (destination == container ? state.arrayProbeSameAddress : state.arrayProbeDistinctAddress)
             : (destination == container ? state.arrayProbeLocalSameAddress
                                         : state.arrayProbeLocalDistinctAddress))
        .fetch_add(1, std::memory_order_relaxed);
    (grouped ? state.arrayProbeGrouped : state.arrayProbeLocal).fetch_add(1, std::memory_order_relaxed);
    if (!record.parent || record.count < 2 || record.count > kArrayProbeElements)
        return;
    const auto frame = state.tick ? *state.tick : 0;
    if (frame != arrayProbeFrame)
    {
        arrayProbeFrame = frame;
        arrayProbeInFrame = 0;
        arrayProbeLocalInFrame = 0;
    }
    // One budget per family. The grouped packets arrive early and outnumber the
    // packet-local ones, so a shared budget would spend every sample on the
    // grouped family and leave the packet-local counters permanently zero.
    if (grouped)
    {
        if (arrayProbeInFrame >= kArrayProbePerFrame)
            return;
        ++arrayProbeInFrame;
    }
    else
    {
        if (arrayProbeLocalInFrame >= kArrayProbeLocalPerFrame)
            return;
        ++arrayProbeLocalInFrame;
    }
    // Compare the bytes the engine consumed for this packet: the grouped array
    // slot for kind 2, the packet's own element array for kinds 3 and 4.
    const auto base = grouped ? container : destination;
    std::uint64_t hashes[kArrayProbeElements] {};
    for (unsigned i = 0; i < record.count; ++i)
        if (!arrayElementHash(base + std::uint64_t(i) * 48, hashes[i]))
        {
            if (local)
                state.arrayProbeLocalUnreadable.fetch_add(1, std::memory_order_relaxed);
            return;
        }
    ArrayProbeSlot* slot = nullptr;
    for (auto& candidate : arrayProbeSlots)
        if (candidate.grouped == grouped && candidate.count == record.count && candidate.proxy == record.parent.proxy &&
            candidate.generation == record.parent.generation)
        {
            slot = &candidate;
            break;
        }
    if (!slot)
        slot = &arrayProbeSlots[arrayProbeCursor++ % kArrayProbeSlots];
    else
    {
        if (local && slot->base != base)
            state.arrayProbeLocalBaseMoved.fetch_add(1, std::memory_order_relaxed);
        bool taken[kArrayProbeElements] {};
        bool sameSet = true, sameOrder = true;
        for (unsigned i = 0; i < record.count && sameSet; ++i)
        {
            bool matched = false;
            for (unsigned j = 0; j < record.count; ++j)
                if (!taken[j] && slot->hashes[j] == hashes[i])
                {
                    taken[j] = true;
                    matched = true;
                    if (j != i)
                        sameOrder = false;
                    break;
                }
            if (!matched)
                sameSet = false;
        }
        if (sameSet)
        {
            (grouped ? state.arrayProbeCompared : state.arrayProbeLocalCompared)
                .fetch_add(1, std::memory_order_relaxed);
            if (!sameOrder)
                (grouped ? state.arrayProbePermuted : state.arrayProbeLocalPermuted)
                    .fetch_add(1, std::memory_order_relaxed);
        }
        else
            (grouped ? state.arrayProbeChanged : state.arrayProbeLocalChanged)
                .fetch_add(1, std::memory_order_relaxed);
    }
    slot->proxy = record.parent.proxy;
    slot->generation = record.parent.generation;
    slot->base = base;
    slot->count = record.count;
    slot->grouped = grouped;
    for (unsigned i = 0; i < record.count; ++i)
        slot->hashes[i] = hashes[i];
}
struct EngineContext
{
    std::uint64_t rigid = 0;
    std::uint32_t rigidCapacity = 0, rigidCount = 0;
    std::uint64_t skinned = 0;
    std::uint32_t skinCapacity = 0, skinCount = 0;
    std::array<std::byte, 0x28> unused {};
    std::uint64_t geometry = 0, entry = 0;
};
static_assert(sizeof(EngineContext) == 0x58 && offsetof(EngineContext, geometry) == 0x48);
struct EngineGeometry
{
    std::uint8_t kind = 0;
    std::array<std::byte, 7> unused0 {};
    std::uint64_t mesh = 0;
    std::array<std::byte, 0x10> unused1 {};
    std::uint32_t indexCount = 0, unused2 = 0;
    std::uint8_t flags = 0, unused3 = 0;
    std::uint16_t chunk = 0;
};
static_assert(offsetof(EngineGeometry, chunk) == 0x2a);
bool renderer(EngineDrawState& state, std::uint64_t& result)
{
    std::uint64_t root = 0;
    return copyAt(reinterpret_cast<std::uint64_t>(state.rendererGlobal), root) && copyAt(root + 0x4628, result) &&
           result && result < 0x7fffffffffff - 0xb74380;
}
// What an append reads before the engine consumes its packet: the context and
// the packet words and, for an append of the batch's own context, the owner's
// registry entry and geometry record. A guarded read is a non-inlined call;
// this one call replaced the four copyAt calls the hook made, with the same
// bytes and the same checks between them. The two guards keep a fault where
// the separate reads put it: in the context or packet the append is not
// tracked, in the entry or geometry record only the owner is lost (no_entry).
struct AppendInputs
{
    EngineContext context;
    std::array<std::uint64_t, 2> words {};
    std::uint64_t entry = 0;
    EngineGeometry geometry;
};
enum class AppendRead
{
    Unreadable, // context or packet: the append is not tracked
    Packet,     // context and packet only: an append of another context
    NoFlag,     // no owner flag (bit 51)
    NoEntry,    // slot index, entry address, entry, geometry record or its kind
    Owner,      // entry and a kind 0 geometry record
};
AppendRead readAppend(std::uint64_t context, std::uint64_t packet, std::uint64_t renderer, bool owner,
                      AppendInputs& value) noexcept
{
    __try
    {
        if (!readable(context, sizeof(value.context)) || !readable(packet, sizeof(value.words)))
            return AppendRead::Unreadable;
        memcpy(&value.context, reinterpret_cast<const void*>(context), sizeof(value.context));
        memcpy(&value.words, reinterpret_cast<const void*>(packet), sizeof(value.words));
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return AppendRead::Unreadable;
    }
    if (!owner)
        return AppendRead::Packet;
    if (!(value.words[1] & (1ull << 51)))
        return AppendRead::NoFlag;
    const auto index = value.words[1] & 0x3ffff;
    const auto entry = value.context.entry, geometry = value.context.geometry;
    if (index >= 131072 || entry != renderer + 0x274248 + index * 24)
        return AppendRead::NoEntry;
    __try
    {
        if (!readable(entry, sizeof(value.entry)) || !readable(geometry, sizeof(value.geometry)))
            return AppendRead::NoEntry;
        memcpy(&value.entry, reinterpret_cast<const void*>(entry), sizeof(value.entry));
        memcpy(&value.geometry, reinterpret_cast<const void*>(geometry), sizeof(value.geometry));
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return AppendRead::NoEntry;
    }
    return value.geometry.kind == 0 ? AppendRead::Owner : AppendRead::NoEntry;
}
// The context and geometry record a flush reads, under one guard as above.
// The record is read only when it is the context's current one.
bool readFlush(std::uint64_t context, std::uint64_t geometry, EngineContext& data, EngineGeometry& desc) noexcept
{
    __try
    {
        if (!readable(context, sizeof(data)))
            return false;
        memcpy(&data, reinterpret_cast<const void*>(context), sizeof(data));
        if (data.geometry != geometry || !readable(geometry, sizeof(desc)))
            return false;
        memcpy(&desc, reinterpret_cast<const void*>(geometry), sizeof(desc));
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
}
// Append-time half of the packet owner: the owner flag, the registry entry and
// the geometry record (read by readAppend) and the lifetime ticket. The ticket
// is taken before the engine consumes the packet, so a registration or array
// setter that runs before the draw changes it and the draw refuses the owner
// (checkOwner).
//
// Nothing here throws (the ticket is a lock-free read and seedObject is
// noexcept), so there is no try block, which let MSVC inline this into the
// hook. The owner is written field by field in place: a returned one was
// built in a temporary and copied with 16-byte loads that store forwarding
// cannot serve.
void takeOwner(EngineDrawState& state, EngineDrawState::Row& row, AppendRead read, const AppendInputs& value,
               GeometryDrawIdentity& parent) noexcept
{
    if (read != AppendRead::Owner)
    {
        bump(read == AppendRead::NoFlag ? row.parentNoFlag : row.parentNoEntry);
        return;
    }
    const auto index = static_cast<std::uint32_t>(value.words[1] & 0x3ffff);
    const auto proxy = value.entry & 0x00ffffffffffffffull;
    auto generation = state.registry->ticket(proxy, index);
    if (!generation && seedObject(state, proxy))
    {
        generation = state.registry->ticket(proxy, index);
        if (generation)
            bump(row.parentSeeded);
    }
    if (!generation)
    {
        bump(row.parentNoTicket);
        return;
    }
    parent.proxy = proxy;
    parent.mesh = value.geometry.mesh;
    parent.slot = index;
    parent.generation = generation;
}
enum class OwnerCheck
{
    Owner,
    NoSlot,
    NoMesh,
    Changed,
};
// Proxy header half of the owner check. The header is read between the
// append's ticket and a second one, so when the two agree no registration or
// array setter was active or completed at any point in between, however far
// the draw is from the append. A changed ticket is reported first: the header
// may then describe another owner state than the one the packet was taken in.
// The array fields (+0xea flags, +0x108/+0x110/+0x114) have no ticket of their
// own; the selection sees them as they are when the draw consumes the packet,
// in the same engine frame and batch run as the append.
OwnerCheck checkOwner(const EngineDrawState& state, const GeometryDrawIdentity& owner, ProxyFields& fields)
{
    const bool read = readProxyFields(owner.proxy, fields);
    if (state.registry->ticket(owner.proxy, owner.slot) != owner.generation)
        return OwnerCheck::Changed;
    if (!read || fields.slot != owner.slot)
        return OwnerCheck::NoSlot;
    if (!fields.mesh || fields.mesh != owner.mesh)
        return OwnerCheck::NoMesh;
    return OwnerCheck::Owner;
}
// Single-object identity and element order of a span whose owner resolved: a
// function of the packet, the owner's proxy header and the controls only.
void selectOwner(GeometryBatchSpan& record, const ProxyFields& fields, bool single, bool skin,
                 const Controls& controls, EngineDrawState::Row& row) noexcept
{
    if (single && !fields.transforms && !fields.instanceCount && fields.globalStart == UINT32_MAX)
        record.identity = record.parent;
    CyberpunkInstanceSelection selection;
    if (selection.resolveGlobalPacket(record.global, record.transformIndex, record.count, fields.globalStart,
                                      fields.instanceCount, fields.flags))
    {
        record.orderKind = 1;
        record.originalFirst = static_cast<std::uint16_t>(selection.linearFirst);
        return;
    }
    bump(row.parentNoSelection);
    switch (CyberpunkInstanceSelection::rejectCode(record.global, record.transformIndex, record.count,
                                                   fields.globalStart, fields.instanceCount, fields.flags))
    {
    case 1:
        bump(row.parentNoSelectionGrouped);
        // Grouped update array. The engine repacks the source
        // elements into this same allocation in group order, so
        // the packet ordinal is the element's position in the
        // group. It is only used together with the array's
        // observed lifetime generation, which invalidates the
        // history whenever the array is mutated.
        if (controls.groupedOrder && record.count > 1)
        {
            record.orderKind = 2;
            record.originalFirst = 0;
        }
        break;
    case 2:
        bump(row.parentNoSelectionNonGlobal);
        // Only count>1 non-skinned spans can become kind 3/4
        // and reach the order probe, so split the population
        // that stays at ordinal 0 instead of guessing: a
        // count==1 span is a single element, and a count==0
        // span selects nothing and lands in count_more.
        if (controls.arrayProbe)
            bump(record.count == 1 ? row.parentNonGlobalCount1
                                   : (skin ? row.parentNonGlobalCountMoreSkin : row.parentNonGlobalCountMore));
        // Packet-local instanced selection: particles and other
        // instanced transparency. The engine keeps the element
        // bytes in the packet itself and exposes no source
        // index. Kind 3 marks the span as a probe target; kind 4
        // authorizes the packet ordinal once the order probe has
        // shown it is stable for this family.
        if (record.count > 1)
        {
            record.orderKind = controls.packetLocalOrder ? 4 : 3;
            record.originalFirst = 0;
        }
        break;
    default: bump(row.parentNoSelectionRange); break;
    }
}
struct RunScope
{
    EngineDrawState* state;
    Batch* previous = currentBatch;
    unsigned slot = 16;
    explicit RunScope(EngineDrawState* value) : state(value)
    {
        currentBatch = nullptr; // Unobserved nested work cannot inherit its parent's identity.
        if (!state)
            return;
        auto occupied = state->occupied.load(std::memory_order_relaxed);
        while ((occupied & 0xffff) != 0xffff)
        {
            const auto candidate = std::countr_zero((~occupied) & 0xffffu);
            if (!state->occupied.compare_exchange_weak(occupied, occupied | (1u << candidate)))
                continue;
            slot = candidate;
            auto& batch = state->pool[slot];
            batch.rigid.clear();
            batch.skinned.clear();
            batch.context = 0;
            batch.frame = *state->tick;
            try
            {
                batch.eager = ReadControls().arrayProbe;
            }
            catch (...)
            {
                batch.eager = false;
            }
            if (renderer(*state, batch.renderer))
                currentBatch = &batch;
            break;
        }
        auto& row = state->row();
        bump(row.batches);
        if (!currentBatch)
            bump(row.rejected);
    }
    ~RunScope()
    {
        currentBatch = previous;
        if (slot < 16)
            state->occupied.fetch_and(~(1u << slot), std::memory_order_release);
    }
};
struct Flush
{
    EngineDrawState* state = nullptr;
    Batch* batch = nullptr;
    GeometryDrawBatch* records = nullptr;
    std::uint64_t source = 0, mesh = 0;
    std::uint32_t count = 0, chunk = 0, stride = 0, indexCount = 0, origin = UINT32_MAX;
    bool uploaded = false, consumed = false, invalid = false;
};
thread_local Flush* currentFlush = nullptr;
struct FlushScope
{
    Flush* previous = currentFlush;
    explicit FlushScope(Flush& value) { currentFlush = &value; }
    ~FlushScope() { currentFlush = previous; }
};

void run(void* a, void* b, void* c)
{
    const bool idle = DrawHooksIdle();
    const HookCostScope cost(RunHookCost(), !idle);
    if (idle)
    {
        cost.Stop();
        originalRun(a, b, c);
        return;
    }
    RunScope scope(activeDrawState.load(std::memory_order_acquire));
    cost.Stop();
    originalRun(a, b, c);
}
void append(void* transforms, void* packet, std::uintptr_t c, std::uintptr_t d, void* context)
{
    if (DrawHooksIdle())
    {
        originalAppend(transforms, packet, c, d, context);
        return;
    }
    auto* state = activeDrawState.load(std::memory_order_acquire);
    auto* batch = currentBatch;
    if (!state || !batch)
    {
        // Outside a tracked geometry batch. The full path reads engine memory
        // and then returns from the same point without recording anything, so
        // this leaves before the probe. The engine calls Append thousands of
        // times per frame, and only a few hundred land inside a batch.
        originalAppend(transforms, packet, c, d, context);
        return;
    }
    auto& row = state->row();
    const HookCostScope cost(AppendHookCost(), true);
    // The stage split is a diagnostic. HookStageTimer::Split is an out-of-line
    // call even when the timer is off, so the hook calls it only in a timed
    // sample of a stage-timing run.
    const bool staged = cost.Sampled() && HookStageTiming();
    HookStageTimer stages(staged);
    const auto address = reinterpret_cast<std::uint64_t>(context);
    // A batch keeps one engine context. An append of another context
    // invalidates the batch below, so its owner is not read.
    const bool foreign = batch->context && batch->context != address;
    AppendInputs in;
    GeometryBatchSpan record;
    PacketOwner owner;
    const auto read = batch->frame == *state->tick
                          ? readAppend(address, reinterpret_cast<std::uint64_t>(packet), batch->renderer, !foreign, in)
                          : AppendRead::Unreadable;
    bool tracked = read != AppendRead::Unreadable;
    if (staged)
        stages.Split(HookStageAppendRead);
    const auto& words = in.words;
    const bool skin = (words[1] & (1ull << 50)) != 0;
    if (tracked)
    {
        if (foreign)
        {
            batch->rigid.invalidate();
            batch->skinned.invalidate();
            tracked = false;
        }
        else
        {
            batch->context = address;
            record.count = (words[1] >> 18) & 0x7fff;
            record.transformIndex = (words[1] >> 33) & 0x1ffff;
            record.global = (words[0] & (1ull << 59)) != 0;
            takeOwner(*state, row, read, in, owner.parent);
            owner.single = record.count == 1 && batch->renderer + 0x574280 + std::uint64_t(record.transformIndex) * 48 ==
                                                    reinterpret_cast<std::uint64_t>(transforms);
            if (staged)
                stages.Split(HookStageAppendTicket);
            if (batch->eager && owner.parent.generation)
            {
                // Array probe run: the probe below needs the element order now,
                // so the owner completes here and the draw only revalidates it.
                ProxyFields fields;
                bump(row.parentMemoMisses);
                try
                {
                    switch (checkOwner(*state, owner.parent, fields))
                    {
                    case OwnerCheck::Owner:
                        if (staged)
                            stages.Split(HookStageAppendFields);
                        record.parent = owner.parent;
                        selectOwner(record, fields, owner.single, skin, ReadControls(), row);
                        break;
                    case OwnerCheck::NoSlot: bump(row.parentNoSlot); break;
                    case OwnerCheck::NoMesh: bump(row.parentNoMesh); break;
                    case OwnerCheck::Changed: bump(row.parentNoHeader); break;
                    }
                }
                catch (...)
                {
                }
            }
            if (staged)
                stages.Split(HookStageAppendSelect);
        }
    }
    HookCostPause pause(cost);
    originalAppend(transforms, packet, c, d, context);
    // Kind 2 is the grouped array path; kinds 3 and 4 are the packet-local
    // family. Both are compared from the array the engine itself read for this
    // packet (see probeArrayOrder). Only an array probe run (Batch::eager) has
    // selected the order kind by this point.
    if (tracked && state && batch && (record.orderKind == 3 || record.orderKind == 4))
    {
        // The call gate below is the last place a kind 3/4 span can be lost
        // before probeArrayOrder. Count the two drops separately so a zero local
        // counter is attributed instead of re-measured. A count==1 span cannot
        // reach this point (orderKind 3/4 is only set for count > 1), so
        // gate_count1 is a must-stay-zero sanity value, not an expected drop.
        auto* counter = skin ? &state->arrayProbeLocalGateSkin
                             : (record.count > 1 ? &state->arrayProbeLocalGatePass
                                                 : &state->arrayProbeLocalGateCount);
        counter->fetch_add(1, std::memory_order_relaxed);
    }
    if (tracked && !skin && state && batch && record.count > 1 &&
        (record.orderKind == 2 || record.orderKind == 3 || record.orderKind == 4))
    {
        // The engine has written this packet's elements by now. Compare the
        // ordinal order with the previous frame of the same array. The grouped
        // container is the audited array expression; the first argument is the
        // array this Append consumed, which is what the packet-local family is
        // hashed from.
        const auto container = batch->renderer + 0x574280 + std::uint64_t(record.transformIndex) * 48;
        probeArrayOrder(*state, record, reinterpret_cast<std::uint64_t>(transforms), container);
    }
    if (!state || !batch)
        return;
    EngineContext after;
    auto& records = skin ? batch->skinned : batch->rigid;
    const auto& before = in.context;
    if (!tracked || batch != currentBatch || batch->frame != *state->tick || !copyAt(address, after) ||
        before.entry != after.entry || before.geometry != after.geometry)
    {
        // If the packet could not be read, even its destination family is unknown.
        batch->rigid.invalidate();
        batch->skinned.invalidate();
        bump(row.rejected);
        return;
    }
    const auto instances = skin ? after.skinCount : after.rigidCount;
    records.append(skin ? before.skinCount : before.rigidCount, instances, record);
    // A kept span is the last one of the view; its owner takes the same index.
    if (const auto kept = records.view(instances); !kept.empty())
        (skin ? batch->skinnedOwners : batch->rigidOwners)[kept.size() - 1] = owner;
    bump(row.appends);
    if (staged)
        stages.Split(HookStageAppendTail);
    if (record.identity)
        bump(row.identities);
}
// Fills a default `result` for a flush of the batch's own context and a kind 0
// geometry record, and leaves it empty otherwise. The hooks keep the Flush on
// their own frame; filling it in place spares the copy of a returned one.
void prepare(Flush& result, void* geometry, void* context, std::uint32_t stride, bool half)
{
    auto* state = activeDrawState.load(std::memory_order_acquire);
    auto* batch = currentBatch;
    EngineContext data;
    EngineGeometry desc;
    if (!state || !batch || batch->context != reinterpret_cast<std::uint64_t>(context) ||
        batch->frame != *state->tick ||
        !readFlush(batch->context, reinterpret_cast<std::uint64_t>(geometry), data, desc) || desc.kind != 0)
        return;
    result.state = state;
    result.batch = batch;
    result.records = stride == 48 ? &batch->rigid : &batch->skinned;
    result.count = stride == 48 ? data.rigidCount : data.skinCount;
    result.source = stride == 48 ? data.rigid : data.skinned;
    result.mesh = desc.mesh;
    result.chunk = desc.chunk;
    result.stride = stride;
    result.indexCount = desc.indexCount >> ((desc.flags & 4) && half ? 1 : 0);
    if (result.records->view(result.count).empty())
        result.invalid = true;
}
void finish(Flush& value)
{
    if (value.records)
        value.records->clear(); // The audited caller clears its matching count after return.
}
void rigid(void* a, void* b, void* c, std::uint32_t globalOrigin, bool half)
{
    const bool idle = DrawHooksIdle();
    const HookCostScope cost(RigidHookCost(), !idle);
    if (idle)
    {
        cost.Stop();
        originalRigid(a, b, c, globalOrigin, half);
        return;
    }
    Flush value;
    prepare(value, b, c, 48, half);
    if (value.records && globalOrigin != UINT32_MAX)
    {
        value.origin = globalOrigin;
        value.uploaded = value.records->globalRange(value.count, globalOrigin);
        value.invalid |= !value.uploaded;
    }
    FlushScope scope(value);
    HookCostPause pause(cost);
    originalRigid(a, b, c, globalOrigin, half);
    finish(value);
}
void skinned(void* a, void* b, void* c, bool half)
{
    const bool idle = DrawHooksIdle();
    const HookCostScope cost(SkinnedHookCost(), !idle);
    if (idle)
    {
        cost.Stop();
        originalSkinned(a, b, c, half);
        return;
    }
    Flush value;
    prepare(value, b, c, 64, half);
    FlushScope scope(value);
    HookCostPause pause(cost);
    originalSkinned(a, b, c, half);
    finish(value);
}
std::uint32_t upload(void* target, void* source, std::uint32_t count, void* allocator)
{
    if (HooksIdle())
        return originalUpload(target, source, count, allocator);
    auto* value = currentFlush;
    std::uint32_t stride = 0;
    const bool observed = value && value->state && !value->invalid && !value->uploaded &&
                          value->source == reinterpret_cast<std::uint64_t>(source) && value->count == count &&
                          copyAt(reinterpret_cast<std::uint64_t>(target) + 0x14, stride) && stride == value->stride;
    const auto result = originalUpload(target, source, count, allocator);
    if (value && value->state)
    {
        if (observed && result != UINT32_MAX && std::uint64_t(result) + count <= UINT32_MAX)
        {
            value->origin = result;
            value->uploaded = true;
        }
        else
            value->invalid = true;
    }
    return result;
}
// Draw-time check of a batch whose owners completed at append (array probe
// run): each distinct owner's mesh and lifetime ticket, as before.
bool validateOwners(const Flush& flush, std::span<const GeometryBatchSpan> spans)
{
    std::uint64_t checkedProxy = 0;
    std::uint32_t checkedSlot = 0, checkedGeneration = 0;
    for (const auto& record : spans)
    {
        const auto& owner = record.parent ? record.parent : record.identity;
        if (!owner)
            continue;
        if (owner.mesh != flush.mesh)
            return false;
        if (owner.proxy == checkedProxy && owner.slot == checkedSlot && owner.generation == checkedGeneration)
            continue;
        if (flush.state->registry->ticket(owner.proxy, owner.slot) != owner.generation)
            return false;
        checkedProxy = owner.proxy;
        checkedSlot = owner.slot;
        checkedGeneration = owner.generation;
    }
    return true;
}
// Draw-time half of the owner check (see PacketOwner). False when an owner's
// lifetime ticket changed since its append or an owner has another mesh than
// the draw; the draw is then refused, as the former draw-time ticket check did.
bool completeOwners(const Flush& flush, std::span<GeometryBatchSpan> spans)
{
    auto& row = flush.state->row();
    const auto controls = ReadControls();
    const bool skin = flush.records == &flush.batch->skinned;
    const auto& owners = skin ? flush.batch->skinnedOwners : flush.batch->rigidOwners;
    const GeometryDrawIdentity* previous = nullptr;
    auto check = OwnerCheck::Changed;
    ProxyFields fields;
    for (std::size_t i = 0; i < spans.size(); ++i)
    {
        const auto& owner = owners[i];
        if (!owner.parent.generation)
            continue; // Counted at append; the span keeps no owner.
        // Consecutive spans can repeat an owner (an array split over several
        // packets). Its header read and ticket are reused, as the former
        // draw-time check reused the ticket.
        if (previous && owner.parent.proxy == previous->proxy && owner.parent.slot == previous->slot &&
            owner.parent.generation == previous->generation && owner.parent.mesh == previous->mesh)
            bump(row.parentMemoHits);
        else
        {
            bump(row.parentMemoMisses);
            check = checkOwner(*flush.state, owner.parent, fields);
            previous = &owner.parent;
        }
        if (check == OwnerCheck::Changed)
        {
            bump(row.parentNoHeader);
            return false;
        }
        if (check != OwnerCheck::Owner)
        {
            bump(check == OwnerCheck::NoSlot ? row.parentNoSlot : row.parentNoMesh);
            continue;
        }
        auto& span = spans[i];
        span.parent = owner.parent;
        if (span.parent.mesh != flush.mesh)
            return false;
        selectOwner(span, fields, owner.single, skin, controls, row);
        if (span.identity)
            bump(row.identities);
    }
    return true;
}
// Owner work of a consumed draw: a draw that reads owners completes them, or
// revalidates those an array probe run completed at the append; a draw that
// does not read them clears what such a run completed. False refuses the draw.
// Kept out of the read below, so its exception frame does not make every draw
// keep its locals on the stack.
bool admitOwners(const Flush& flush, std::span<GeometryBatchSpan> spans, bool owners) noexcept
{
    try
    {
        if (owners)
            return flush.batch->eager ? validateOwners(flush, spans) : completeOwners(flush, spans);
        for (auto& span : spans)
        {
            span.identity = span.parent = {};
            span.orderKind = 0;
            span.originalFirst = 0;
        }
        return true;
    }
    catch (...)
    {
        return false;
    }
}
} // namespace

GeometryDrawView ReadCyberpunkGeometryDraw(const void* sourceReturnAddress, std::uint32_t indexCount,
                                           std::uint32_t instanceCount, std::uint32_t startIndex,
                                           std::int32_t baseVertex, std::uint32_t startInstance,
                                           bool owners) noexcept
{
    // Every return is this one object, so the view is built in the caller's
    // result instead of in a temporary that is then copied out.
    GeometryDrawView view;
    auto* value = currentFlush;
    if (!value || !value->state || value->invalid || value->consumed || !value->uploaded ||
        currentBatch != value->batch || value->batch->frame != *value->state->tick ||
        sourceReturnAddress != value->state->drawReturn || indexCount != value->indexCount ||
        instanceCount != value->count || startIndex || baseVertex || startInstance != value->origin)
        return view;
    const auto records = value->records->view(value->count);
    if (records.empty())
        return view;
    // The view lends the batch's own span storage, which only this flush
    // reads, so the owner fields are completed (or cleared) in place. Only a
    // draw that reads owners or an array probe run has owner work to do.
    if ((owners || value->batch->eager) &&
        !admitOwners(*value, std::span(const_cast<GeometryBatchSpan*>(records.data()), records.size()), owners))
    {
        value->invalid = true; // A repeated read refuses the same draw.
        return view;
    }
    value->consumed = true;
    bump(value->state->row().draws);
    // Built from its parts: assigning the span object made MSVC spill it and
    // reload it with one 16-byte load, which store forwarding cannot serve.
    view.objects = std::span(records.data(), records.size());
    view.mesh = value->mesh;
    view.frame = value->batch->frame;
    view.chunk = value->chunk;
    view.stride = value->stride;
    view.startInstanceLocation = value->origin;
    view.instances = value->count;
    return view;
}
GeometryDrawView ReadCyberpunkGeometryDraw(const void* sourceReturnAddress, std::uint32_t indexCount,
                                           std::uint32_t instanceCount, std::uint32_t startIndex,
                                           std::int32_t baseVertex, std::uint32_t startInstance) noexcept
{
    return ReadCyberpunkGeometryDraw(sourceReturnAddress, indexCount, instanceCount, startIndex, baseVertex,
                                     startInstance, true);
}
namespace
{
// Diagnostics only: relaxed atomics, no allocation and no lock. Only the
// rejection path stores, so an accepted mesh read stays as cheap as before.
std::atomic<std::uint64_t> shapeRejectedCount = 0, shapeNoFlushCount = 0, shapeNoBatchCount = 0,
                           shapeEmptyObjectsCount = 0, shapeMeshChunkCount = 0, shapeFrameCount = 0,
                           shapeInstancesCount = 0, shapeRecordsCount = 0, shapeHeaderCount = 0,
                           shapeBuffersCount = 0, shapeChunkReadCount = 0, shapeUnstableChunkCount = 0,
                           shapeHeaderMovedCount = 0, shapeBuffersMovedCount = 0, shapeFieldsCount = 0;
// `fields` is the sum of these six; the checks keep their original order so
// exactly one of them is charged per rejection and the split cannot drift.
std::atomic<std::uint64_t> shapeFieldsVerticesCount = 0, shapeFieldsIndicesCount = 0,
                           shapeFieldsStreamsCount = 0, shapeFieldsStreamRangeCount = 0,
                           shapeFieldsIndexTypeCount = 0, shapeFieldsIndexOffsetCount = 0;

// Bounded value samples for the first field rejections, so the counters can be
// read together with the numbers that produced them.
constexpr std::size_t kShapeSampleCapacity = 4;
std::array<CyberpunkShapeSample, kShapeSampleCapacity> shapeSamples {};
std::array<std::atomic<bool>, kShapeSampleCapacity> shapeSampleReady {};
std::atomic<std::uint32_t> shapeSampleCursor { 0 };

CyberpunkMeshShape rejectShape(std::atomic<std::uint64_t>& reason) noexcept
{
    shapeRejectedCount.fetch_add(1, std::memory_order_relaxed);
    reason.fetch_add(1, std::memory_order_relaxed);
    return {};
}

void recordShapeSample(const CyberpunkShapeSample& value) noexcept
{
    const auto slot = shapeSampleCursor.fetch_add(1, std::memory_order_relaxed);
    if (slot >= kShapeSampleCapacity)
        return;
    shapeSamples[slot] = value;
    shapeSampleReady[slot].store(true, std::memory_order_release);
}

CyberpunkMeshShape rejectShapeFields(std::atomic<std::uint64_t>& reason, std::uint32_t reasonIndex,
                                     const CyberpunkShapeSample& sample) noexcept
{
    shapeRejectedCount.fetch_add(1, std::memory_order_relaxed);
    shapeFieldsCount.fetch_add(1, std::memory_order_relaxed);
    reason.fetch_add(1, std::memory_order_relaxed);
    auto recorded = sample;
    recorded.reason = reasonIndex;
    recordShapeSample(recorded);
    return {};
}
} // namespace

CyberpunkMeshShape ReadCyberpunkMeshShape(const GeometryDrawView& draw) noexcept
{
    // The accepted set of this function is unchanged: the same conditions are
    // split in their original evaluation order so each rejection gets exactly
    // one reason without changing which draws are accepted.
    const auto* flush = currentFlush;
    if (!flush || !flush->consumed || flush->invalid)
        return rejectShape(shapeNoFlushCount);
    if (!currentBatch)
        return rejectShape(shapeNoBatchCount);
    if (draw.objects.empty())
        return rejectShape(shapeEmptyObjectsCount);
    if (draw.mesh != flush->mesh || draw.chunk != flush->chunk)
        return rejectShape(shapeMeshChunkCount);
    if (draw.frame != currentBatch->frame || draw.frame != *flush->state->tick)
        return rejectShape(shapeFrameCount);
    if (draw.instances != flush->count)
        return rejectShape(shapeInstancesCount);
    if (draw.objects.data() != flush->records->view(flush->count).data())
        return rejectShape(shapeRecordsCount);
    // rendChunk layout is also exposed by the game's reflection (RED4ext SDK).
    // Read only this chunk, twice, instead of scanning meshes or GPU contents.
    struct Header { std::uint64_t data; std::uint32_t capacity, count; };
    Header first {}, second {};
    std::array<std::uint32_t, 2> buffers {}, buffersAfter {};
    std::array<std::byte, 0xf8> chunk {}, after {};
    if (!copyAt(draw.mesh + 0xa0, first) || !first.data || first.count > first.capacity ||
        first.capacity > 4096 || draw.chunk >= first.count)
        return rejectShape(shapeHeaderCount);
    if (!copyAt(draw.mesh + 0x30, buffers) || !buffers[0] || !buffers[1] || buffers[0] > 65536 ||
        buffers[1] > 65536)
        return rejectShape(shapeBuffersCount);
    const auto address = first.data + std::uint64_t(draw.chunk) * chunk.size();
    if (address < first.data || !copyAt(address, chunk) || !copyAt(address, after))
        return rejectShape(shapeChunkReadCount);
    if (chunk != after)
        return rejectShape(shapeUnstableChunkCount);
    if (!copyAt(draw.mesh + 0xa0, second) || memcmp(&first, &second, sizeof(first)))
        return rejectShape(shapeHeaderMovedCount);
    if (!copyAt(draw.mesh + 0x30, buffersAfter) || buffers != buffersAfter)
        return rejectShape(shapeBuffersMovedCount);
    CyberpunkMeshShape result;
    result.chunkAddress = address;
    result.vertexBuffer = buffers[0];
    result.indexBuffer = buffers[1];
    memcpy(result.streamOffsets.data(), chunk.data() + 0xb8, sizeof(result.streamOffsets));
    memcpy(&result.streams, chunk.data() + 0xcc, 4);
    memcpy(&result.indexOffset, chunk.data() + 0xd4, 4);
    memcpy(&result.indices, chunk.data() + 0xe8, 4);
    std::uint16_t vertices = 0;
    memcpy(&vertices, chunk.data() + 0xec, 2);
    result.vertices = vertices;
    result.indexType = std::to_integer<std::uint8_t>(chunk[0xd0]);
    result.vertexFactory = std::to_integer<std::uint8_t>(chunk[0xf4]);
    // Split of the single field condition, in its original short-circuit
    // order. The sample is a local copy; nothing shared is stored on the
    // accepted path.
    CyberpunkShapeSample sample;
    sample.mesh = draw.mesh;
    sample.chunk = draw.chunk;
    sample.vertices = result.vertices;
    sample.indices = result.indices;
    sample.flushIndices = flush->indexCount;
    sample.streams = result.streams;
    sample.indexOffset = result.indexOffset;
    sample.indexType = result.indexType;
    if (!result.vertices)
        return rejectShapeFields(shapeFieldsVerticesCount, 0, sample);
    if (result.indices != flush->indexCount)
        return rejectShapeFields(shapeFieldsIndicesCount, 1, sample);
    if (!result.streams)
        return rejectShapeFields(shapeFieldsStreamsCount, 2, sample);
    if (result.streams > 5)
        return rejectShapeFields(shapeFieldsStreamRangeCount, 3, sample);
    if (result.indexType > 1)
        return rejectShapeFields(shapeFieldsIndexTypeCount, 4, sample);
    if (result.indexOffset % (result.indexType ? 2 : 4))
        return rejectShapeFields(shapeFieldsIndexOffsetCount, 5, sample);
    return result;
}
bool InitializeCyberpunkDraws(HMODULE executable) noexcept
{
    try
    {
        static std::mutex startup;
        std::lock_guard lock(startup);
        if (activeDrawState.load(std::memory_order_acquire))
            return true;
        auto registry = GetCyberpunkObjects();
        const auto* layout = GetCyberpunkLayout(executable);
        if (!registry || !layout)
            return false;
        auto* base = reinterpret_cast<unsigned char*>(executable);
        auto state = std::make_unique<EngineDrawState>();
        state->registry = std::move(registry);
        state->tick = reinterpret_cast<const volatile std::uint32_t*>(base + layout->tick);
        state->rendererGlobal = base + layout->rendererGlobal;
        state->drawReturn = base + layout->drawReturn;
        HMODULE resident = nullptr;
        if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
                                reinterpret_cast<LPCWSTR>(&InitializeCyberpunkDraws), &resident))
            return false;
        DetourThreads threads;
        if (!threads.gather() || DetourTransactionBegin() != NO_ERROR)
            return false;
        originalRun = reinterpret_cast<Run>(base + layout->functions[CyberpunkLayout::Run]);
        originalAppend = reinterpret_cast<Append>(base + layout->functions[CyberpunkLayout::Append]);
        originalRigid = reinterpret_cast<Rigid>(base + layout->functions[CyberpunkLayout::Rigid]);
        originalSkinned = reinterpret_cast<Skinned>(base + layout->functions[CyberpunkLayout::Skinned]);
        originalUpload = reinterpret_cast<Upload>(base + layout->functions[CyberpunkLayout::Upload]);
        bool okay = true;
        const auto attach = [&](auto& original, auto hook)
        {
            okay = DetourAttach(reinterpret_cast<PVOID*>(&original), reinterpret_cast<PVOID>(hook)) == NO_ERROR && okay;
        };
        attach(originalRun, &run);
        attach(originalAppend, &append);
        attach(originalRigid, &rigid);
        attach(originalSkinned, &skinned);
        attach(originalUpload, &upload);
        if (!okay || !threads.enlist())
        {
            DetourTransactionAbort();
            return false;
        }
        activeDrawState.store(state.get(), std::memory_order_release);
        if (DetourTransactionCommit() != NO_ERROR)
        {
            activeDrawState.store(nullptr, std::memory_order_release);
            return false;
        }
        state.release();
        return true;
    }
    catch (...)
    {
        return false;
    }
}
CyberpunkDrawStatus GetCyberpunkDrawStatus() noexcept
{
    auto* state = activeDrawState.load(std::memory_order_acquire);
    if (!state)
        return {};
    CyberpunkDrawStatus value;
    value.active = true;
    // Sum the per-thread rows. Every row has a single writer, so a relaxed
    // load is a complete value for that thread; the totals are monotonic.
    for (const auto& row : state->rows)
    {
        value.batches += row.batches.load(std::memory_order_relaxed);
        value.appends += row.appends.load(std::memory_order_relaxed);
        value.identities += row.identities.load(std::memory_order_relaxed);
        value.draws += row.draws.load(std::memory_order_relaxed);
        value.rejected += row.rejected.load(std::memory_order_relaxed);
        value.parentNoFlag += row.parentNoFlag.load(std::memory_order_relaxed);
        value.parentNoEntry += row.parentNoEntry.load(std::memory_order_relaxed);
        value.parentNoTicket += row.parentNoTicket.load(std::memory_order_relaxed);
        value.parentNoSlot += row.parentNoSlot.load(std::memory_order_relaxed);
        value.parentNoMesh += row.parentNoMesh.load(std::memory_order_relaxed);
        value.parentNoHeader += row.parentNoHeader.load(std::memory_order_relaxed);
        value.parentNoSelection += row.parentNoSelection.load(std::memory_order_relaxed);
        value.parentNoSelectionGrouped += row.parentNoSelectionGrouped.load(std::memory_order_relaxed);
        value.parentNoSelectionNonGlobal += row.parentNoSelectionNonGlobal.load(std::memory_order_relaxed);
        value.parentNoSelectionRange += row.parentNoSelectionRange.load(std::memory_order_relaxed);
        value.parentNonGlobalCount1 += row.parentNonGlobalCount1.load(std::memory_order_relaxed);
        value.parentNonGlobalCountMore += row.parentNonGlobalCountMore.load(std::memory_order_relaxed);
        value.parentNonGlobalCountMoreSkin += row.parentNonGlobalCountMoreSkin.load(std::memory_order_relaxed);
        value.parentSeeded += row.parentSeeded.load(std::memory_order_relaxed);
        value.parentMemoHits += row.parentMemoHits.load(std::memory_order_relaxed);
        value.parentMemoMisses += row.parentMemoMisses.load(std::memory_order_relaxed);
    }
    value.arrayProbeCompared = state->arrayProbeCompared.load();
    value.arrayProbePermuted = state->arrayProbePermuted.load();
    value.arrayProbeChanged = state->arrayProbeChanged.load();
    value.arrayProbeSameAddress = state->arrayProbeSameAddress.load();
    value.arrayProbeDistinctAddress = state->arrayProbeDistinctAddress.load();
    value.arrayProbeGrouped = state->arrayProbeGrouped.load();
    value.arrayProbeLocal = state->arrayProbeLocal.load();
    value.arrayProbeLocalCompared = state->arrayProbeLocalCompared.load();
    value.arrayProbeLocalPermuted = state->arrayProbeLocalPermuted.load();
    value.arrayProbeLocalChanged = state->arrayProbeLocalChanged.load();
    value.arrayProbeLocalSameAddress = state->arrayProbeLocalSameAddress.load();
    value.arrayProbeLocalDistinctAddress = state->arrayProbeLocalDistinctAddress.load();
    value.arrayProbeLocalUnreadable = state->arrayProbeLocalUnreadable.load();
    value.arrayProbeLocalBaseMoved = state->arrayProbeLocalBaseMoved.load();
    value.arrayProbeLocalGatePass = state->arrayProbeLocalGatePass.load();
    value.arrayProbeLocalGateCount = state->arrayProbeLocalGateCount.load();
    value.arrayProbeLocalGateSkin = state->arrayProbeLocalGateSkin.load();
    return value;
}
std::uint32_t ReadCyberpunkDrawFrame() noexcept
{
    auto* state = activeDrawState.load(std::memory_order_acquire);
    return state && state->tick ? *state->tick : 0;
}
CyberpunkShapeStats ReadCyberpunkShapeStats() noexcept
{
    CyberpunkShapeStats value;
    value.rejected = shapeRejectedCount.load(std::memory_order_relaxed);
    value.noFlush = shapeNoFlushCount.load(std::memory_order_relaxed);
    value.noBatch = shapeNoBatchCount.load(std::memory_order_relaxed);
    value.emptyObjects = shapeEmptyObjectsCount.load(std::memory_order_relaxed);
    value.meshChunk = shapeMeshChunkCount.load(std::memory_order_relaxed);
    value.frame = shapeFrameCount.load(std::memory_order_relaxed);
    value.instances = shapeInstancesCount.load(std::memory_order_relaxed);
    value.records = shapeRecordsCount.load(std::memory_order_relaxed);
    value.header = shapeHeaderCount.load(std::memory_order_relaxed);
    value.buffers = shapeBuffersCount.load(std::memory_order_relaxed);
    value.chunkRead = shapeChunkReadCount.load(std::memory_order_relaxed);
    value.unstableChunk = shapeUnstableChunkCount.load(std::memory_order_relaxed);
    value.headerMoved = shapeHeaderMovedCount.load(std::memory_order_relaxed);
    value.buffersMoved = shapeBuffersMovedCount.load(std::memory_order_relaxed);
    value.fields = shapeFieldsCount.load(std::memory_order_relaxed);
    value.fieldsVertices = shapeFieldsVerticesCount.load(std::memory_order_relaxed);
    value.fieldsIndices = shapeFieldsIndicesCount.load(std::memory_order_relaxed);
    value.fieldsStreams = shapeFieldsStreamsCount.load(std::memory_order_relaxed);
    value.fieldsStreamRange = shapeFieldsStreamRangeCount.load(std::memory_order_relaxed);
    value.fieldsIndexType = shapeFieldsIndexTypeCount.load(std::memory_order_relaxed);
    value.fieldsIndexOffset = shapeFieldsIndexOffsetCount.load(std::memory_order_relaxed);
    return value;
}
std::size_t ReadCyberpunkShapeSamples(CyberpunkShapeSample* out, std::size_t capacity) noexcept
{
    if (!out)
        return 0;
    std::size_t count = 0;
    for (std::size_t slot = 0; slot < kShapeSampleCapacity && count < capacity; ++slot)
    {
        if (!shapeSampleReady[slot].load(std::memory_order_acquire))
            break;
        out[count++] = shapeSamples[slot];
    }
    return count;
}
} // namespace GlassFg
