#pragma once
#include "GeometryTestDevice.h"

// Artificial GPU gate for independent testing only. No game input/device.
template<class Replace, class Verify> void checkInFlightCapture(Device& gpu, Replace replace, Verify verify)
{
    struct Gate
    {
        ComPtr<ID3D12Fence> fence;
        ~Gate() { if (fence) fence->Signal(1); }
    } gate;
    check(gpu.d->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&gate.fence)));
    ComPtr<ID3D12CommandAllocator> fresh;
    check(gpu.d->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&fresh)));
    check(gpu.c->Close());
    check(gpu.q->Wait(gate.fence.Get(), 1));
    ID3D12CommandList* lists[] { gpu.c.Get() };
    gpu.q->ExecuteCommandLists(1, lists); // Actual observer forwards this submission.
    const auto expected = ++gpu.value;
    check(gpu.q->Signal(gpu.f.Get(), expected));
    require(gpu.f->GetCompletedValue() < expected, "Test GPU gate was not pending");
    replace(); verify();
    // Reset onto a fresh allocator without touching the pending allocator/data.
    check(gpu.c->Reset(fresh.Get(), nullptr));
    verify();
    require(gpu.f->GetCompletedValue() < expected, "Reset completed a blocked GPU submission");
    check(gate.fence->Signal(1));
    check(gpu.f->SetEventOnCompletion(expected, gpu.event));
    require(WaitForSingleObject(gpu.event, 10000) == WAIT_OBJECT_0, "Gated capture completion timed out");
    check(gpu.d->GetDeviceRemovedReason());
    check(gpu.c->Close());
    gpu.a = std::move(fresh); // Old allocator is released only after GPU completion.
}
