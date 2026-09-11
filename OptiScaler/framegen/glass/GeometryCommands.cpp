#include "pch.h"
#include "GeometryCommands.h"
#include "GeometryCreation.h"
#include "GeometryDrawCapture.h"
#include "CyberpunkDraws.h"
#include "CommandLifetime.h"
#include "IndirectBindings.h"
#include "DetourThreads.h"
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
thread_local unsigned commandDepth = 0;
struct Scope
{
    bool outer = commandDepth++ == 0;
    ~Scope() { --commandDepth; }
};
struct Record
{
    std::atomic<Command*> key = nullptr;
    CommandLifetime lifetime;
    GraphicsRootBindings bindings;
    GeometryRasterState raster;
    std::array<ID3D12DescriptorHeap*, 2> heaps {};
    std::array<ID3D12DescriptorHeap*, 2> heapArguments {};
    UINT heapCount = 0;
    bool open = false;
    void reset(ID3D12PipelineState* initial)
    {
        bindings.reset(initial);
        raster = {};
        heaps = {};
        heapArguments = {};
        heapCount = 0;
        open = true;
    }
};
struct Observation
{
    ComPtr<ID3D12Device> device;
    UINT rtvIncrement = 0;
    std::mutex registrations;
    std::array<Record, 512> records;
    IndirectBindingCache indirect;
    std::array<void*, 85> targets {};
    bool has4 = false, has10 = false;
    std::atomic<bool> active = false;
    std::atomic<std::uint64_t> recordings = 0, capacityRejected = 0, indexed = 0, packets = 0;
    std::atomic<std::uint64_t> instances = 0, identities = 0, pipelinesReady = 0, bindingsReady = 0;
    std::atomic<std::uint64_t> signatures = 0, indirectKnown = 0, indirectUnknown = 0;
    std::atomic<std::uint64_t> captureRecorded = 0, captureRejected = 0;
    std::atomic<std::uint32_t> lastFrame = 0;
};
std::atomic<Observation*> published = nullptr;
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
Record* find(Command* command) noexcept
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
            return r.open && !r.lifetime.wasDestroyed() ? &r : nullptr;
    }
    return nullptr;
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
                ++state->recordings;
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
        available->key.store(command, std::memory_order_release);
        ++state->recordings;
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
            r->bindings.invalidate();
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
#define GLASS_GRAPHICS(Name, Class, Declaration, Arguments, Update)                                                    \
    MethodType<&Class::Name> original##Name = nullptr;                                                                 \
    void WINAPI hook##Name Declaration                                                                                 \
    {                                                                                                                  \
        Scope scope;                                                                                                   \
        if (scope.outer)                                                                                               \
            if (auto* r = find(command))                                                                               \
            {                                                                                                          \
                Update;                                                                                                \
            }                                                                                                          \
        original##Name Arguments;                                                                                      \
    }                                                                                                                  \
    static_assert(std::is_same_v<decltype(&hook##Name), MethodType<&Class::Name>>);
GLASS_GRAPHICS(ClearState, Command, (Command * command, ID3D12PipelineState* initial), (command, initial),
               r->reset(initial))
GLASS_GRAPHICS(SetPipelineState, Command, (Command * command, ID3D12PipelineState* pipeline), (command, pipeline),
               r->bindings.setPipeline(pipeline))
GLASS_GRAPHICS(SetGraphicsRootSignature, Command, (Command * command, ID3D12RootSignature* root), (command, root),
               r->bindings.setRoot(root))
GLASS_GRAPHICS(SetGraphicsRootDescriptorTable, Command,
               (Command * command, UINT index, D3D12_GPU_DESCRIPTOR_HANDLE handle), (command, index, handle),
               r->bindings.table(index, handle))
GLASS_GRAPHICS(SetGraphicsRoot32BitConstant, Command, (Command * command, UINT index, UINT value, UINT offset),
               (command, index, value, offset), r->bindings.constants(index, 1, &value, offset))
GLASS_GRAPHICS(SetGraphicsRoot32BitConstants, Command,
               (Command * command, UINT index, UINT count, const void* data, UINT offset),
               (command, index, count, data, offset), r->bindings.constants(index, count, data, offset))
GLASS_GRAPHICS(SetGraphicsRootConstantBufferView, Command,
               (Command * command, UINT index, D3D12_GPU_VIRTUAL_ADDRESS address), (command, index, address),
               r->bindings.address(index, D3D12_ROOT_PARAMETER_TYPE_CBV, address))
GLASS_GRAPHICS(SetGraphicsRootShaderResourceView, Command,
               (Command * command, UINT index, D3D12_GPU_VIRTUAL_ADDRESS address), (command, index, address),
               r->bindings.address(index, D3D12_ROOT_PARAMETER_TYPE_SRV, address))
GLASS_GRAPHICS(SetGraphicsRootUnorderedAccessView, Command,
               (Command * command, UINT index, D3D12_GPU_VIRTUAL_ADDRESS address), (command, index, address),
               r->bindings.address(index, D3D12_ROOT_PARAMETER_TYPE_UAV, address))
GLASS_GRAPHICS(ExecuteBundle, Command, (Command * command, Command* bundle), (command, bundle),
               r->bindings.invalidate(); r->raster.unknown = true)
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
GLASS_GRAPHICS(ExecuteIndirect, Command,
               (Command * command, ID3D12CommandSignature* signature, UINT count, ID3D12Resource* args, UINT64 offset,
                ID3D12Resource* counter, UINT64 counterOffset),
               (command, signature, count, args, offset, counter, counterOffset),
               auto* state = published.load(std::memory_order_acquire);
               if (state->indirect.apply(signature, r->bindings))++ state->indirectKnown; else ++state->indirectUnknown)
GLASS_GRAPHICS(SetPipelineState1, ID3D12GraphicsCommandList4,
               (ID3D12GraphicsCommandList4 * command, ID3D12StateObject* object), (command, object),
               r->bindings.invalidate())
GLASS_GRAPHICS(SetProgram, ID3D12GraphicsCommandList10,
               (ID3D12GraphicsCommandList10 * command, const D3D12_SET_PROGRAM_DESC* desc), (command, desc),
               r->bindings.invalidate())
#undef GLASS_GRAPHICS
MethodType<&Command::SetDescriptorHeaps> originalHeaps = nullptr;
void WINAPI heaps(Command* command, UINT count, ID3D12DescriptorHeap* const* values)
{
    Scope scope;
    if (scope.outer)
        if (auto* r = find(command))
        {
            // Redundant heap binding is common. Avoid descriptor queries for
            // the exact same list; heap type is immutable for its lifetime.
            bool same = count == r->heapCount && count <= 2 && (!count || values);
            for (UINT i = 0; same && i < count; ++i)
                same = values[i] == r->heapArguments[i];
            if (same)
            {
                originalHeaps(command, count, values);
                return;
            }
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
                r->bindings.invalidate();
            if (next != r->heaps)
                r->bindings.heapsChanged();
            r->heaps = next;
            r->heapCount = valid ? count : UINT_MAX;
            r->heapArguments = {};
            if (valid)
                for (UINT i = 0; i < count; ++i)
                    r->heapArguments[i] = values[i];
        }
    originalHeaps(command, count, values);
}
MethodType<&Command::DrawIndexedInstanced> originalIndexed = nullptr;
void WINAPI indexed(Command* command, UINT indices, UINT instances, UINT startIndex, INT baseVertex, UINT startInstance)
{
    const auto source = _ReturnAddress();
    Scope scope;
    if (scope.outer)
        if (auto* state = published.load(std::memory_order_acquire); state && state->active.load())
        {
            ++state->indexed;
            const auto draw =
                ReadCyberpunkGeometryDraw(source, indices, instances, startIndex, baseVertex, startInstance);
            if (!draw.objects.empty())
            {
                ++state->packets;
                state->instances += draw.instances;
                state->lastFrame.store(draw.frame);
                for (const auto& object : draw.objects)
                    if (object.identity)
                        state->identities += object.count;
                if (auto* r = find(command))
                    if (auto pipeline = FindGeometryPipeline(r->bindings.pipeline))
                    {
                        ++state->pipelinesReady;
                        if (r->bindings.canReplay(*pipeline->root, pipeline->original.Get()))
                        {
                            ++state->bindingsReady;
                            if (auto* owner = captureOwner.load(std::memory_order_acquire))
                            {
                                GeometryPreparedDraw prepared;
                                const GeometryIndexedArguments args { indices, instances, startIndex, baseVertex,
                                                                      startInstance };
                                if (owner->prepare(command, draw, args, pipeline, r->bindings, prepared))
                                {
                                    const bool accepted = prepared.bindable() && r->raster.usable() &&
                                        pipeline->root->layout == GeometryLayout::PerInstance &&
                                        prepared.history.instances == instances;
                                    if (accepted)
                                    {
                                        prepared.bind(command, *pipeline->root, r->bindings);
                                        originalIndexed(command, indices, instances, startIndex, baseVertex,
                                                        startInstance);
                                        r->bindings.replay(command, pipeline->root->original.Get());
                                        command->SetPipelineState(pipeline->original.Get());
                                        ++state->captureRecorded;
                                    }
                                    else
                                        ++state->captureRejected;
                                    owner->finish(command, accepted);
                                    if (accepted)
                                        return;
                                }
                            }
                        }
                    }
            }
        }
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
void NotifyGeometryCaptureSubmit(ID3D12CommandQueue* queue, UINT count, ID3D12CommandList* const* commands) noexcept
{
    if (auto* owner = captureOwner.load(std::memory_order_acquire))
        owner->submitted(queue, count, commands);
}
const GeometryRasterState* ReadGeometryRasterState(ID3D12GraphicsCommandList* command) noexcept
{
    if (auto* record = find(command))
        return &record->raster;
    return nullptr;
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
        for (auto slot : { 9, 10, 11, 13, 21, 22, 25, 27, 28, 30, 32, 34, 36, 38, 40, 42, 46, 56, 59 })
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
                    attach(originalIndexed, table[13], indexed) && attach(originalHeaps, table[28], heaps);
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
    return r ? &r->bindings : nullptr;
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
        GLASS_STAT(indexed);
        GLASS_STAT(packets);
        GLASS_STAT(instances);
        GLASS_STAT(identities);
        GLASS_STAT(pipelinesReady);
        GLASS_STAT(bindingsReady);
        GLASS_STAT(captureRecorded);
        GLASS_STAT(captureRejected);
        GLASS_STAT(signatures);
        GLASS_STAT(indirectKnown);
        GLASS_STAT(indirectUnknown);
        GLASS_STAT(lastFrame);
#undef GLASS_STAT
    }
    return r;
}
} // namespace GlassFg
