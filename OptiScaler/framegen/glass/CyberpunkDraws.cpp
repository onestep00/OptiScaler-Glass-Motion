#include "pch.h"
#include "CyberpunkDraws.h"
#include "CyberpunkObjects.h"
#include "CyberpunkSurfacePass.h"
#include "DetourThreads.h"
#include <bit>
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
    std::atomic<std::uint64_t> batches = 0, appends = 0, identities = 0, draws = 0, rejected = 0;
};
std::atomic<EngineDrawState*> activeDrawState = nullptr;
thread_local Batch* currentBatch = nullptr;

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
        ++state->batches;
        if (!currentBatch)
            ++state->rejected;
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
    RunScope scope(activeDrawState.load(std::memory_order_acquire));
    originalRun(a, b, c);
}
void append(void* transforms, void* packet, std::uintptr_t c, std::uintptr_t d, void* context)
{
    auto* state = activeDrawState.load(std::memory_order_acquire);
    auto* batch = currentBatch;
    EngineContext before;
    std::array<std::uint64_t, 2> words {};
    GeometryBatchSpan record;
    bool tracked = state && batch && batch->frame == *state->tick &&
                   copyAt(reinterpret_cast<std::uint64_t>(context), before) &&
                   copyAt(reinterpret_cast<std::uint64_t>(packet), words);
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
            std::uint64_t encoded = 0, mesh = 0;
            std::uint32_t actualSlot = UINT32_MAX;
            const auto source = batch->renderer + 0x574280 + std::uint64_t(record.transformIndex) * 48;
            if (record.count == 1 && (words[1] & (1ull << 51)) && index < 131072 &&
                source == reinterpret_cast<std::uint64_t>(transforms) &&
                before.entry == batch->renderer + 0x274248 + std::uint64_t(index) * 24 &&
                copyAt(before.entry, encoded) && copyAt(before.geometry, geometry) && geometry.kind == 0)
            {
                const auto proxy = encoded & 0x00ffffffffffffffull;
                if (copyAt(proxy + 0x98, actualSlot) && actualSlot == index && copyAt(proxy + 0xd8, mesh) && mesh &&
                    mesh == geometry.mesh)
                {
                    try
                    {
                        const auto generation = state->registry->ticket(proxy, index);
                        if (generation)
                            record.identity = { proxy, mesh, index, generation };
                    }
                    catch (...)
                    {
                    }
                }
            }
        }
    }
    originalAppend(transforms, packet, c, d, context);
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
        ++state->rejected;
        return;
    }
    records.append(skin ? before.skinCount : before.rigidCount, skin ? after.skinCount : after.rigidCount, record);
    ++state->appends;
    if (record.identity)
        ++state->identities;
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
    auto value = prepare(b, c, 48, half);
    if (value.records && globalOrigin != UINT32_MAX)
    {
        value.origin = globalOrigin;
        value.uploaded = value.records->globalRange(value.count, globalOrigin);
        value.invalid |= !value.uploaded;
    }
    FlushScope scope(value);
    originalRigid(a, b, c, globalOrigin, half);
    finish(value);
}
void skinned(void* a, void* b, void* c, bool half)
{
    auto value = prepare(b, c, 64, half);
    FlushScope scope(value);
    originalSkinned(a, b, c, half);
    finish(value);
}
std::uint32_t upload(void* target, void* source, std::uint32_t count, void* allocator)
{
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
bool matchesCode(const unsigned char* code, unsigned bytes, std::uint64_t expected)
{
    std::uint64_t value = 14695981039346656037ull;
    for (unsigned i = 0; i < bytes; ++i)
        value = (value ^ code[i]) * 1099511628211ull;
    return value == expected;
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
        for (const auto& record : records)
            if (record.identity && (record.identity.mesh != value->mesh ||
                                    value->state->registry->ticket(record.identity.proxy, record.identity.slot) !=
                                        record.identity.generation))
                return {};
    }
    catch (...)
    {
        return {};
    }
    value->consumed = true;
    ++value->state->draws;
    return { records, value->mesh, value->batch->frame, value->chunk, value->stride, value->origin, value->count };
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
        CyberpunkSurfacePass admitted;
        if (!registry || !admitted.initialize(executable, nullptr))
            return false;
        auto* base = reinterpret_cast<unsigned char*>(executable);
        if (!matchesCode(base + 0x1f1208, 1812, 0x945f222f10bba789ull) ||
            !matchesCode(base + 0x1f1a88, 953, 0x85e35e0963682f91ull) ||
            !matchesCode(base + 0x1f1fa8, 237, 0x2c394f6b77326845ull) ||
            !matchesCode(base + 0x1f020c, 214, 0x557f85e81a081b90ull) ||
            !matchesCode(base + 0x1f3e40, 219, 0x308c3423d7684bcdull) ||
            !matchesCode(base + 0x1f2098, 585, 0x4d25972c8945d3e0ull))
            return false;
        auto state = std::make_unique<EngineDrawState>();
        state->registry = std::move(registry);
        state->tick = reinterpret_cast<const volatile std::uint32_t*>(base + 0x3438a30);
        state->rendererGlobal = base + 0x3427c00;
        state->drawReturn = base + 0x1f22c2;
        HMODULE resident = nullptr;
        if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
                                reinterpret_cast<LPCWSTR>(&InitializeCyberpunkDraws), &resident))
            return false;
        DetourThreads threads;
        if (!threads.gather() || DetourTransactionBegin() != NO_ERROR)
            return false;
        originalRun = reinterpret_cast<Run>(base + 0x1f1208);
        originalAppend = reinterpret_cast<Append>(base + 0x1f1a88);
        originalRigid = reinterpret_cast<Rigid>(base + 0x1f1fa8);
        originalSkinned = reinterpret_cast<Skinned>(base + 0x1f020c);
        originalUpload = reinterpret_cast<Upload>(base + 0x1f3e40);
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
    return state ? CyberpunkDrawStatus { true,
                                         state->batches.load(),
                                         state->appends.load(),
                                         state->identities.load(),
                                         state->draws.load(),
                                         state->rejected.load() }
                 : CyberpunkDrawStatus {};
}
} // namespace GlassFg
