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

struct Batch
{
    GeometryDrawBatch rigid, skinned;
    std::uint64_t context = 0, renderer = 0;
    std::uint32_t frame = 0;
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
        // Context parent memo (see ParentMemoEntry). Hits skip the 0x90-byte
        // proxy field read and two of the three lifetime tickets per packet.
        std::atomic<std::uint64_t> parentMemoHits = 0, parentMemoMisses = 0;
    };
    static constexpr unsigned RowCount = 64;
    std::array<Row, RowCount> rows {};
    // The calling thread's row for this state. Never call from the report
    // thread expecting its own totals; use the summed status below.
    Row& row() noexcept;
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
EngineDrawState::Row& EngineDrawState::row() noexcept
{
    if (rowState != this)
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
    }
    return rows[rowIndex];
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

template <typename T> bool copyAt(std::uint64_t address, T& value) noexcept
{
    static_assert(std::is_trivially_copyable_v<T>);
    __try
    {
        if (address < 0x10000 || address > 0x7fffffffffff - sizeof(T))
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
// the checks below used to make. A guarded copy is a non-inlined call, and this
// runs for every engine packet, not only for the captured ones. The bytes are
// the same bytes the individual reads covered.
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
// The memo drops the 0x90-byte proxy header read, but the array fields inside
// that header are rewritten by the engine's own update path. The group update
// (0x1e8778) republishes +0x108/+0x110/+0x114 and the grouped flag at +0xea
// without touching the registry ticket, so a packet that resolves the same
// index later in the same batch can see different array state. A memo hit
// therefore re-reads the 0x2e bytes that hold every field the selection uses
// and keeps the memo only while all of them are unchanged. The mesh and slot
// fields stay validated through the geometry record and the lifetime ticket.
bool confirmProxyFields(std::uint64_t proxy, const ProxyFields& value) noexcept
{
    static_assert(0x108 - 0xea == 0x1e);
    static_assert(0x110 - 0xea == 0x26);
    static_assert(0x114 - 0xea == 0x2a);
    std::array<std::byte, 0x2e> tail {};
    if (!copyAt(proxy + 0xea, tail))
        return false;
    std::uint16_t flags = 0;
    std::uint64_t transforms = 0;
    std::uint32_t instanceCount = 0, globalStart = 0;
    std::memcpy(&flags, tail.data(), sizeof(flags));
    std::memcpy(&transforms, tail.data() + 0x1e, sizeof(transforms));
    std::memcpy(&instanceCount, tail.data() + 0x26, sizeof(instanceCount));
    std::memcpy(&globalStart, tail.data() + 0x2a, sizeof(globalStart));
    return flags == value.flags && transforms == value.transforms && instanceCount == value.instanceCount &&
           globalStart == value.globalStart;
}
// Packet owner memo.
//
// A packet's parent (proxy, mesh, slot, lifetime generation) is a function of
// the engine context it belongs to plus the packet's own transform index. The
// engine appends thousands of packets per frame and a batch shares one context,
// so the same checks were repeated for every packet: read the entry's proxy
// pointer, read the 0x30-byte geometry record, read the 0x90 bytes of proxy
// fields, and take two lifetime tickets.
//
// The memo keeps the resolved parent plus the fields the selection needs. A hit
// still re-reads the entry, the geometry record and one lifetime ticket, so any
// setter, re-registration or geometry swap invalidates it exactly like the full
// path does. It also re-reads the array state in the proxy header, because the
// engine's group update rewrites those fields without touching the ticket. It
// only drops the slot/mesh bytes and the second ticket, which the geometry
// record and the first ticket already cover.
struct ParentMemoEntry
{
    std::uint64_t context = 0, entry = 0, geometry = 0;
    const Batch* batch = nullptr;
    std::uint32_t tick = 0, index = UINT32_MAX;
    GeometryDrawIdentity parent;
    ProxyFields fields;
    bool valid = false;
};
thread_local ParentMemoEntry parentMemo[4];
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
    std::uint64_t context = 0, source = 0, geometry = 0, mesh = 0;
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
    HookStageTimer stages(cost.Sampled() && HookStageTiming());
    EngineContext before;
    std::array<std::uint64_t, 2> words {};
    GeometryBatchSpan record;
    bool tracked = batch->frame == *state->tick &&
                   copyAt(reinterpret_cast<std::uint64_t>(context), before) &&
                   copyAt(reinterpret_cast<std::uint64_t>(packet), words);
    stages.Split(HookStageAppendRead);
    const bool skin = (words[1] & (1ull << 50)) != 0;
    if (tracked)
    {
        if (batch->context && batch->context != reinterpret_cast<std::uint64_t>(context))
        {
            batch->rigid.invalidate();
            batch->skinned.invalidate();
            tracked = false;
        }
        else
        {
            batch->context = reinterpret_cast<std::uint64_t>(context);
            record.count = (words[1] >> 18) & 0x7fff;
            record.transformIndex = (words[1] >> 33) & 0x1ffff;
            record.global = (words[0] & (1ull << 59)) != 0;
            const auto index = static_cast<std::uint32_t>(words[1] & 0x3ffff);
            EngineGeometry geometry;
            std::uint64_t encoded = 0;
            const auto source = batch->renderer + 0x574280 + std::uint64_t(record.transformIndex) * 48;
            const bool flagged = (words[1] & (1ull << 51)) != 0;
            ProxyFields fields;
            auto& memo = parentMemo[index & 3u];
            bool resolved = false;
            if (!flagged)
                bump(row.parentNoFlag);
            else if (memo.valid && memo.index == index && memo.context == batch->context && memo.batch == batch &&
                     memo.tick == batch->frame && memo.entry == before.entry && memo.geometry == before.geometry &&
                     copyAt(before.entry, encoded) &&
                     (encoded & 0x00ffffffffffffffull) == memo.parent.proxy && copyAt(before.geometry, geometry) &&
                     geometry.kind == 0 && geometry.mesh == memo.fields.mesh &&
                     state->registry->ticket(memo.parent.proxy, memo.parent.slot) == memo.parent.generation &&
                     confirmProxyFields(memo.parent.proxy, memo.fields))
            {
                // Same context, entry and geometry record as a packet that
                // already resolved this slot, and the lifetime ticket still
                // matches, so the proxy fields cannot have changed between the
                // two packets. The 0x90-byte field read and the second ticket
                // are not repeated.
                bump(row.parentMemoHits);
                fields = memo.fields;
                record.parent = memo.parent;
                resolved = true;
            }
            else if (index >= 131072 ||
                     before.entry != batch->renderer + 0x274248 + std::uint64_t(index) * 24 ||
                     !copyAt(before.entry, encoded) || !copyAt(before.geometry, geometry) ||
                     geometry.kind != 0)
                bump(row.parentNoEntry);
            else
            {
                bump(row.parentMemoMisses);
                const auto proxy = encoded & 0x00ffffffffffffffull;
                try
                {
                    // Bracket header reads with the lifetime/array mutation
                    // ticket. A setter active or completed between these reads
                    // cannot publish a mixed old header with a new generation.
                    auto generation = state->registry->ticket(proxy, index);
                    if (!generation && seedObject(*state, proxy))
                    {
                        generation = state->registry->ticket(proxy, index);
                        if (generation) bump(row.parentSeeded);
                    }
                    if (!generation)
                        bump(row.parentNoTicket);
                    else
                    {
                        stages.Split(HookStageAppendTicket);
                        if (!readProxyFields(proxy, fields) || fields.slot != index)
                            bump(row.parentNoSlot);
                        else if (!fields.mesh || fields.mesh != geometry.mesh)
                            bump(row.parentNoMesh);
                        else if (state->registry->ticket(proxy, index) != generation)
                            bump(row.parentNoHeader);
                        else
                        {
                            stages.Split(HookStageAppendFields);
                            record.parent = { proxy, fields.mesh, index, generation };
                            memo.context = batch->context;
                            memo.entry = before.entry;
                            memo.geometry = before.geometry;
                            memo.batch = batch;
                            memo.tick = batch->frame;
                            memo.index = index;
                            memo.parent = record.parent;
                            memo.fields = fields;
                            memo.valid = true;
                            resolved = true;
                        }
                    }
                }
                catch (...) {}
            }
            if (resolved)
                try
                {
                    if (record.count == 1 && source == reinterpret_cast<std::uint64_t>(transforms) &&
                        !fields.transforms && !fields.instanceCount && fields.globalStart == UINT32_MAX)
                        record.identity = record.parent;
                    CyberpunkInstanceSelection selection;
                    if (selection.resolveGlobalPacket(record.global, record.transformIndex, record.count,
                                                      fields.globalStart, fields.instanceCount, fields.flags))
                    {
                        record.orderKind = 1;
                        record.originalFirst = static_cast<std::uint16_t>(selection.linearFirst);
                    }
                    else
                    {
                        bump(row.parentNoSelection);
                        const auto code = CyberpunkInstanceSelection::rejectCode(
                            record.global, record.transformIndex, record.count, fields.globalStart,
                            fields.instanceCount, fields.flags);
                        switch (code)
                        {
                        case 1:
                            bump(row.parentNoSelectionGrouped);
                            // Grouped update array. The engine repacks the source
                            // elements into this same allocation in group order, so
                            // the packet ordinal is the element's position in the
                            // group. It is only used together with the array's
                            // observed lifetime generation, which invalidates the
                            // history whenever the array is mutated.
                            if (ReadControls().groupedOrder && record.count > 1)
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
                            if (ReadControls().arrayProbe)
                            {
                                const auto counter = record.count == 1
                                                         ? &row.parentNonGlobalCount1
                                                         : (skin ? &row.parentNonGlobalCountMoreSkin
                                                                 : &row.parentNonGlobalCountMore);
                                bump(*counter);
                            }
                            // Packet-local instanced selection: particles and other
                            // instanced transparency. The engine keeps the element
                            // bytes in the packet itself and exposes no source
                            // index. Kind 3 marks the span as a probe target; kind 4
                            // authorizes the packet ordinal once the order probe has
                            // shown it is stable for this family.
                            if (record.count > 1)
                            {
                                record.orderKind = ReadControls().packetLocalOrder ? 4 : 3;
                                record.originalFirst = 0;
                            }
                            break;
                        default: bump(row.parentNoSelectionRange); break;
                        }
                    }
                }
                catch (...) {}
            stages.Split(HookStageAppendSelect);
        }
    }
    HookCostPause pause(cost);
    originalAppend(transforms, packet, c, d, context);
    // Kind 2 is the grouped array path; kinds 3 and 4 are the packet-local
    // family. Both are compared from the array the engine itself read for this
    // packet (see probeArrayOrder).
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
    if (!tracked || batch != currentBatch || batch->frame != *state->tick ||
        !copyAt(reinterpret_cast<std::uint64_t>(context), after) || before.entry != after.entry ||
        before.geometry != after.geometry)
    {
        // If the packet could not be read, even its destination family is unknown.
        batch->rigid.invalidate();
        batch->skinned.invalidate();
        bump(row.rejected);
        return;
    }
    records.append(skin ? before.skinCount : before.rigidCount, skin ? after.skinCount : after.rigidCount, record);
    bump(row.appends);
    stages.Split(HookStageAppendTail);
    if (record.identity)
        bump(row.identities);
}
Flush prepare(void* geometry, void* context, std::uint32_t stride, bool half)
{
    Flush result;
    auto* state = activeDrawState.load(std::memory_order_acquire);
    auto* batch = currentBatch;
    EngineContext data;
    EngineGeometry desc;
    if (!state || !batch || batch->context != reinterpret_cast<std::uint64_t>(context) ||
        batch->frame != *state->tick || !copyAt(batch->context, data) ||
        data.geometry != reinterpret_cast<std::uint64_t>(geometry) || !copyAt(data.geometry, desc) || desc.kind != 0)
        return result;
    result.state = state;
    result.batch = batch;
    result.context = batch->context;
    result.records = stride == 48 ? &batch->rigid : &batch->skinned;
    result.count = stride == 48 ? data.rigidCount : data.skinCount;
    result.source = stride == 48 ? data.rigid : data.skinned;
    result.geometry = data.geometry;
    result.mesh = desc.mesh;
    result.chunk = desc.chunk;
    result.stride = stride;
    result.indexCount = desc.indexCount >> ((desc.flags & 4) && half ? 1 : 0);
    if (result.records->view(result.count).empty())
        result.invalid = true;
    return result;
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
    auto value = prepare(b, c, 48, half);
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
    auto value = prepare(b, c, 64, half);
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
} // namespace

GeometryDrawView ReadCyberpunkGeometryDraw(const void* sourceReturnAddress, std::uint32_t indexCount,
                                           std::uint32_t instanceCount, std::uint32_t startIndex,
                                           std::int32_t baseVertex, std::uint32_t startInstance) noexcept
{
    auto* value = currentFlush;
    if (!value || !value->state || value->invalid || value->consumed || !value->uploaded ||
        currentBatch != value->batch || value->batch->frame != *value->state->tick ||
        sourceReturnAddress != value->state->drawReturn || indexCount != value->indexCount ||
        instanceCount != value->count || startIndex || baseVertex || startInstance != value->origin)
        return {};
    const auto records = value->records->view(value->count);
    if (records.empty())
        return {};
    try
    {
        // Ordinary identity is copied from parent at append. Validate its
        // lifetime once per distinct owner, not once per span: the spans of one
        // draw almost always repeat the same array parent, and the registry
        // ticket is a random read into a multi-megabyte table. This memo is a
        // pure cache, every distinct owner is still checked before it is used.
        std::uint64_t checkedProxy = 0;
        std::uint32_t checkedSlot = 0, checkedGeneration = 0;
        for (const auto& record : records)
        {
            const auto& owner = record.parent ? record.parent : record.identity;
            if (!owner)
                continue;
            if (owner.mesh != value->mesh)
                return {};
            if (owner.proxy == checkedProxy && owner.slot == checkedSlot &&
                owner.generation == checkedGeneration)
                continue;
            if (value->state->registry->ticket(owner.proxy, owner.slot) != owner.generation)
                return {};
            checkedProxy = owner.proxy;
            checkedSlot = owner.slot;
            checkedGeneration = owner.generation;
        }
    }
    catch (...)
    {
        return {};
    }
    value->consumed = true;
    bump(value->state->row().draws);
    return { records, value->mesh, value->batch->frame, value->chunk, value->stride, value->origin, value->count };
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
