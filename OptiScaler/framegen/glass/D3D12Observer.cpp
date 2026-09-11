#include "pch.h"
#include "D3D12Observer.h"
#include "ComputeRecording.h"
#include <hooks/Hook_Utils.h>
#include <detours/detours.h>
#include <tlhelp32.h>
#include <wrl/client.h>
#include <array>

namespace GlassFg
{
namespace
{
using Microsoft::WRL::ComPtr;
using Command = ID3D12GraphicsCommandList;
D3D12Callbacks callbacks;
thread_local unsigned internalDepth = 0;
uint32_t methods = 0;
bool attempted = false, installed = false;
std::array<void*, 85> commandTargets {};

struct Call
{
    bool active;
    explicit Call(bool selected = true) : active(!internalDepth && selected)
    {
        if (active)
            callbacks.enter(callbacks.context);
    }
    ~Call()
    {
        if (active)
            callbacks.leave(callbacks.context);
    }
};
bool stateTracked(Command* command)
{
    return !internalDepth && (callbacks.stateTracked ? callbacks.stateTracked(callbacks.context, command)
                                                     : command->GetType() == D3D12_COMMAND_LIST_TYPE_COMPUTE);
}
template <auto Method> using MethodType = typename rewrite_signature<decltype(Method)>::type;

MethodType<&Command::Reset> originalReset = nullptr;
HRESULT WINAPI reset(Command* command, ID3D12CommandAllocator* allocator, ID3D12PipelineState* pipeline)
{
    Call call;
    const auto result = originalReset(command, allocator, pipeline);
    if (call.active)
        callbacks.reset(callbacks.context, command, SUCCEEDED(result), pipeline);
    return result;
}
MethodType<&Command::Close> originalClose = nullptr;
HRESULT WINAPI close(Command* command)
{
    Call call(stateTracked(command));
    if (call.active)
        callbacks.mutation(callbacks.context, command);
    return originalClose(command);
}

#define GLASS_SETTER(Name, Class, Declaration, Arguments)                                                              \
    MethodType<&Class::Name> original##Name = nullptr;                                                                 \
    void WINAPI hook##Name Declaration                                                                                 \
    {                                                                                                                  \
        Call call(stateTracked(command));                                                                              \
        if (call.active)                                                                                               \
            callbacks.mutation(callbacks.context, command);                                                            \
        original##Name Arguments;                                                                                      \
    }                                                                                                                  \
    static_assert(std::is_same_v<decltype(&hook##Name), MethodType<&Class::Name>>);

GLASS_SETTER(ClearState, Command, (Command * command, ID3D12PipelineState* state), (command, state))
GLASS_SETTER(SetPipelineState, Command, (Command * command, ID3D12PipelineState* state), (command, state))
GLASS_SETTER(SetDescriptorHeaps, Command, (Command * command, UINT count, ID3D12DescriptorHeap* const* heaps),
             (command, count, heaps))
GLASS_SETTER(SetComputeRootSignature, Command, (Command * command, ID3D12RootSignature* root), (command, root))
GLASS_SETTER(SetComputeRootDescriptorTable, Command, (Command * command, UINT index, D3D12_GPU_DESCRIPTOR_HANDLE base),
             (command, index, base))
GLASS_SETTER(SetComputeRoot32BitConstant, Command, (Command * command, UINT index, UINT data, UINT offset),
             (command, index, data, offset))
GLASS_SETTER(SetComputeRoot32BitConstants, Command,
             (Command * command, UINT index, UINT count, const void* data, UINT offset),
             (command, index, count, data, offset))
GLASS_SETTER(SetComputeRootConstantBufferView, Command,
             (Command * command, UINT index, D3D12_GPU_VIRTUAL_ADDRESS address), (command, index, address))
GLASS_SETTER(SetComputeRootShaderResourceView, Command,
             (Command * command, UINT index, D3D12_GPU_VIRTUAL_ADDRESS address), (command, index, address))
GLASS_SETTER(SetComputeRootUnorderedAccessView, Command,
             (Command * command, UINT index, D3D12_GPU_VIRTUAL_ADDRESS address), (command, index, address))
GLASS_SETTER(SetPredication, Command,
             (Command * command, ID3D12Resource* buffer, UINT64 offset, D3D12_PREDICATION_OP operation),
             (command, buffer, offset, operation))
GLASS_SETTER(ExecuteIndirect, Command,
             (Command * command, ID3D12CommandSignature* signature, UINT count, ID3D12Resource* args, UINT64 offset,
              ID3D12Resource* counter, UINT64 counterOffset),
             (command, signature, count, args, offset, counter, counterOffset))
GLASS_SETTER(SetPipelineState1, ID3D12GraphicsCommandList4,
             (ID3D12GraphicsCommandList4 * command, ID3D12StateObject* state), (command, state))
GLASS_SETTER(SetProgram, ID3D12GraphicsCommandList10,
             (ID3D12GraphicsCommandList10 * command, const D3D12_SET_PROGRAM_DESC* desc), (command, desc))
#undef GLASS_SETTER

MethodType<&Command::ResourceBarrier> originalBarrier = nullptr;
void WINAPI barrier(Command* command, UINT count, const D3D12_RESOURCE_BARRIER* barriers)
{
    bool selected = false;
    constexpr auto read = D3D12_RESOURCE_STATE_DEPTH_READ | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE |
                          D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    if (!internalDepth && barriers && count <= 4096)
        for (UINT i = 0; i < count; ++i)
            selected |= barriers[i].Type == D3D12_RESOURCE_BARRIER_TYPE_TRANSITION &&
                        barriers[i].Flags == D3D12_RESOURCE_BARRIER_FLAG_NONE &&
                        barriers[i].Transition.StateBefore == D3D12_RESOURCE_STATE_DEPTH_WRITE &&
                        barriers[i].Transition.StateAfter == read;
    Call call(selected);
    originalBarrier(command, count, barriers);
    if (call.active)
        callbacks.barrier(callbacks.context, command, count, barriers);
}
MethodType<&ID3D12CommandQueue::ExecuteCommandLists> originalSubmit = nullptr;
void WINAPI submit(ID3D12CommandQueue* queue, UINT count, ID3D12CommandList* const* commands)
{
    Call call;
    // Preserve identities through post-submit completion bookkeeping, even if
    // another app thread drops its own reference immediately after submission.
    if (call.active)
        for (UINT i = 0; i < count; ++i)
            commands[i]->AddRef();
    if (call.active && callbacks.beforeSubmit)
        callbacks.beforeSubmit(callbacks.context, queue, count, commands);
    originalSubmit(queue, count, commands);
    if (call.active)
    {
        callbacks.submit(callbacks.context, queue, count, commands);
        for (UINT i = 0; i < count; ++i)
            commands[i]->Release();
    }
}
MethodType<&ID3D12CommandQueue::Signal> originalSignal = nullptr;
HRESULT WINAPI signal(ID3D12CommandQueue* queue, ID3D12Fence* fence, UINT64 value)
{
    Call call;
    const auto result = originalSignal(queue, fence, value);
    if (call.active && SUCCEEDED(result))
        callbacks.signal(callbacks.context, queue, fence, value);
    return result;
}
MethodType<&ID3D12CommandQueue::Wait> originalWait = nullptr;
HRESULT WINAPI wait(ID3D12CommandQueue* queue, ID3D12Fence* fence, UINT64 value)
{
    Call call;
    const auto result = originalWait(queue, fence, value);
    if (call.active && SUCCEEDED(result))
        callbacks.wait(callbacks.context, queue, fence, value);
    return result;
}

// Open handles before suspending any thread. A failed enlistment aborts the
// transaction rather than advertising partial state-observer coverage.
struct Threads
{
    std::array<HANDLE, 512> handles {};
    unsigned count = 0;
    ~Threads()
    {
        for (unsigned i = 0; i < count; ++i)
            CloseHandle(handles[i]);
    }
    bool gather()
    {
        HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
        if (snapshot == INVALID_HANDLE_VALUE)
            return false;
        THREADENTRY32 entry {};
        entry.dwSize = sizeof(entry);
        bool okay = Thread32First(snapshot, &entry) != FALSE;
        if (okay)
            do
            {
                if (entry.th32OwnerProcessID != GetCurrentProcessId() || entry.th32ThreadID == GetCurrentThreadId())
                    continue;
                if (count == handles.size())
                {
                    okay = false;
                    break;
                }
                auto handle = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_SET_CONTEXT |
                                             THREAD_QUERY_INFORMATION,
                                         FALSE, entry.th32ThreadID);
                if (handle)
                    handles[count++] = handle;
                else if (GetLastError() != ERROR_INVALID_PARAMETER)
                {
                    okay = false;
                    break;
                }
            } while (Thread32Next(snapshot, &entry));
        CloseHandle(snapshot);
        return okay;
    }
    bool enlist()
    {
        if (DetourUpdateThread(GetCurrentThread()) != NO_ERROR)
            return false;
        for (unsigned i = 0; i < count; ++i)
        {
            DWORD code = 0;
            if (!GetExitCodeThread(handles[i], &code))
                return false;
            if (code == STILL_ACTIVE && DetourUpdateThread(handles[i]) != NO_ERROR)
                return false;
        }
        return true;
    }
};
template <class Function> bool attach(Function& original, void* target, Function replacement)
{
    original = reinterpret_cast<Function>(target);
    return DetourAttach(reinterpret_cast<PVOID*>(&original), reinterpret_cast<PVOID>(replacement)) == NO_ERROR;
}
} // namespace

InternalD3D12Scope::InternalD3D12Scope() { ++internalDepth; }
InternalD3D12Scope::~InternalD3D12Scope() { --internalDepth; }
uint32_t ObservedComputeMethods() { return installed ? methods : 0; }
bool MatchesD3D12Observer(Command* command)
{
    if (!installed || !command)
        return false;
    auto table = *reinterpret_cast<void***>(command);
    for (unsigned i = 0; i < 60; ++i)
        if (commandTargets[i] && commandTargets[i] != table[i])
            return false;
    ComPtr<ID3D12GraphicsCommandList4> c4;
    const auto r4 = command->QueryInterface(IID_PPV_ARGS(&c4));
    if (SUCCEEDED(r4))
    {
        if (!(methods & ComputeRecording::StateObject) || static_cast<Command*>(c4.Get()) != command ||
            (*reinterpret_cast<void***>(c4.Get()))[75] != commandTargets[75])
            return false;
    }
    else if (r4 != E_NOINTERFACE || (methods & ComputeRecording::StateObject))
        return false;
    ComPtr<ID3D12GraphicsCommandList10> c10;
    const auto r10 = command->QueryInterface(IID_PPV_ARGS(&c10));
    if (SUCCEEDED(r10))
    {
        if (!(methods & ComputeRecording::Program) || static_cast<Command*>(c10.Get()) != command ||
            (*reinterpret_cast<void***>(c10.Get()))[84] != commandTargets[84])
            return false;
    }
    else if (r10 != E_NOINTERFACE || (methods & ComputeRecording::Program))
        return false;
    return true;
}

bool InstallD3D12Observer(Command* command, const D3D12Callbacks& supplied)
{
    if (attempted)
        return installed && MatchesD3D12Observer(command);
    attempted = true;
    if (!command || command->GetType() != D3D12_COMMAND_LIST_TYPE_COMPUTE || !supplied.enter || !supplied.leave ||
        !supplied.reset || !supplied.mutation || !supplied.barrier || !supplied.submit || !supplied.signal ||
        !supplied.wait)
        return false;
    ComPtr<ID3D12Device> device;
    if (FAILED(command->GetDevice(IID_PPV_ARGS(&device))))
        return false;
    auto table = *reinterpret_cast<void***>(command);
    constexpr unsigned slots[] = { 9, 10, 11, 25, 26, 28, 29, 31, 33, 35, 37, 39, 41, 55, 59 };
    for (auto slot : slots)
        commandTargets[slot] = table[slot];
    methods = ComputeRecording::BaseMethods;
    ComPtr<ID3D12GraphicsCommandList4> c4;
    ComPtr<ID3D12GraphicsCommandList10> c10;
    auto result = command->QueryInterface(IID_PPV_ARGS(&c4));
    if (SUCCEEDED(result))
    {
        if (static_cast<Command*>(c4.Get()) != command)
            return false;
        commandTargets[75] = (*reinterpret_cast<void***>(c4.Get()))[75];
        methods |= ComputeRecording::StateObject;
    }
    else if (result != E_NOINTERFACE)
        return false;
    result = command->QueryInterface(IID_PPV_ARGS(&c10));
    if (SUCCEEDED(result))
    {
        if (static_cast<Command*>(c10.Get()) != command)
            return false;
        commandTargets[84] = (*reinterpret_cast<void***>(c10.Get()))[84];
        methods |= ComputeRecording::Program;
    }
    else if (result != E_NOINTERFACE)
        return false;
    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<Command> direct;
    ComPtr<ID3D12CommandQueue> directQueue, computeQueue;
    if (FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator))) ||
        FAILED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr,
                                         IID_PPV_ARGS(&direct))))
        return false;
    if (FAILED(direct->Close()))
        return false;
    const auto directTable = *reinterpret_cast<void***>(direct.Get());
    if (directTable[10] != table[10] || directTable[26] != table[26])
        return false;
    D3D12_COMMAND_QUEUE_DESC desc {};
    desc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    if (FAILED(device->CreateCommandQueue(&desc, IID_PPV_ARGS(&directQueue))))
        return false;
    desc.Type = D3D12_COMMAND_LIST_TYPE_COMPUTE;
    if (FAILED(device->CreateCommandQueue(&desc, IID_PPV_ARGS(&computeQueue))))
        return false;
    auto queues = *reinterpret_cast<void***>(computeQueue.Get());
    auto directQueues = *reinterpret_cast<void***>(directQueue.Get());
    for (auto slot : { 10, 14, 15 })
        if (queues[slot] != directQueues[slot])
            return false;
    Threads threads;
    if (!threads.gather() || DetourTransactionBegin() != NO_ERROR)
        return false;
    callbacks = supplied; // Complete callbacks are visible before any hook runs.
    bool okay = attach(originalReset, table[10], reset) && attach(originalClose, table[9], close) &&
                attach(originalBarrier, table[26], barrier) && attach(originalSubmit, queues[10], submit) &&
                attach(originalSignal, queues[14], signal) && attach(originalWait, queues[15], wait);
#define GLASS_ATTACH(Name, Slot) okay = attach(original##Name, commandTargets[Slot], hook##Name) && okay
    GLASS_ATTACH(ClearState, 11);
    GLASS_ATTACH(SetPipelineState, 25);
    GLASS_ATTACH(SetDescriptorHeaps, 28);
    GLASS_ATTACH(SetComputeRootSignature, 29);
    GLASS_ATTACH(SetComputeRootDescriptorTable, 31);
    GLASS_ATTACH(SetComputeRoot32BitConstant, 33);
    GLASS_ATTACH(SetComputeRoot32BitConstants, 35);
    GLASS_ATTACH(SetComputeRootConstantBufferView, 37);
    GLASS_ATTACH(SetComputeRootShaderResourceView, 39);
    GLASS_ATTACH(SetComputeRootUnorderedAccessView, 41);
    GLASS_ATTACH(SetPredication, 55);
    GLASS_ATTACH(ExecuteIndirect, 59);
    if (c4)
    {
        GLASS_ATTACH(SetPipelineState1, 75);
    }
    if (c10)
    {
        GLASS_ATTACH(SetProgram, 84);
    }
#undef GLASS_ATTACH
    if (!okay || !threads.enlist())
    {
        DetourTransactionAbort();
        return false;
    }
    installed = DetourTransactionCommit() == NO_ERROR;
    return installed;
}
} // namespace GlassFg
