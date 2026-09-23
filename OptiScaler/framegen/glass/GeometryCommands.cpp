#include "pch.h"
#include "GeometryCommands.h"
#include "GeometryCreation.h"
#include "GeometryPipelineCache.h"
#include "GeometryDrawCapture.h"
#include "ExperimentDrawBridge.h"
#include "ExperimentCensusBridge.h"
#include "CyberpunkDraws.h"
#include "CommandLifetime.h"
#include "IndirectBindings.h"
#include "DetourThreads.h"
#include "GeometryGateTrace.h"
#include "GlassHookProbe.h"
#include <hooks/Hook_Utils.h>
#include <array>
#include <atomic>
#include <mutex>
#include <intrin.h>

namespace GlassFg
{
namespace
{
using Microsoft::WRL::ComPtr;
using Command = ID3D12GraphicsCommandList;
template <auto Method> using MethodType = typename rewrite_signature<decltype(Method)>::type;
// A hook's find() memo hit and an indexed draw's lease memo read only the
// fields up to lifetime's state pointer (its first word), which fill the
// record's first cache line; the cold fields follow. In the former order the
// same reads spread over two to four lines.
// The aligned records and rows are padded on purpose; /W4 reports the
// padding as C4324.
#pragma warning(push)
#pragma warning(disable : 4324)
struct alignas(64) Record
{
    std::atomic<Command*> key = nullptr;
    bool open = false;
    // The replayed root bindings live in a parallel array. A Record plus its
    // bindings is ~18 KiB (64 root slots holding up to 64 constants each), and
    // keeping them in one array made the hash table 9 MiB wide: every probe
    // step pulled a fresh cache line and TLB entry straight out of DRAM.
    // Splitting them keeps the probed array at a few hundred bytes per entry
    // and touches the bindings only after a key matched.
    GraphicsRootBindings* bindings = nullptr;
    // Last FindGeometryPipeline result for leasePipeline, misses included. Valid
    // while GeometryPipelineLookupGeneration() equals leaseGeneration (0 = none);
    // holds the entry the way the cache's own lookup memo does.
    ID3D12PipelineState* leasePipeline = nullptr;
    std::uint64_t leaseGeneration = 0;
    ExperimentPipelineLease lease;
    CommandLifetime lifetime;
    uint64_t epoch = 0;
    GeometryRasterState raster;
    std::array<ID3D12DescriptorHeap*, 2> heaps {};
    std::array<ID3D12DescriptorHeap*, 2> heapArguments {};
    UINT heapCount = 0;
    void reset(ID3D12PipelineState* initial)
    {
        bindings->reset(initial);
        raster = {};
        heaps = {};
        heapArguments = {};
        heapCount = 0;
        leaseGeneration = 0;
        lease.reset();
        open = true;
    }
};
struct Observation
{
    Observation()
    {
        for (std::size_t index = 0; index < records.size(); ++index)
            records[index].bindings = &bindings[index];
    }
    ComPtr<ID3D12Device> device;
    UINT rtvIncrement = 0;
    std::array<void*, 85> targets {};
    bool has4 = false, has10 = false;
    std::atomic<bool> active = false;
    std::array<GraphicsRootBindings, 512> bindings;
    std::array<Record, 512> records;
    IndirectBindingCache indirect;
    // Per-thread counter rows. Every indexed draw used to bump shared cache
    // lines from every render thread; those locked read-modify-writes were a
    // measured part of the hook cost. The owner thread writes its own row with
    // a relaxed load/store pair and the report thread sums the rows; a row has
    // exactly one writer (claimed by thread id in claimRow()).
    struct Row
    {
        alignas(64) std::atomic<std::uint32_t> owner { 0 };
        // Beside the owner, so a draw without a capture writes one line.
        std::atomic<std::uint32_t> lastFrame = 0;
        std::atomic<std::uint64_t> indexed = 0, packets = 0, instances = 0, identities = 0;
        std::atomic<std::uint64_t> pipelinesReady = 0, bindingsReady = 0;
        std::atomic<std::uint64_t> captureRecorded = 0, captureRejected = 0;
    };
    static constexpr unsigned RowCount = 64;
    std::array<Row, RowCount> rows {};
    // Written by every Reset (begin) and ExecuteIndirect from every recording
    // thread, so kept on their own lines, away from the fields lookups read.
    alignas(64) std::mutex registrations;
    std::atomic<std::uint64_t> recordings = 0, capacityRejected = 0;
    std::atomic<std::uint64_t> signatures = 0, indirectKnown = 0, indirectUnknown = 0;
};
#pragma warning(pop)
std::atomic<Observation*> published = nullptr;
// Per-thread state every command hook touches, in one TLS block: the hook
// nesting depth, this thread's counter row and the find() memo. Constant-
// initialized, because a dynamic initializer puts a guard call on every access
// (GlassHookProbe.h). published is written once and active never reverts, so a
// row or memo taken while the state was active stays valid for the thread.
struct ThreadHooks
{
    unsigned depth = 0;
    Observation::Row* row = nullptr;
    Command* command = nullptr;
    Record* record = nullptr;
};
thread_local ThreadHooks threadHooks;
struct Scope
{
    bool outer = threadHooks.depth++ == 0;
    ~Scope() { --threadHooks.depth; }
};

// One row per thread, claimed by thread id on its first indexed draw after the
// state went active. A thread that finds every row taken shares row 0; that
// costs counter accuracy only, never safety, and needs more than RowCount
// threads touching one state to happen.
__declspec(noinline) Observation::Row* claimRow() noexcept
{
    auto* state = published.load(std::memory_order_acquire);
    if (!state || !state->active.load(std::memory_order_acquire))
        return nullptr;
    const auto id = static_cast<std::uint32_t>(GetCurrentThreadId());
    unsigned index = 0;
    for (unsigned i = 0; i < Observation::RowCount; ++i)
    {
        std::uint32_t expected = 0;
        if (state->rows[i].owner.compare_exchange_strong(expected, id, std::memory_order_relaxed))
        {
            index = i;
            break;
        }
    }
    return threadHooks.row = &state->rows[index];
}
// This thread's row, or null while no state is published and active.
inline Observation::Row* activeRow() noexcept
{
    auto* row = threadHooks.row;
    return row ? row : claimRow();
}

inline void bump(std::atomic<std::uint64_t>& value) noexcept
{
    value.store(value.load(std::memory_order_relaxed) + 1, std::memory_order_relaxed);
}
inline void add(std::atomic<std::uint64_t>& value, std::uint64_t count) noexcept
{
    value.store(value.load(std::memory_order_relaxed) + count, std::memory_order_relaxed);
}
std::atomic<GeometryDrawCaptureOwner*> captureOwner = nullptr;
std::mutex startup;
constexpr unsigned Probes = 32;
std::size_t first(Command* command)
{
    auto value = reinterpret_cast<std::uintptr_t>(command) >> 4;
    value ^= value >> 17;
    value *= 0x9e3779b97f4a7c15ull;
    return (value >> 32) & 511;
}
// Bounded probe, the slow half of find(). The memo is refreshed on a match.
__declspec(noinline) Record* probeRecord(Command* command) noexcept
{
    auto* state = published.load(std::memory_order_acquire);
    if (!state || !state->active.load(std::memory_order_relaxed) || !command)
        return nullptr;
    const auto start = first(command);
    for (unsigned i = 0; i < Probes; ++i)
    {
        auto& r = state->records[(start + i) & 511];
        const auto key = r.key.load(std::memory_order_acquire);
        if (!key)
            return nullptr;
        if (key == command)
        {
            if (!r.open || r.lifetime.wasDestroyed())
                return nullptr;
            threadHooks.command = command;
            threadHooks.record = &r;
            return &r;
        }
    }
    return nullptr;
}
// One published table per process. D3D12 records one list at a time per
// thread, so the same (table, command) pair repeats for thousands of calls in
// a row: 26,000 root/binding setters per engine frame against a 512-entry
// table. The memo turns those probes into one compare. It is a pure cache:
// the key and the record's open/destroyed flags are re-checked on every use,
// and a mismatch falls back to the probe. A memo exists only once the probe
// saw the state published and active, so a hit needs neither check again.
// Forced inline: the hook has the thread's TLS base in a register already.
__forceinline Record* find(Command* command) noexcept
{
    if (auto* memo = threadHooks.record; memo && threadHooks.command == command &&
        memo->key.load(std::memory_order_acquire) == command && !memo->lifetime.wasDestroyed())
        return memo->open ? memo : nullptr;
    // Another list, or this one's record was recycled, replaced or destroyed
    // since the memo was taken: the probe decides.
    return probeRecord(command);
}
bool matches(Observation& state, Command* command)
{
    if (command->GetType() != D3D12_COMMAND_LIST_TYPE_DIRECT)
        return false;
    ComPtr<ID3D12Device> device;
    if (FAILED(command->GetDevice(IID_PPV_ARGS(&device))) || device.Get() != state.device.Get())
        return false;
    auto table = *reinterpret_cast<void***>(command);
    for (unsigned i = 0; i < 60; ++i)
        if (state.targets[i] && table[i] != state.targets[i])
            return false;
    ComPtr<ID3D12GraphicsCommandList4> c4;
    ComPtr<ID3D12GraphicsCommandList10> c10;
    auto result = command->QueryInterface(IID_PPV_ARGS(&c4));
    if (SUCCEEDED(result))
    {
        if (!state.has4 || static_cast<Command*>(c4.Get()) != command ||
            (*reinterpret_cast<void***>(c4.Get()))[75] != state.targets[75] ||
            (*reinterpret_cast<void***>(c4.Get()))[68] != state.targets[68] ||
            (*reinterpret_cast<void***>(c4.Get()))[69] != state.targets[69])
            return false;
    }
    else if (result != E_NOINTERFACE || state.has4)
        return false;
    result = command->QueryInterface(IID_PPV_ARGS(&c10));
    if (SUCCEEDED(result))
    {
        if (!state.has10 || static_cast<Command*>(c10.Get()) != command ||
            (*reinterpret_cast<void***>(c10.Get()))[84] != state.targets[84])
            return false;
    }
    else if (result != E_NOINTERFACE || state.has10)
        return false;
    return true;
}
void begin(Command* command, ID3D12PipelineState* initial) noexcept
{
    auto* state = published.load(std::memory_order_acquire);
    if (!state || !state->active.load(std::memory_order_relaxed))
        return;
    try
    {
        // Cold path only. Application recording of a single list is serialized
        // by D3D12's contract. Hot setters do a bounded lookup without this lock.
        std::lock_guard lock(state->registrations);
        Record* available = nullptr;
        const auto start = first(command);
        for (unsigned i = 0; i < Probes; ++i)
        {
            auto& r = state->records[(start + i) & 511];
            const auto key = r.key.load(std::memory_order_acquire);
            if (key == command && !r.lifetime.wasDestroyed())
            {
                r.reset(initial);
                r.epoch = ++state->recordings;
                return;
            }
            if (!available && (!key || r.lifetime.wasDestroyed()))
                available = &r;
            if (!key)
                break;
        }
        if (!available)
        {
            ++state->capacityRejected;
            return;
        }
        // The same live COM object retains its method layout. Query interfaces
        // once per lifetime instead of again for every frame's Reset.
        if (!matches(*state, command))
            return;
        available->lifetime.takeDestroyed();
        if (!available->lifetime.attach(command))
            return;
        available->reset(initial);
        available->epoch = ++state->recordings;
        available->key.store(command, std::memory_order_release);
    }
    catch (...)
    {
    }
}
MethodType<&ID3D12Device::CreateCommandList> originalCreate = nullptr;
HRESULT WINAPI create(ID3D12Device* device, UINT node, D3D12_COMMAND_LIST_TYPE type, ID3D12CommandAllocator* allocator,
                      ID3D12PipelineState* initial, REFIID iid, void** output)
{
    Scope scope;
    const auto result = originalCreate(device, node, type, allocator, initial, iid, output);
    if (scope.outer && SUCCEEDED(result) && type == D3D12_COMMAND_LIST_TYPE_DIRECT && output && *output)
    {
        ComPtr<Command> command;
        if (SUCCEEDED(static_cast<IUnknown*>(*output)->QueryInterface(IID_PPV_ARGS(&command))))
            begin(command.Get(), initial);
    }
    return result;
}
MethodType<&ID3D12Device::CreateCommandSignature> originalSignature = nullptr;
HRESULT WINAPI createSignature(ID3D12Device* device, const D3D12_COMMAND_SIGNATURE_DESC* desc,
                               ID3D12RootSignature* root, REFIID iid, void** output)
{
    Scope scope;
    const auto result = originalSignature(device, desc, root, iid, output);
    if (scope.outer && SUCCEEDED(result) && desc && output && *output)
        if (auto* state = published.load(std::memory_order_acquire);
            state && state->active.load(std::memory_order_relaxed) && state->device.Get() == device)
        {
            try
            {
                ComPtr<ID3D12CommandSignature> signature;
                if (SUCCEEDED(static_cast<IUnknown*>(*output)->QueryInterface(IID_PPV_ARGS(&signature))) &&
                    state->indirect.created(signature.Get(), root, *desc))
                    ++state->signatures;
            }
            catch (...)
            {
            } // Missing metadata leaves this signature unknown.
        }
    return result;
}
MethodType<&Command::Reset> originalReset = nullptr;
HRESULT WINAPI reset(Command* command, ID3D12CommandAllocator* allocator, ID3D12PipelineState* initial)
{
    Scope scope;
    const auto result = originalReset(command, allocator, initial);
    if (scope.outer)
    {
        if (SUCCEEDED(result))
        {
            if (auto* owner = captureOwner.load(std::memory_order_acquire))
                owner->discarded(command);
            begin(command, initial);
        }
        else if (auto* r = find(command))
            r->bindings->invalidate();
    }
    return result;
}
MethodType<&Command::Close> originalClose = nullptr;
HRESULT WINAPI close(Command* command)
{
    Scope scope;
    const auto result = originalClose(command);
    if (scope.outer)
        if (auto* r = find(command))
            r->open = false;
    return result;
}
void censusDraw(const ExperimentCensusObserver& observer, Command* command, Record* record,
                GlassExperimentCensusInput& input, const GeometryDrawView& draw,
                const GeometryIndexedArguments& args, const ExperimentPipelineLease& pipeline) noexcept
{
    const auto frame = draw.frame ? draw.frame : ReadCyberpunkDrawFrame();
    if (record)
        input.draw = MakeExperimentDrawInput(command, record->epoch, draw, args, record->raster, *record->bindings, pipeline);
    else
    {
        // Keep known packet metadata even if command recording was not tracked.
        // Empty raster/bindings remain borrowed until this synchronous callback.
        const GeometryRasterState raster {};
        const GraphicsRootBindings bindings {};
        input.draw = MakeExperimentDrawInput(command, 0, draw, args, raster, bindings, {});
        ObserveExperimentCensus(observer, input, frame, nullptr);
        return;
    }
    ObserveExperimentCensus(observer, input, frame, record ? &record->raster : nullptr);
}
MethodType<&Command::DrawInstanced> originalInstanced = nullptr;
void WINAPI instanced(Command* command, UINT vertices, UINT instances, UINT startVertex, UINT startInstance)
{
    const auto source = _ReturnAddress(); Scope scope;
    const bool live = scope.outer && !HooksIdle();
    const HookCostScope cost(CommandHookCost(), live);
    if (live)
        if (const auto* observer = ActiveExperimentCensus())
        {
            auto* r = SkipStateTrackingFor(HookSkipMode()) ? nullptr : find(command);
            GlassExperimentCensusInput input {}; input.operation = GlassCensusInstanced;
            input.callsite = reinterpret_cast<uint64_t>(source);
            censusDraw(*observer, command, r, input, {}, { vertices, instances, startVertex, 0, startInstance },
                       r ? FindObservedGeometryPipeline(r->bindings->pipeline) : ExperimentPipelineLease {});
        }
    cost.Stop();
    originalInstanced(command, vertices, instances, startVertex, startInstance);
}
MethodType<&Command::ExecuteIndirect> originalExecuteIndirect = nullptr;
void WINAPI hookExecuteIndirect(Command* command, ID3D12CommandSignature* signature, UINT count,
                                ID3D12Resource* args, UINT64 offset, ID3D12Resource* counter, UINT64 counterOffset)
{
    const auto source = _ReturnAddress(); Scope scope;
    const bool live = scope.outer && !HooksIdle();
    const HookCostScope cost(CommandHookCost(), live);
    if (live)
    {
        auto* r = SkipStateTrackingFor(HookSkipMode()) ? nullptr : find(command);
        if (const auto* observer = ActiveExperimentCensus())
        {
            GlassExperimentCensusInput input {}; input.operation = GlassCensusIndirect;
            input.callsite = reinterpret_cast<uint64_t>(source);
            input.signature = reinterpret_cast<uint64_t>(signature); input.maxCommands = count;
            input.arguments = reinterpret_cast<uint64_t>(args); input.argumentOffset = offset;
            input.counter = reinterpret_cast<uint64_t>(counter); input.counterOffset = counterOffset;
            censusDraw(*observer, command, r, input, {}, {},
                       r ? FindObservedGeometryPipeline(r->bindings->pipeline) : ExperimentPipelineLease {});
        }
        if (r)
        {
            auto* state = published.load(std::memory_order_acquire);
            if (state->indirect.apply(signature, *r->bindings)) ++state->indirectKnown;
            else ++state->indirectUnknown;
        }
    }
    cost.Stop();
    originalExecuteIndirect(command, signature, count, args, offset, counter, counterOffset);
}
// `live` is an outer call while the hooks are enabled: nested calls and idle
// calls go straight to the original. The idle latch and the skip mode are
// re-read on every call. A per-thread copy would save two relaxed loads but
// keep a thread tracking (or idle) until its next list after the setting
// flips, where GlassHookProbe.h promises the next call.
#define GLASS_GRAPHICS(Name, Class, Declaration, Arguments, Update)                                                    \
    MethodType<&Class::Name> original##Name = nullptr;                                                                 \
    void WINAPI hook##Name Declaration                                                                                 \
    {                                                                                                                  \
        Scope scope;                                                                                                   \
        const bool live = scope.outer && !HooksIdle();                                                                 \
        const HookCostScope cost(CommandHookCost(), live);                                                             \
        if (live)                                                                                                      \
            if (const auto skip = HookSkipMode(); !SkipStateTrackingFor(skip))                                         \
                if (auto* r = find(command); r && !BindingsOnlyFor(skip))                                              \
                {                                                                                                      \
                    Update;                                                                                            \
                }                                                                                                      \
        cost.Stop();                                                                                                   \
        original##Name Arguments;                                                                                      \
    }                                                                                                                  \
    static_assert(std::is_same_v<decltype(&hook##Name), MethodType<&Class::Name>>);
GLASS_GRAPHICS(ClearState, Command, (Command * command, ID3D12PipelineState* initial), (command, initial),
               r->reset(initial))
GLASS_GRAPHICS(SetPipelineState, Command, (Command * command, ID3D12PipelineState* pipeline), (command, pipeline),
               r->bindings->setPipeline(pipeline))
GLASS_GRAPHICS(SetGraphicsRootSignature, Command, (Command * command, ID3D12RootSignature* root), (command, root),
               r->bindings->setRoot(root))
GLASS_GRAPHICS(SetGraphicsRootDescriptorTable, Command,
               (Command * command, UINT index, D3D12_GPU_DESCRIPTOR_HANDLE handle), (command, index, handle),
               r->bindings->table(index, handle))
GLASS_GRAPHICS(SetGraphicsRoot32BitConstant, Command, (Command * command, UINT index, UINT value, UINT offset),
               (command, index, value, offset), r->bindings->constants(index, 1, &value, offset))
GLASS_GRAPHICS(SetGraphicsRoot32BitConstants, Command,
               (Command * command, UINT index, UINT count, const void* data, UINT offset),
               (command, index, count, data, offset), r->bindings->constants(index, count, data, offset))
GLASS_GRAPHICS(SetGraphicsRootConstantBufferView, Command,
               (Command * command, UINT index, D3D12_GPU_VIRTUAL_ADDRESS address), (command, index, address),
               r->bindings->address(index, D3D12_ROOT_PARAMETER_TYPE_CBV, address))
GLASS_GRAPHICS(SetGraphicsRootShaderResourceView, Command,
               (Command * command, UINT index, D3D12_GPU_VIRTUAL_ADDRESS address), (command, index, address),
               r->bindings->address(index, D3D12_ROOT_PARAMETER_TYPE_SRV, address))
GLASS_GRAPHICS(SetGraphicsRootUnorderedAccessView, Command,
               (Command * command, UINT index, D3D12_GPU_VIRTUAL_ADDRESS address), (command, index, address),
               r->bindings->address(index, D3D12_ROOT_PARAMETER_TYPE_UAV, address))
GLASS_GRAPHICS(ExecuteBundle, Command, (Command * command, Command* bundle), (command, bundle),
               r->bindings->invalidate(); r->raster.unknown = true)
GLASS_GRAPHICS(RSSetViewports, Command, (Command * command, UINT count, const D3D12_VIEWPORT* values),
               (command, count, values), r->raster.viewports(count, values))
GLASS_GRAPHICS(RSSetScissorRects, Command, (Command * command, UINT count, const D3D12_RECT* values),
               (command, count, values), r->raster.scissors(count, values))
GLASS_GRAPHICS(OMSetRenderTargets, Command,
               (Command * command, UINT count, const D3D12_CPU_DESCRIPTOR_HANDLE* values, BOOL contiguous,
                const D3D12_CPU_DESCRIPTOR_HANDLE* depth), (command, count, values, contiguous, depth),
               r->raster.renderTargets(count, values, contiguous, depth, published.load()->rtvIncrement))
GLASS_GRAPHICS(SetPredication, Command,
               (Command * command, ID3D12Resource* buffer, UINT64 offset, D3D12_PREDICATION_OP operation),
               (command, buffer, offset, operation), r->raster.predicate = buffer != nullptr)
GLASS_GRAPHICS(BeginRenderPass, ID3D12GraphicsCommandList4,
               (ID3D12GraphicsCommandList4 * command, UINT count, const D3D12_RENDER_PASS_RENDER_TARGET_DESC* targets,
                const D3D12_RENDER_PASS_DEPTH_STENCIL_DESC* depth, D3D12_RENDER_PASS_FLAGS flags),
               (command, count, targets, depth, flags), r->raster.renderPass = true; r->raster.targetsKnown = false)
GLASS_GRAPHICS(EndRenderPass, ID3D12GraphicsCommandList4, (ID3D12GraphicsCommandList4 * command), (command),
               r->raster.renderPass = false; r->raster.targetsKnown = false)
GLASS_GRAPHICS(SetPipelineState1, ID3D12GraphicsCommandList4,
               (ID3D12GraphicsCommandList4 * command, ID3D12StateObject* object), (command, object),
               r->bindings->invalidate())
GLASS_GRAPHICS(SetProgram, ID3D12GraphicsCommandList10,
               (ID3D12GraphicsCommandList10 * command, const D3D12_SET_PROGRAM_DESC* desc), (command, desc),
               r->bindings->invalidate())
#undef GLASS_GRAPHICS
// Redundant heap binding is common. Avoid descriptor queries for the exact
// same list; heap type is immutable for its lifetime.
bool sameHeaps(const Record& r, UINT count, ID3D12DescriptorHeap* const* values) noexcept
{
    bool same = count == r.heapCount && count <= 2 && (!count || values);
    for (UINT i = 0; same && i < count; ++i)
        same = values[i] == r.heapArguments[i];
    return same;
}
void trackHeaps(Record& r, UINT count, ID3D12DescriptorHeap* const* values)
{
    std::array<ID3D12DescriptorHeap*, 2> next {};
    bool valid = count <= 2 && (!count || values);
    for (UINT i = 0; valid && i < count; ++i)
    {
        if (!values[i])
        {
            valid = false;
            break;
        }
        const auto type = values[i]->GetDesc().Type;
        const auto slot = type == D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV ? 0u : 1u;
        if ((type != D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV && type != D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER) ||
            next[slot])
            valid = false;
        else
            next[slot] = values[i];
    }
    if (!valid)
        r.bindings->invalidate();
    if (next != r.heaps)
        r.bindings->heapsChanged();
    r.heaps = next;
    r.heapCount = valid ? count : UINT_MAX;
    r.heapArguments = {};
    if (valid)
        for (UINT i = 0; i < count; ++i)
            r.heapArguments[i] = values[i];
}
MethodType<&Command::SetDescriptorHeaps> originalHeaps = nullptr;
void WINAPI heaps(Command* command, UINT count, ID3D12DescriptorHeap* const* values)
{
    Scope scope;
    const bool live = scope.outer && !HooksIdle();
    const HookCostScope cost(CommandHookCost(), live);
    if (live)
        if (const auto skip = HookSkipMode(); !SkipStateTrackingFor(skip))
            if (auto* r = find(command); r && !BindingsOnlyFor(skip) && !sameHeaps(*r, count, values))
                trackHeaps(*r, count, values);
    cost.Stop();
    originalHeaps(command, count, values);
}
// FindGeometryPipeline for the list's current pipeline. The cache answers a
// pipeline it has no entry for under its shared lock, which every render
// thread takes, and ~95% of engine draws carry such a pipeline (2026-09-23:
// ~225 of ~4,070 object draws per frame had a ready entry). Each list keeps its
// last result, misses included, and each thread keeps recent misses. Both are
// keyed by GeometryPipelineLookupGeneration(), which changes whenever a find()
// result can change, so a pipeline that becomes ready reaches the next draw.
struct PipelineMiss
{
    ID3D12PipelineState* pipeline = nullptr;
    std::uint64_t generation = 0;
};
constexpr unsigned PipelineMissSlots = 64;
thread_local PipelineMiss pipelineMisses[PipelineMissSlots];
// The list's memo missed: the thread's recent misses, then the cache.
__declspec(noinline) const ExperimentPipelineLease& resolveLease(Record& r, ID3D12PipelineState* pipeline,
                                                               std::uint64_t generation) noexcept
{
    // Same multiplicative slot hash as the cache's own hit memo.
    const auto mixed = (static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(pipeline)) >> 4) * 2654435761ull;
    auto& miss = pipelineMisses[(mixed >> 26) & (PipelineMissSlots - 1)];
    if (miss.pipeline == pipeline && miss.generation == generation)
        r.lease.reset();
    else if (!(r.lease = FindGeometryPipeline(pipeline)))
        miss = { pipeline, generation };
    r.leasePipeline = pipeline;
    r.leaseGeneration = generation;
    return r.lease;
}
const ExperimentPipelineLease& pipelineLease(Record& r) noexcept
{
    auto* const pipeline = r.bindings->pipeline;
    // Loaded before the lookup: a result that changes after this load leaves the
    // stored generation behind, so the next draw resolves again.
    const auto generation = GeometryPipelineLookupGeneration();
    if (r.leaseGeneration == generation && r.leasePipeline == pipeline)
        return r.lease;
    return resolveLease(r, pipeline, generation);
}
const ExperimentPipelineLease noPipeline {};
MethodType<&Command::DrawIndexedInstanced> originalIndexed = nullptr;
// The parts of indexed() that a draw without a census, experiment observer,
// armed gate trace or ready pipeline never runs (~95% of object draws) are out
// of line below; inlined, they gave every draw a 1.1 KiB stack frame. They take
// the draw's arguments as scalars, so a plain draw builds no
// GeometryIndexedArguments it never passes.
__declspec(noinline) void censusIndexed(Command* command, UINT indices, UINT instances, UINT startIndex,
                                        INT baseVertex, UINT startInstance, const ExperimentCensusObserver& census,
                                        Record* r, const void* source, const GeometryDrawView& draw,
                                        const ExperimentPipelineLease& pipeline) noexcept
{
    const GeometryIndexedArguments arguments { indices, instances, startIndex, baseVertex, startInstance };
    GlassExperimentCensusInput input {}; input.operation = GlassCensusIndexed;
    input.callsite = reinterpret_cast<uint64_t>(source);
    const auto observed = pipeline ? pipeline :
        (r ? FindObservedGeometryPipeline(r->bindings->pipeline) : ExperimentPipelineLease {});
    censusDraw(census, command, r, input, draw, arguments, observed);
}
__declspec(noinline) void observeDraw(Command* command, UINT indices, UINT instances, UINT startIndex,
                                      INT baseVertex, UINT startInstance, const Record& r,
                                      const GeometryDrawView& draw, const ExperimentPipelineLease& pipeline) noexcept
{
    const GeometryIndexedArguments arguments { indices, instances, startIndex, baseVertex, startInstance };
    ObserveExperimentDraw(command, r.epoch, draw, arguments, r.raster, *r.bindings, pipeline);
}
// Gate trace of an object draw whose pipeline has no ready rewrite.
__declspec(noinline) void noteNoPipeline(ID3D12PipelineState* pipeline) noexcept
{
    GateNote(GateNoPipeline);
    GateNoteUnseenPipeline(pipeline);
    // A draw can miss the rewrite cache because creation refused the pipeline
    // or because it never saw it at all. The two have different fixes, so
    // separate them before the next round of coverage work.
    bool transparentLooking = false;
    const auto classification = GateClassifyCreatedPipeline(pipeline, &transparentLooking);
    GateNoteUnseenClass(classification, transparentLooking);
}
// Capture of an object draw whose pipeline has a ready rewrite. True when the
// draw was issued here: the original call with the capture bound, then the
// application's root bindings and pipeline restored.
__declspec(noinline) bool captureDraw(Command* command, UINT indices, UINT instances, UINT startIndex,
                                      INT baseVertex, UINT startInstance, Record& r, Observation::Row& row,
                                      const GeometryDrawView& draw, const ExperimentPipelineLease& pipeline,
                                      bool gate, const HookCostScope& cost) noexcept
{
    bump(row.pipelinesReady);
    if (!r.bindings->canReplay(*pipeline->root, pipeline->original.Get()))
    {
        if (gate)
            GateNote(GateNoBindings);
        return false;
    }
    bump(row.bindingsReady);
    auto* owner = captureOwner.load(std::memory_order_acquire);
    if (!owner)
    {
        if (gate)
            GateNote(GateNoOwner);
        return false;
    }
    GeometryPreparedDraw prepared;
    const GeometryIndexedArguments arguments { indices, instances, startIndex, baseVertex, startInstance };
    if (!owner->prepare(command, draw, arguments, pipeline, *r.bindings, prepared))
        return false;
    const bool accepted = prepared.bindable() && r.raster.usable() &&
                          pipeline->root->layout == GeometryLayout::PerInstance &&
                          prepared.history.instances == instances;
    if (accepted)
    {
        prepared.bind(command, *pipeline->root, *r.bindings);
        cost.Stop();
        originalIndexed(command, indices, instances, startIndex, baseVertex, startInstance);
        r.bindings->replay(command, pipeline->root->original.Get());
        command->SetPipelineState(pipeline->original.Get());
        bump(row.captureRecorded);
        if (gate)
            GateNote(GateCaptured);
    }
    else
    {
        bump(row.captureRejected);
        if (gate)
            GateNote(GateBindRejected);
    }
    owner->finish(command, accepted);
    return accepted;
}
void WINAPI indexed(Command* command, UINT indices, UINT instances, UINT startIndex, INT baseVertex, UINT startInstance)
{
    const auto source = _ReturnAddress();
    Scope scope;
    const bool live = scope.outer && !IndexedHooksIdle();
    const HookCostScope cost(IndexedHookCost(), live);
    // HookStageTimer::Split is out of line: guarded here, an untimed draw makes
    // none of the four calls.
    const bool timing = cost.Sampled() && HookStageTiming();
    HookStageTimer stages(timing);
    if (live)
        if (auto* row = activeRow())
        {
            bump(row->indexed);
            auto* r = find(command);
            if (timing)
                stages.Split(HookStageIndexedFind);
            const auto* census = ActiveExperimentCensus();
            // Independent of the draw, so resolved first; used only for draws with
            // objects or during a census. Bound by reference: the list's memo
            // keeps the entry for this whole call, because only this thread
            // records this list and a nested hook call never updates a record.
            const auto& pipeline = r ? pipelineLease(*r) : noPipeline;
            if (timing)
                stages.Split(HookStageIndexedPipeline);
            // Owner identities (proxy header read, lifetime ticket) are only
            // consumed by the capture, a census or an experiment observer; every
            // other draw reads the packet without completing its owners. The
            // observer is registered once and never cleared, so this one load
            // also decides whether the draw is observed.
            const bool observed = experimentDrawObserver.load(std::memory_order_acquire) != nullptr;
            const bool owners = pipeline || census || observed;
            const auto draw = ReadCyberpunkGeometryDraw(source, indices, instances, startIndex, baseVertex,
                                                        startInstance, owners);
            if (timing)
                stages.Split(HookStageIndexedRead);
            if (census)
                censusIndexed(command, indices, instances, startIndex, baseVertex, startInstance, *census, r, source,
                              draw, pipeline);
            if (!draw.objects.empty())
            {
                bump(row->packets);
                add(row->instances, draw.instances);
                row->lastFrame.store(draw.frame, std::memory_order_relaxed);
                if (owners)
                {
                    std::uint64_t identities = 0;
                    for (const auto& object : draw.objects)
                        if (object.identity)
                            identities += object.count;
                    add(row->identities, identities);
                }
                const bool gate = GateArmed();
                if (gate)
                    GateNote(GateObjectDraws);
                if (r)
                {
                    if (observed)
                        observeDraw(command, indices, instances, startIndex, baseVertex, startInstance, *r, draw,
                                    pipeline);
                    if (pipeline)
                    {
                        if (captureDraw(command, indices, instances, startIndex, baseVertex, startInstance, *r, *row,
                                        draw, pipeline, gate, cost))
                            return;
                    }
                    else if (gate)
                        noteNoPipeline(r->bindings->pipeline);
                }
                else if (gate)
                    GateNote(GateNoRecord);
            }
            if (timing)
                stages.Split(HookStageIndexedTail);
        }
    cost.Stop();
    originalIndexed(command, indices, instances, startIndex, baseVertex, startInstance);
}
template <class Function> bool attach(Function& original, void* address, Function replacement)
{
    original = reinterpret_cast<Function>(address);
    return address &&
           DetourAttach(reinterpret_cast<PVOID*>(&original), reinterpret_cast<PVOID>(replacement)) == NO_ERROR;
}
} // namespace

bool RegisterGeometryDrawCapture(GeometryDrawCaptureOwner* owner) noexcept
{
    if (!owner)
        return false;
    GeometryDrawCaptureOwner* expected = nullptr;
    return captureOwner.compare_exchange_strong(expected, owner, std::memory_order_release) || expected == owner;
}
bool UnregisterGeometryDrawCapture(GeometryDrawCaptureOwner* owner) noexcept
{
    if (!owner)
        return false;
    GeometryDrawCaptureOwner* expected = owner;
    return captureOwner.compare_exchange_strong(expected, nullptr, std::memory_order_acq_rel);
}
void NotifyGeometryCaptureSubmit(ID3D12CommandQueue* queue, UINT count, ID3D12CommandList* const* commands) noexcept
{
    if (auto* owner = captureOwner.load(std::memory_order_acquire))
        owner->submitted(queue, count, commands);
}
void NotifyGeometryCaptureBeforeSubmit(ID3D12CommandQueue* queue, UINT count, ID3D12CommandList* const* commands) noexcept
{
    if (auto* owner = captureOwner.load(std::memory_order_acquire))
        owner->beforeSubmit(queue, count, commands);
}
void NotifyGeometryCaptureSignal(ID3D12CommandQueue* queue, ID3D12Fence* fence, UINT64 value) noexcept
{
    if (auto* owner = captureOwner.load(std::memory_order_acquire))
        owner->signal(queue, fence, value);
}
void NotifyGeometryCaptureWait(ID3D12CommandQueue* queue, ID3D12Fence* fence, UINT64 value) noexcept
{
    if (auto* owner = captureOwner.load(std::memory_order_acquire))
        owner->wait(queue, fence, value);
}
const GeometryRasterState* ReadGeometryRasterState(ID3D12GraphicsCommandList* command) noexcept
{
    if (auto* record = find(command))
        return &record->raster;
    return nullptr;
}
std::uint64_t ReadGeometryRecordingEpoch(ID3D12GraphicsCommandList* command) noexcept
{
    if (auto* record = find(command))
        return record->epoch;
    return 0;
}

bool StartGeometryCommands(ID3D12Device* device) noexcept
{
    try
    {
        std::lock_guard lock(startup);
        if (auto* current = published.load())
            return current->device.Get() == device && current->active.load();
        if (!device)
            return false;
        auto state = std::make_unique<Observation>();
        state->device = device;
        state->rtvIncrement = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
        ComPtr<ID3D12CommandAllocator> allocator;
        ComPtr<Command> command;
        if (FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator))) ||
            FAILED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr,
                                             IID_PPV_ARGS(&command))) ||
            FAILED(command->Close()))
            return false;
        auto table = *reinterpret_cast<void***>(command.Get());
        for (auto slot : { 9, 10, 11, 12, 13, 21, 22, 25, 27, 28, 30, 32, 34, 36, 38, 40, 42, 46, 56, 59 })
            state->targets[slot] = table[slot];
        ComPtr<ID3D12GraphicsCommandList4> c4;
        ComPtr<ID3D12GraphicsCommandList10> c10;
        auto result = command->QueryInterface(IID_PPV_ARGS(&c4));
        if (SUCCEEDED(result))
        {
            if (static_cast<Command*>(c4.Get()) != command.Get())
                return false;
            state->has4 = true;
            state->targets[68] = (*reinterpret_cast<void***>(c4.Get()))[68];
            state->targets[69] = (*reinterpret_cast<void***>(c4.Get()))[69];
            state->targets[75] = (*reinterpret_cast<void***>(c4.Get()))[75];
        }
        else if (result != E_NOINTERFACE)
            return false;
        result = command->QueryInterface(IID_PPV_ARGS(&c10));
        if (SUCCEEDED(result))
        {
            if (static_cast<Command*>(c10.Get()) != command.Get())
                return false;
            state->has10 = true;
            state->targets[84] = (*reinterpret_cast<void***>(c10.Get()))[84];
        }
        else if (result != E_NOINTERFACE)
            return false;
        HMODULE resident = nullptr;
        if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
                                reinterpret_cast<LPCWSTR>(&StartGeometryCommands), &resident))
            return false;
        DetourThreads threads;
        if (!threads.gather() || DetourTransactionBegin() != NO_ERROR)
            return false;
        bool okay = attach(originalCreate, (*reinterpret_cast<void***>(device))[12], create) &&
                    attach(originalSignature, (*reinterpret_cast<void***>(device))[41], createSignature) &&
                    attach(originalReset, table[10], reset) && attach(originalClose, table[9], close) &&
                    attach(originalIndexed, table[13], indexed) && attach(originalInstanced, table[12], instanced) &&
                    attach(originalHeaps, table[28], heaps);
#define GLASS_ATTACH(Name, Slot) okay = attach(original##Name, state->targets[Slot], hook##Name) && okay
        GLASS_ATTACH(ClearState, 11);
        GLASS_ATTACH(RSSetViewports, 21);
        GLASS_ATTACH(RSSetScissorRects, 22);
        GLASS_ATTACH(OMSetRenderTargets, 46);
        GLASS_ATTACH(SetPredication, 56);
        GLASS_ATTACH(SetPipelineState, 25);
        GLASS_ATTACH(ExecuteBundle, 27);
        GLASS_ATTACH(SetGraphicsRootSignature, 30);
        GLASS_ATTACH(SetGraphicsRootDescriptorTable, 32);
        GLASS_ATTACH(SetGraphicsRoot32BitConstant, 34);
        GLASS_ATTACH(SetGraphicsRoot32BitConstants, 36);
        GLASS_ATTACH(SetGraphicsRootConstantBufferView, 38);
        GLASS_ATTACH(SetGraphicsRootShaderResourceView, 40);
        GLASS_ATTACH(SetGraphicsRootUnorderedAccessView, 42);
        GLASS_ATTACH(ExecuteIndirect, 59);
        if (state->has4)
        {
            GLASS_ATTACH(BeginRenderPass, 68);
            GLASS_ATTACH(EndRenderPass, 69);
            GLASS_ATTACH(SetPipelineState1, 75);
        }
        if (state->has10)
        {
            GLASS_ATTACH(SetProgram, 84);
        }
#undef GLASS_ATTACH
        if (!okay || !threads.enlist())
        {
            DetourTransactionAbort();
            return false;
        }
        auto* stable = state.release();
        published.store(stable, std::memory_order_release);
        if (DetourTransactionCommit() != NO_ERROR)
            return false;
        stable->active.store(true, std::memory_order_release);
        return true;
    }
    catch (...)
    {
        return false;
    }
}
const GraphicsRootBindings* ReadGeometryBindings(Command* command) noexcept
{
    const auto* r = find(command);
    return r ? r->bindings : nullptr;
}
GeometryCommandStats GetGeometryCommandStats() noexcept
{
    GeometryCommandStats r;
    if (auto* s = published.load(std::memory_order_acquire))
    {
        r.active = s->active.load();
#define GLASS_STAT(Name) r.Name = s->Name.load()
        GLASS_STAT(recordings);
        GLASS_STAT(capacityRejected);
        GLASS_STAT(signatures);
        GLASS_STAT(indirectKnown);
        GLASS_STAT(indirectUnknown);
#undef GLASS_STAT
        // Sum the per-thread rows. Every row has a single writer, so a relaxed
        // load yields a complete value for that thread; totals are monotonic.
        for (const auto& row : s->rows)
        {
            r.indexed += row.indexed.load(std::memory_order_relaxed);
            r.packets += row.packets.load(std::memory_order_relaxed);
            r.instances += row.instances.load(std::memory_order_relaxed);
            r.identities += row.identities.load(std::memory_order_relaxed);
            r.pipelinesReady += row.pipelinesReady.load(std::memory_order_relaxed);
            r.bindingsReady += row.bindingsReady.load(std::memory_order_relaxed);
            r.captureRecorded += row.captureRecorded.load(std::memory_order_relaxed);
            r.captureRejected += row.captureRejected.load(std::memory_order_relaxed);
            const auto frame = row.lastFrame.load(std::memory_order_relaxed);
            if (frame > r.lastFrame)
                r.lastFrame = frame;
        }
    }
    return r;
}
} // namespace GlassFg
