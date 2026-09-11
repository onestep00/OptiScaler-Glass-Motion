#pragma once
#include "../D3D12Observer.cpp"
#include "../GeometryDrawCapture.h"
#include <mutex>

// Independent test device only. Use the production public observer and the
// same InternalD3D12Scope/submit forwarding sequence as NativeHost.
namespace CaptureQueueTest
{
struct State { std::mutex mutex; uint64_t submitted = 0; };
inline State& state() { static auto* value = new State; return *value; }
inline bool install(ID3D12Device* device)
{
    Microsoft::WRL::ComPtr<ID3D12CommandAllocator> allocator;
    Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> command;
    if (FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_COMPUTE, IID_PPV_ARGS(&allocator))) ||
        FAILED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_COMPUTE, allocator.Get(), nullptr, IID_PPV_ARGS(&command))))
        return false;
    GlassFg::D3D12Callbacks callbacks;
    callbacks.context = &state();
    callbacks.enter = [](void* p) { static_cast<State*>(p)->mutex.lock(); };
    callbacks.leave = [](void* p) { static_cast<State*>(p)->mutex.unlock(); };
    callbacks.stateTracked = [](void*, ID3D12GraphicsCommandList*) { return false; };
    callbacks.reset = [](void*, ID3D12GraphicsCommandList*, bool, ID3D12PipelineState*) {};
    callbacks.mutation = [](void*, ID3D12GraphicsCommandList*) {};
    callbacks.barrier = [](void*, ID3D12GraphicsCommandList*, UINT, const D3D12_RESOURCE_BARRIER*) {};
    callbacks.signal = [](void*, ID3D12CommandQueue*, ID3D12Fence*, UINT64) {};
    callbacks.wait = [](void*, ID3D12CommandQueue*, ID3D12Fence*, UINT64) {};
    callbacks.submit = [](void* p, ID3D12CommandQueue* queue, UINT count, ID3D12CommandList* const* lists)
    {
        ++static_cast<State*>(p)->submitted;
        GlassFg::InternalD3D12Scope ownSignals;
        GlassFg::NotifyGeometryCaptureSubmit(queue, count, lists);
    };
    const bool okay = GlassFg::InstallD3D12Observer(command.Get(), callbacks);
    return SUCCEEDED(command->Close()) && okay;
}
inline uint64_t submissions() { std::lock_guard lock(state().mutex); return state().submitted; }
} // namespace CaptureQueueTest
