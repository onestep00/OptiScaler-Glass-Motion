#include "pch.h"
#include "../NativeSession.h"
#include <wrl/client.h>
#include <cstdio>
#include <stdexcept>
#include <vector>
using Microsoft::WRL::ComPtr;
#ifdef GLASS_TEST_OBSERVER
#include "ObservedSession.h"
using TestedSession = ObserverTest::Session;
#else
using TestedSession = GlassFg::NativeSession;
namespace ObserverTest { void omitWait(bool) {} }
#endif

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
static void barrier(ID3D12GraphicsCommandList* command, ID3D12Resource* resource, D3D12_RESOURCE_STATES before,
                    D3D12_RESOURCE_STATES after)
{
    D3D12_RESOURCE_BARRIER b {};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition = { resource, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, before, after };
    command->ResourceBarrier(1, &b);
}
static ComPtr<ID3D12Resource> texture(ID3D12Device* device, DXGI_FORMAT format, bool depth = false)
{
    D3D12_HEAP_PROPERTIES heap {};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC desc {};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = 64;
    desc.Height = 32;
    desc.DepthOrArraySize = desc.MipLevels = desc.SampleDesc.Count = 1;
    desc.Format = format;
    if (depth)
        desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
    ComPtr<ID3D12Resource> resource;
    hr(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                       IID_PPV_ARGS(&resource)),
       "texture");
    return resource;
}
static ComPtr<ID3D12Resource> uploadZeros(ID3D12Device* device, ID3D12GraphicsCommandList* command,
                                          ID3D12Resource* resource)
{
    auto desc = resource->GetDesc();
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp {};
    UINT64 bytes;
    device->GetCopyableFootprints(&desc, 0, 1, 0, &fp, nullptr, nullptr, &bytes);
    D3D12_HEAP_PROPERTIES heap {};
    heap.Type = D3D12_HEAP_TYPE_UPLOAD;
    D3D12_RESOURCE_DESC buffer {};
    buffer.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    buffer.Width = bytes;
    buffer.Height = buffer.DepthOrArraySize = buffer.MipLevels = buffer.SampleDesc.Count = 1;
    buffer.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ComPtr<ID3D12Resource> upload;
    hr(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &buffer, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                       IID_PPV_ARGS(&upload)),
       "upload");
    void* data = nullptr;
    hr(upload->Map(0, nullptr, &data), "map");
    std::memset(data, 0, static_cast<size_t>(bytes));
    upload->Unmap(0, nullptr);
    D3D12_TEXTURE_COPY_LOCATION src {}, dst {};
    src.pResource = upload.Get();
    src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    src.PlacedFootprint = fp;
    dst.pResource = resource;
    dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    command->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    return upload;
}

int wmain(int argc, wchar_t** argv)
{
    ComPtr<ID3D12Fence> gate;
    try
    {
        check(argc == 3, "seed and region shader paths required");
        ComPtr<ID3D12Device> device;
        hr(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&device)), "device");
        ComPtr<ID3D12CommandQueue> producer, consumer;
        ComPtr<ID3D12CommandAllocator> pa, ca, alternate;
        ComPtr<ID3D12GraphicsCommandList> pc, cc;
        D3D12_COMMAND_QUEUE_DESC q {};
        q.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        hr(device->CreateCommandQueue(&q, IID_PPV_ARGS(&producer)), "producer");
        hr(device->CreateCommandAllocator(q.Type, IID_PPV_ARGS(&pa)), "producer allocator");
        hr(device->CreateCommandList(0, q.Type, pa.Get(), nullptr, IID_PPV_ARGS(&pc)), "producer list");
        q.Type = D3D12_COMMAND_LIST_TYPE_COMPUTE;
        hr(device->CreateCommandQueue(&q, IID_PPV_ARGS(&consumer)), "consumer");
        hr(device->CreateCommandAllocator(q.Type, IID_PPV_ARGS(&ca)), "consumer allocator");
        hr(device->CreateCommandAllocator(q.Type, IID_PPV_ARGS(&alternate)), "alternate allocator");
        hr(device->CreateCommandList(0, q.Type, ca.Get(), nullptr, IID_PPV_ARGS(&cc)), "consumer list");
        auto motion = texture(device.Get(), DXGI_FORMAT_R16G16B16A16_FLOAT);
        auto color = texture(device.Get(), DXGI_FORMAT_R8G8B8A8_UNORM);
        auto depth = texture(device.Get(), DXGI_FORMAT_R32_TYPELESS);
        auto surface = texture(device.Get(), DXGI_FORMAT_R32_TYPELESS, true);
        std::vector<ComPtr<ID3D12Resource>> uploads;
        for (auto resource : { motion.Get(), color.Get(), depth.Get(), surface.Get() })
            uploads.push_back(uploadZeros(device.Get(), pc.Get(), resource));
        constexpr auto readState = D3D12_RESOURCE_STATE_DEPTH_READ | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE |
                                   D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
        barrier(pc.Get(), surface.Get(), D3D12_RESOURCE_STATE_COPY_DEST, readState);
        hr(pc->Close(), "upload close");
        ID3D12CommandList* pLists[] = { pc.Get() };
        ID3D12CommandList* cLists[] = { cc.Get() };
        producer->ExecuteCommandLists(1, pLists);
        drain(device.Get(), producer.Get());
        uploads.clear();
        TestedSession session;
        D3D12_RESOURCE_DESC descs[] = { motion->GetDesc(), color->GetDesc(), depth->GetDesc() };
        check(session.initialize(device.Get(), descs, argv[1], argv[2], stdout), "initialize session");
        check(session.bindFgCommand(cc.Get(), GlassFg::ComputeRecording::AllMethods,
                                    GlassFg::ComputeRecording::AllMethods),
              "bind compute");
        hr(cc->Close(), "bootstrap close");
        session.onStateMutation(cc.Get());
        consumer->ExecuteCommandLists(1, cLists);
        check(session.afterSubmit(consumer.Get(), 1, cLists), "bootstrap submit");
        drain(device.Get(), consumer.Get());
        ComPtr<ID3D12Fence> native;
        hr(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&native)), "native fence");
        GlassFg::Inputs inputs {};
        inputs.motion = motion.Get();
        inputs.color = color.Get();
        inputs.depth = depth.Get();
        inputs.index = inputs.count = 1;
        inputs.scaleX = 64;
        inputs.scaleY = 32;
        for (unsigned i = 0; i < 4; ++i)
            inputs.clipToPrevious[i * 5] = 1;
        const D3D12_RESOURCE_STATES states[] = { D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COPY_DEST,
                                                 D3D12_RESOURCE_STATE_COPY_DEST };
        for (unsigned scenario = 1; scenario <= 4; ++scenario)
        {
            ObserverTest::omitWait(scenario == 1);
            hr(pa->Reset(), "reset producer allocator");
            hr(pc->Reset(pa.Get(), nullptr), "reset producer list");
            session.onReset(pc.Get(), true, nullptr);
            hr(ca->Reset(), "reset consumer allocator");
            hr(cc->Reset(ca.Get(), nullptr), "reset consumer list");
            session.onReset(cc.Get(), true, nullptr);
            check(session.captureIdentifiedSurface(pc.Get(), surface.Get(), readState), "capture");
            if (scenario == 2)
                check(session.captureIdentifiedSurface(pc.Get(), surface.Get(), readState), "ambiguous capture");
            hr(pc->Close(), "close producer");
            session.onStateMutation(pc.Get());
            producer->ExecuteCommandLists(1, pLists);
            check(session.afterSubmit(producer.Get(), 1, pLists), "submit producer");
            hr(producer->Signal(native.Get(), scenario), "native producer signal");
            session.onSignal(producer.Get(), native.Get(), scenario);
            hr(consumer->Wait(native.Get(), scenario), "native consumer wait");
            if (scenario != 1)
                session.onWait(consumer.Get(), native.Get(), scenario);
            if (scenario == 3)
            {
                cc->SetPredication(nullptr, 0, D3D12_PREDICATION_OP_EQUAL_ZERO);
                session.onStateMutation(cc.Get());
            }
            auto prepared = session.prepare(cc.Get(), inputs, states, { true, 100, true });
            check((prepared.motion != nullptr) == (scenario == 4), "admission mismatch");
            if (scenario < 4)
            {
                hr(cc->Close(), "close rejected recording");
                session.onStateMutation(cc.Get());
                consumer->ExecuteCommandLists(1, cLists);
                check(session.afterSubmit(consumer.Get(), 1, cLists), "submit rejected recording");
                drain(device.Get(), consumer.Get());
                continue;
            }
            // Hold the correction in flight. Reset with another allocator is
            // legal, but must not make the GPU resources releasable yet.
            session.stop();
            check(!session.readyToRelease(), "unsubmitted output released");
            hr(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&gate)), "gate");
            hr(consumer->Wait(gate.Get(), 1), "hold consumer");
            hr(cc->Close(), "close accepted recording");
            session.onStateMutation(cc.Get());
            consumer->ExecuteCommandLists(1, cLists);
            check(session.afterSubmit(consumer.Get(), 1, cLists), "submit accepted recording");
            drain(device.Get(), producer.Get());
            hr(pa->Reset(), "release producer allocator");
            hr(pc->Reset(pa.Get(), nullptr), "release producer recording");
            session.onReset(pc.Get(), true, nullptr);
            hr(cc->Reset(alternate.Get(), nullptr), "reset inflight recording");
            session.onReset(cc.Get(), true, nullptr);
            check(!session.readyToRelease(), "inflight output released");
            check(!session.pollTiming(), "inflight timing read");
            hr(gate->Signal(1), "release gate");
            drain(device.Get(), consumer.Get());
            check(session.readyToRelease(), "completed output retained");
            check(session.pollTiming().has_value(), "completed timing missing");
            check(!session.pollTiming(), "duplicate timing");
            check(!session.captureIdentifiedSurface(pc.Get(), surface.Get(), readState), "capture after stop");
            check(!session.prepare(cc.Get(), inputs, states, { true, 100, true }).motion, "prepare after stop");
            hr(cc->Close(), "close final consumer");
            hr(pc->Close(), "close final producer");
            session.releaseAfterGpuDrain();
        }
        // The real command objects can die without Reset. Destruction drops
        // recording references, but must not drop their submitted GPU work.
        for (bool submitted : { false, true })
        {
            ComPtr<ID3D12CommandAllocator> destroyPa, destroyCa;
            ComPtr<ID3D12GraphicsCommandList> destroyPc, destroyCc;
            hr(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&destroyPa)), "destroy pa");
            hr(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_COMPUTE, IID_PPV_ARGS(&destroyCa)), "destroy ca");
            hr(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, destroyPa.Get(), nullptr,
                                        IID_PPV_ARGS(&destroyPc)), "destroy pc");
            hr(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_COMPUTE, destroyCa.Get(), nullptr,
                                        IID_PPV_ARGS(&destroyCc)), "destroy cc");
            TestedSession retiring;
            check(retiring.initialize(device.Get(), descs, argv[1], argv[2], stdout), "destroy initialize");
            check(retiring.bindFgCommand(destroyCc.Get(), GlassFg::ComputeRecording::AllMethods,
                                        GlassFg::ComputeRecording::AllMethods), "destroy bind");
            ID3D12CommandList* producerLists[] = { destroyPc.Get() };
            ID3D12CommandList* consumerLists[] = { destroyCc.Get() };
            hr(destroyCc->Close(), "destroy bootstrap close");
            retiring.onStateMutation(destroyCc.Get());
            consumer->ExecuteCommandLists(1, consumerLists);
            check(retiring.afterSubmit(consumer.Get(), 1, consumerLists), "destroy bootstrap submit");
            drain(device.Get(), consumer.Get());
            hr(destroyCa->Reset(), "destroy reset allocator");
            hr(destroyCc->Reset(destroyCa.Get(), nullptr), "destroy reset compute");
            retiring.onReset(destroyCc.Get(), true, nullptr);
            check(retiring.captureIdentifiedSurface(destroyPc.Get(), surface.Get(), readState), "destroy capture");
            hr(destroyPc->Close(), "destroy close producer");
            if (submitted)
            {
                producer->ExecuteCommandLists(1, producerLists);
                check(retiring.afterSubmit(producer.Get(), 1, producerLists), "destroy submit producer");
                hr(producer->Signal(native.Get(), 100), "destroy native signal");
                retiring.onSignal(producer.Get(), native.Get(), 100);
                hr(consumer->Wait(native.Get(), 100), "destroy native wait");
                retiring.onWait(consumer.Get(), native.Get(), 100);
                check(retiring.prepare(destroyCc.Get(), inputs, states, { true, 100, true }).motion != nullptr,
                      "destroy prepare");
                hr(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&gate)), "destroy gate");
                hr(consumer->Wait(gate.Get(), 1), "destroy hold gpu");
            }
            hr(destroyCc->Close(), "destroy close consumer");
            retiring.onStateMutation(destroyCc.Get());
            if (submitted)
            {
                consumer->ExecuteCommandLists(1, consumerLists);
                check(retiring.afterSubmit(consumer.Get(), 1, consumerLists), "destroy submit consumer");
            }
            retiring.stop();
            check(!retiring.readyToRelease(), "closed recording released before destruction");
            destroyPc.Reset();
            destroyCc.Reset();
            if (submitted)
            {
                check(!retiring.readyToRelease(), "destroyed inflight recording released GPU resources");
                hr(gate->Signal(1), "destroy release gpu");
                drain(device.Get(), consumer.Get());
            }
            check(retiring.readyToRelease(), "destroyed completed recording retained");
            retiring.releaseAfterGpuDrain();
        }
        std::puts("NATIVE_SESSION_OK destruction_without_reset=1 destroyed_inflight_retained=1 "
                  "missing_wait_rejected=1 ambiguous_rejected=1 dirty_state_rejected=1 "
                  "unsubmitted_release_rejected=1 inflight_release_rejected=1 completed_release=1 timing=1 "
                  "stop_rejected=1 game_attachment=0");
#ifdef GLASS_TEST_OBSERVER
        const auto& observed = ObserverTest::context();
        check(observed.resets && observed.mutations && observed.barriers && observed.submits &&
              observed.signals && observed.waits, "missing actual observer callback");
        std::printf("D3D12_OBSERVER_OK mask=%x reset=%u mutation=%u barrier=%u submit=%u signal=%u wait=%u\n",
                    GlassFg::ObservedComputeMethods(), observed.resets, observed.mutations, observed.barriers,
                    observed.submits, observed.signals, observed.waits);
#endif
        return 0;
    }
    catch (const std::exception& error)
    {
        if (gate)
            gate->Signal(1);
        std::fprintf(stderr, "FAILED %s\n", error.what());
        return 1;
    }
}
