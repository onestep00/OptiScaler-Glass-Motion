#include "pch.h"
#include "../NativeSession.h"
#include <wrl/client.h>
#include <cstdio>
#include <stdexcept>
using Microsoft::WRL::ComPtr;

static void check(bool value, const char* label)
{
    if (!value)
        throw std::runtime_error(label);
}
static void hr(HRESULT value, const char* label) { check(SUCCEEDED(value), label); }
static void drain(ID3D12Device* device, ID3D12CommandQueue* queue)
{
    ComPtr<ID3D12Fence> fence;
    hr(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)), "drain fence");
    hr(queue->Signal(fence.Get(), 1), "drain signal");
    HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    check(event != nullptr, "event");
    hr(fence->SetEventOnCompletion(1, event), "arm event");
    const auto result = WaitForSingleObject(event, 15000);
    CloseHandle(event);
    check(result == WAIT_OBJECT_0, "drain timeout");
    hr(device->GetDeviceRemovedReason(), "device removed");
}
static D3D12_RESOURCE_DESC texture(DXGI_FORMAT format)
{
    D3D12_RESOURCE_DESC desc {};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = 64;
    desc.Height = 32;
    desc.DepthOrArraySize = desc.MipLevels = desc.SampleDesc.Count = 1;
    desc.Format = format;
    return desc;
}
// No evaluation is prepared here, so the packed capture never hands out a frame.
static GlassFg::PackedMotionFrame noFrame(ID3D12GraphicsCommandList*, std::uint32_t, std::uint32_t, std::uint64_t,
                                         bool) noexcept
{
    return {};
}
static GlassFg::PackedMotionFrame noSecondFrame(std::uint32_t, std::uint32_t, std::uint64_t) noexcept { return {}; }
static void noDiscard(const void*, bool) noexcept {}

int wmain(int argc, wchar_t** argv)
{
    try
    {
        check(argc == 2, "packed object-motion shader path required");
        ComPtr<ID3D12Device> device;
        hr(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&device)), "device");
        D3D12_COMMAND_QUEUE_DESC q {};
        q.Type = D3D12_COMMAND_LIST_TYPE_COMPUTE;
        ComPtr<ID3D12CommandQueue> consumer;
        hr(device->CreateCommandQueue(&q, IID_PPV_ARGS(&consumer)), "consumer");
        const D3D12_RESOURCE_DESC descs[] = { texture(DXGI_FORMAT_R16G16B16A16_FLOAT),
                                              texture(DXGI_FORMAT_R8G8B8A8_UNORM),
                                              texture(DXGI_FORMAT_R32_TYPELESS) };
        // The list type belongs to the command list: Streamline submits the same
        // frame generation list on a compute queue for the driver-level block
        // and on a direct queue for the tagged evaluation. A session-wide type
        // failed the session on the second case, and a failed session used to be
        // unreleasable, which blocked every later substitution.
        ComPtr<ID3D12CommandAllocator> directAllocator, computeAllocator;
        ComPtr<ID3D12GraphicsCommandList> directList, computeList;
        hr(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&directAllocator)),
           "queue type direct allocator");
        hr(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_COMPUTE, IID_PPV_ARGS(&computeAllocator)),
           "queue type compute allocator");
        hr(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, directAllocator.Get(), nullptr,
                                     IID_PPV_ARGS(&directList)),
           "queue type direct list");
        hr(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_COMPUTE, computeAllocator.Get(), nullptr,
                                     IID_PPV_ARGS(&computeList)),
           "queue type compute list");
        GlassFg::NativeSession typed;
        check(typed.initializePacked(device.Get(), descs, argv[1], stdout, { noFrame, noSecondFrame, noDiscard }),
              "queue type initialize");
        check(typed.bindFgCommand(computeList.Get(), GlassFg::ComputeRecording::AllMethods,
                                  GlassFg::ComputeRecording::AllMethods),
              "queue type bind compute");
        check(typed.adoptFgCommand(directList.Get(), GlassFg::ComputeRecording::AllMethods,
                                   GlassFg::ComputeRecording::AllMethods),
              "queue type adopt direct");
        ID3D12CommandList* computeLists[] = { computeList.Get() };
        ID3D12CommandList* directLists[] = { directList.Get() };
        hr(computeList->Close(), "queue type close compute");
        typed.onStateMutation(computeList.Get());
        check(typed.afterSubmit(consumer.Get(), 1, computeLists), "queue type compute submit rejected");
        hr(directList->Close(), "queue type close direct");
        typed.onStateMutation(directList.Get());
        // A direct list cannot execute on the compute queue. That single
        // mismatched submission must not fail the session, or one queue type
        // change stops the correction for the rest of the process.
        check(typed.afterSubmit(consumer.Get(), 1, directLists), "queue type mismatch failed session");
        typed.stop();
        drain(device.Get(), consumer.Get());
        check(typed.readyToRelease(), "queue type session not releasable");
        typed.releaseAfterGpuDrain();
        std::puts("NATIVE_SESSION_OK per_list_queue_type=1 failed_releasable=1 game_attachment=0");
        return 0;
    }
    catch (const std::exception& error)
    {
        std::fprintf(stderr, "FAILED %s\n", error.what());
        return 1;
    }
}
