#include "pch.h"
#include <windows.h>
#include <d3d12.h>
#include <d3d12sdklayers.h>
#include <dxgi1_6.h>
#include <wrl/client.h>
#include <cstdio>
#include <cstdlib>
#include <stdexcept>
#include <cstring>
#include "../SurfaceSnapshotPool.h"
#include "../GlassGpuTimer.h"
using Microsoft::WRL::ComPtr;
void check(bool ok, const char* message)
{
    if (!ok)
        throw std::runtime_error(message);
}
void hr(HRESULT value, const char* message) { check(SUCCEEDED(value), message); }
void barrier(ID3D12GraphicsCommandList* c, ID3D12Resource* r, D3D12_RESOURCE_STATES a, D3D12_RESOURCE_STATES b)
{
    D3D12_RESOURCE_BARRIER x {};
    x.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    x.Transition = { r, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, a, b };
    c->ResourceBarrier(1, &x);
}
void drain(ID3D12Device* d, ID3D12CommandQueue* q)
{
    ComPtr<ID3D12Fence> f;
    hr(d->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&f)), "fence");
    hr(q->Signal(f.Get(), 1), "signal");
    HANDLE e = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    check(e != nullptr, "event");
    hr(f->SetEventOnCompletion(1, e), "event arm");
    auto s = WaitForSingleObject(e, 15000);
    CloseHandle(e);
    check(s == WAIT_OBJECT_0, "queue drain timeout");
    hr(d->GetDeviceRemovedReason(), "removed device");
}
int main()
{
    ComPtr<ID3D12Device> device;
    ComPtr<ID3D12CommandQueue> producer, consumer;
    ComPtr<ID3D12Fence> gate;
    GlassFg::SurfaceSnapshotPool pool;
    GlassFg::GpuTimer timer;
    try
    {
        ComPtr<ID3D12Debug> debug;
        bool debugEnabled = SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug)));
        if (debugEnabled)
            debug->EnableDebugLayer();
        hr(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&device)), "device");
        ComPtr<ID3D12InfoQueue> info;
        device.As(&info);
        if (info)
            info->ClearStoredMessages();
        D3D12_COMMAND_QUEUE_DESC q {};
        q.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        hr(device->CreateCommandQueue(&q, IID_PPV_ARGS(&producer)), "producer");
        q.Type = D3D12_COMMAND_LIST_TYPE_COMPUTE;
        hr(device->CreateCommandQueue(&q, IID_PPV_ARGS(&consumer)), "consumer");
        ComPtr<ID3D12CommandAllocator> a, b, b2;
        hr(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&a)), "allocator a");
        hr(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_COMPUTE, IID_PPV_ARGS(&b)), "allocator b");
        hr(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_COMPUTE, IID_PPV_ARGS(&b2)), "allocator b2");
        ComPtr<ID3D12GraphicsCommandList> pc, cc;
        hr(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, a.Get(), nullptr, IID_PPV_ARGS(&pc)), "pc");
        hr(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_COMPUTE, b.Get(), nullptr, IID_PPV_ARGS(&cc)), "cc");
        constexpr unsigned W = 64, H = 32;
        D3D12_HEAP_PROPERTIES heap {};
        heap.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC desc {};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        desc.Width = W;
        desc.Height = H;
        desc.DepthOrArraySize = desc.MipLevels = desc.SampleDesc.Count = 1;
        desc.Format = DXGI_FORMAT_R32_TYPELESS;
        desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
        ComPtr<ID3D12Resource> source;
        hr(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_DEPTH_WRITE,
                                           nullptr, IID_PPV_ARGS(&source)),
           "source");
        D3D12_DESCRIPTOR_HEAP_DESC hd {};
        hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
        hd.NumDescriptors = 1;
        ComPtr<ID3D12DescriptorHeap> dsv;
        hr(device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&dsv)), "dsv heap");
        D3D12_DEPTH_STENCIL_VIEW_DESC vd {};
        vd.Format = DXGI_FORMAT_D32_FLOAT;
        vd.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
        device->CreateDepthStencilView(source.Get(), &vd, dsv->GetCPUDescriptorHandleForHeapStart());
        pc->ClearDepthStencilView(dsv->GetCPUDescriptorHandleForHeapStart(), D3D12_CLEAR_FLAG_DEPTH, .25f, 0, 0,
                                  nullptr);
        const auto readState = D3D12_RESOURCE_STATE_DEPTH_READ | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE |
                               D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
        barrier(pc.Get(), source.Get(), D3D12_RESOURCE_STATE_DEPTH_WRITE, readState);
        check(pool.initialize(device.Get(), W, H), "pool initialize");
        GlassFg::SurfaceSnapshotPool::Token tokens[4];
        for (unsigned i = 0; i < 4; ++i)
        {
            tokens[i] = pool.capture(pc.Get(), source.Get(), readState, i + 1);
            check(bool(tokens[i]), "capture");
            check(!pool.beginRead(tokens[i], cc.Get(), i + 1), "unsubmitted read accepted");
        }
        check(!pool.capture(pc.Get(), source.Get(), readState, 5), "unretired slot reused");
        hr(pc->Close(), "close pc");
        ID3D12CommandList* pLists[] = { pc.Get() };
        producer->ExecuteCommandLists(1, pLists);
        check(pool.afterSubmit(producer.Get(), 1, pLists), "write tracking");
        ComPtr<ID3D12Fence> native;
        hr(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&native)), "native fence");
        hr(producer->Signal(native.Get(), 1), "native signal");
        hr(consumer->Wait(native.Get(), 1), "native wait");
        drain(device.Get(), producer.Get());
        hr(a->Reset(), "reset a");
        hr(pc->Reset(a.Get(), nullptr), "reset pc");
        pool.onReset(pc.Get());
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint {};
        UINT rows;
        UINT64 bytes, total;
        device->GetCopyableFootprints(&desc, 0, 1, 0, &footprint, &rows, &bytes, &total);
        heap.Type = D3D12_HEAP_TYPE_READBACK;
        D3D12_RESOURCE_DESC bd {};
        bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        bd.Width = total * 4;
        bd.Height = bd.DepthOrArraySize = bd.MipLevels = bd.SampleDesc.Count = 1;
        bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        ComPtr<ID3D12Resource> output;
        hr(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                           IID_PPV_ARGS(&output)),
           "readback");
        check(timer.initialize(device.Get()), "timer initialize");
        auto ticket = timer.begin(cc.Get());
        check(bool(ticket), "timer begin");
        for (unsigned i = 0; i < 4; ++i)
        {
            check(!pool.beginRead(tokens[i], cc.Get(), 999), "wrong generation read accepted");
            auto snapshot = pool.beginRead(tokens[i], cc.Get(), i + 1);
            check(snapshot != nullptr, "begin read");
            check(!pool.beginRead(tokens[i], cc.Get(), i + 1), "duplicate read accepted");
            barrier(cc.Get(), snapshot, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_SOURCE);
            D3D12_TEXTURE_COPY_LOCATION src {};
            src.pResource = snapshot;
            src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            D3D12_TEXTURE_COPY_LOCATION dst {};
            dst.pResource = output.Get();
            dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
            dst.PlacedFootprint = footprint;
            dst.PlacedFootprint.Offset = total * i;
            cc->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
            barrier(cc.Get(), snapshot, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COMMON);
            pool.retire(tokens[i]);
        }
        timer.end(cc.Get(), ticket);
        hr(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&gate)), "gate");
        hr(consumer->Wait(gate.Get(), 1), "gate wait");
        hr(cc->Close(), "close cc");
        ID3D12CommandList* cLists[] = { cc.Get() };
        consumer->ExecuteCommandLists(1, cLists);
        check(pool.afterSubmit(consumer.Get(), 1, cLists), "read tracking");
        ComPtr<ID3D12Fence> timerCompletion;
        hr(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&timerCompletion)), "timer completion");
        hr(consumer->Signal(timerCompletion.Get(), 1), "timer host signal");
        check(timer.submitted(cc.Get(), consumer.Get(), timerCompletion.Get(), 1), "timer submitted");
        // Resetting a list with a fresh allocator is valid while its older recording
        // is in flight. Fence completion, not just Reset, must prevent slot reuse.
        hr(cc->Reset(b2.Get(), nullptr), "reset inflight list");
        pool.onReset(cc.Get());
        timer.onReset(cc.Get());
        check(!timer.poll(), "inflight timestamp returned");
        check(!pool.capture(pc.Get(), source.Get(), readState, 5), "inflight reader slot reused");
        hr(gate->Signal(1), "release gate");
        drain(device.Get(), consumer.Get());
        auto timing = timer.poll();
        check(timing.has_value() && timing->milliseconds >= 0, "completed timing unavailable");
        check(!timer.poll(), "duplicate timing delivered");
        std::printf("GPU_TIMER_OK milliseconds=%.6f early_poll_empty=1 completed_sample=1 duplicate_rejected=1 "
                    "timer_cpu_waits=0 timer_queue_signals=0\n",
                    timing->milliseconds);
        void* data = nullptr;
        hr(output->Map(0, nullptr, &data), "map");
        unsigned checked = 0;
        for (unsigned i = 0; i < 4; ++i)
            for (unsigned y = 0; y < H; ++y)
                for (unsigned x = 0; x < W; ++x)
                {
                    float value;
                    std::memcpy(&value, (char*) data + total * i + y * footprint.Footprint.RowPitch + x * 4, 4);
                    check(value == .25f, "snapshot pixel corrupted");
                    ++checked;
                }
        output->Unmap(0, nullptr);
        auto replacement = pool.capture(pc.Get(), source.Get(), readState, 5);
        check(bool(replacement), "completed slot not reused");
        check(!pool.beginRead(tokens[replacement.slot], cc.Get(), tokens[replacement.slot].generation),
              "stale token accepted");
        // Cancel an unsubmitted recording, then verify that the abandoned slot frees.
        hr(pc->Close(), "close canceled pc");
        hr(a->Reset(), "reset canceled allocator");
        hr(pc->Reset(a.Get(), nullptr), "reset canceled pc");
        pool.onReset(pc.Get());
        auto canceledReplacement = pool.capture(pc.Get(), source.Get(), readState, 6);
        check(bool(canceledReplacement), "canceled slot leaked");
        hr(pc->Close(), "close final pc");
        producer->ExecuteCommandLists(1, pLists);
        check(pool.afterSubmit(producer.Get(), 1, pLists), "final tracking");
        drain(device.Get(), producer.Get());
        hr(cc->Close(), "close empty cc");
        pool.releaseAfterGpuDrain();
        timer.releaseAfterGpuDrain();
        unsigned errors = 0;
        if (info)
        {
            for (UINT64 i = 0; i < info->GetNumStoredMessages(); ++i)
            {
                SIZE_T size = 0;
                info->GetMessage(i, nullptr, &size);
                auto message = (D3D12_MESSAGE*) std::malloc(size);
                info->GetMessage(i, message, &size);
                if (message->Severity <= D3D12_MESSAGE_SEVERITY_ERROR)
                {
                    std::printf("D3D12_ERROR %s\n", message->pDescription);
                    ++errors;
                }
                std::free(message);
            }
        }
        check(errors == 0, "D3D12 validation error");
        std::printf("SNAPSHOT_POOL_OK slots=4 pixels=%u inflight_reuse_rejected=1 stale_token_rejected=1 "
                    "canceled_recording_reused=1 debug_layer=%u debug_errors=%u\n",
                    checked, debugEnabled, errors);
        return 0;
    }
    catch (const std::exception& e)
    {
        if (gate)
            gate->Signal(1);
        std::fprintf(stderr, "FAILED %s\n", e.what());
        return 1;
    }
}
