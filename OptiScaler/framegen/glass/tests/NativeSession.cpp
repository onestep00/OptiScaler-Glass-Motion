#include "pch.h"
#include "../NativeSession.h"
#include <dxgi1_4.h>
#include <wrl/client.h>
#include <cstdio>
#include <cwchar>
#include <stdexcept>
using Microsoft::WRL::ComPtr;

// The two host services the second consumer reads, defined by the test: the
// live controls (NrMotion is the switch under test) and the engine frame the
// upscaler evaluates.
static bool nrMotion = false;
GlassFg::Controls GlassFg::ReadControls()
{
    Controls value;
    value.nrMotion = nrMotion;
    return value;
}
GlassFg::GeometryCommandStats GlassFg::GetGeometryCommandStats() noexcept { return {}; }

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
static ComPtr<ID3D12Resource> committed(ID3D12Device* device, const D3D12_RESOURCE_DESC& desc,
                                        D3D12_RESOURCE_STATES state)
{
    D3D12_HEAP_PROPERTIES heap {};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    ComPtr<ID3D12Resource> resource;
    hr(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, state, nullptr, IID_PPV_ARGS(&resource)),
       "resource");
    return resource;
}
// No evaluation is prepared here, so the packed capture never hands out a frame.
static GlassFg::PackedMotionFrame noFrame(ID3D12GraphicsCommandList*, std::uint32_t, std::uint32_t, std::uint64_t,
                                         bool) noexcept
{
    return {};
}
static GlassFg::PackedMotionFrame noSecondFrame(std::uint32_t, std::uint32_t, std::uint64_t) noexcept { return {}; }
static void noDiscard(const void*, bool) noexcept {}
// The packed records of the frame the upscaler evaluates, as the capture hands
// them to the second consumer.
static ID3D12Resource* secondPacked = nullptr;
static GlassFg::PackedMotionFrame secondFrame(std::uint32_t width, std::uint32_t height, std::uint64_t) noexcept
{
    return { secondPacked, width, height, 1 };
}

int wmain(int argc, wchar_t** argv)
{
    try
    {
        const bool warp = argc == 3 && std::wcscmp(argv[2], L"--warp") == 0;
        check(argc == 2 || warp, "usage: NativeSession.exe <GlassObjectMotion.hlsl> [--warp]");
        // WARP runs the same queues and fences on the CPU, independent of the
        // GPU a running game uses.
        ComPtr<IDXGIAdapter> adapter;
        if (warp)
        {
            ComPtr<IDXGIFactory4> factory;
            hr(CreateDXGIFactory1(IID_PPV_ARGS(&factory)), "dxgi factory");
            hr(factory->EnumWarpAdapter(IID_PPV_ARGS(&adapter)), "warp adapter");
        }
        ComPtr<ID3D12Device> device;
        hr(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&device)), "device");
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

        // Second consumer (DLSS-NR): the compose and the pair it hands out stay
        // on the upscaler's own list. A session must not be released while that
        // list can still be submitted or one of its executions is still running.
        D3D12_COMMAND_QUEUE_DESC g {};
        g.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        ComPtr<ID3D12CommandQueue> graphics;
        hr(device->CreateCommandQueue(&g, IID_PPV_ARGS(&graphics)), "graphics");
        const auto guideState = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
        const auto motion = committed(device.Get(), descs[0], guideState);
        const auto depth = committed(device.Get(), descs[2], guideState);
        D3D12_RESOURCE_DESC records {};
        records.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        records.Width = descs[0].Width * descs[0].Height * 8;
        records.Height = records.DepthOrArraySize = records.MipLevels = records.SampleDesc.Count = 1;
        records.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        records.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        const auto packed = committed(device.Get(), records, D3D12_RESOURCE_STATE_COMMON);
        secondPacked = packed.Get();
        const auto open = [&](GlassFg::NativeSession& session, ComPtr<ID3D12CommandAllocator>& allocator,
                              ComPtr<ID3D12GraphicsCommandList>& list)
        {
            check(session.initializePacked(device.Get(), descs, argv[1], stdout, { noFrame, secondFrame, noDiscard }),
                  "second consumer initialize");
            hr(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)),
               "upscaler allocator");
            hr(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr,
                                         IID_PPV_ARGS(&list)),
               "upscaler list");
        };
        const auto serve = [&](GlassFg::NativeSession& session, ID3D12GraphicsCommandList* list)
        {
            ID3D12Resource* composedMotion = nullptr;
            ID3D12Resource* composedDepth = nullptr;
            return session.secondConsumerGuides(list, motion.Get(), depth.Get(), guideState, guideState, 0.f, 0.f,
                                                1.f, 1.f, &composedMotion, &composedDepth) &&
                   composedMotion != nullptr && composedDepth != nullptr;
        };
        {
            // Off by default: the upscaler keeps the game's guides and its list
            // never holds the session.
            GlassFg::NativeSession off;
            ComPtr<ID3D12CommandAllocator> allocator;
            ComPtr<ID3D12GraphicsCommandList> list;
            open(off, allocator, list);
            check(!serve(off, list.Get()), "NrMotion off served the second consumer");
            off.stop();
            check(off.readyToRelease(), "NrMotion off held the session");
            off.releaseAfterGpuDrain();
        }
        nrMotion = true;
        {
            GlassFg::NativeSession session;
            ComPtr<ID3D12CommandAllocator> allocator, nextAllocator;
            ComPtr<ID3D12GraphicsCommandList> list;
            open(session, allocator, list);
            hr(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&nextAllocator)),
               "next allocator");
            check(serve(session, list.Get()), "second consumer not served");
            session.stop();
            check(!session.readyToRelease(), "released with an unsubmitted inline recording");
            hr(list->Close(), "upscaler close");
            // The queue waits on a fence the CPU owns, so the execution provably
            // cannot finish before the gate opens. Checks taken inside that
            // window are asserted only after it reopens, so a failure cannot
            // leave the queue blocked.
            ComPtr<ID3D12Fence> gate;
            hr(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&gate)), "gate");
            hr(graphics->Wait(gate.Get(), 1), "gate wait");
            ID3D12CommandList* lists[] = { list.Get() };
            graphics->ExecuteCommandLists(1, lists);
            session.afterSubmit(graphics.Get(), 1, lists);
            const bool heldClosed = !session.readyToRelease();
            const bool reset = SUCCEEDED(list->Reset(nextAllocator.Get(), nullptr));
            if (reset)
                session.onReset(list.Get(), true, nullptr);
            const bool heldExecuting = !session.readyToRelease();
            // CPU-side signal: a queue-side Signal would sit behind the wait.
            hr(gate->Signal(1), "gate signal");
            drain(device.Get(), graphics.Get());
            check(heldClosed, "released while the closed list could execute again");
            check(reset, "upscaler reset");
            check(heldExecuting, "released while the inline recording was executing");
            check(session.readyToRelease(), "finished inline recording still held the session");
            session.releaseAfterGpuDrain();
        }
        {
            GlassFg::NativeSession session;
            ComPtr<ID3D12CommandAllocator> allocator;
            ComPtr<ID3D12GraphicsCommandList> list;
            open(session, allocator, list);
            check(serve(session, list.Get()), "second consumer not served before destruction");
            session.stop();
            // The game destroys the list instead of submitting it.
            list.Reset();
            check(session.readyToRelease(), "destroyed list still held the session");
            session.releaseAfterGpuDrain();
        }
        std::printf("NATIVE_SESSION_OK per_list_queue_type=1 failed_releasable=1 nr_off=1 inline_held=1 "
                    "inline_destroyed=1 warp=%u game_attachment=0\n",
                    warp ? 1u : 0u);
        return 0;
    }
    catch (const std::exception& error)
    {
        std::fprintf(stderr, "FAILED %s\n", error.what());
        return 1;
    }
}
