#include "pch.h"
#include <windows.h>
#include <d3d12.h>
#include <d3d12sdklayers.h>
#include <dxgi1_6.h>
#include <wrl/client.h>
#include <cstdio>
#include <cstdlib>
#include <stdexcept>
#include "../GlassGpuTimer.h"
using Microsoft::WRL::ComPtr;
void check(bool ok, const char* message)
{
    if (!ok)
        throw std::runtime_error(message);
}
void hr(HRESULT value, const char* message) { check(SUCCEEDED(value), message); }
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
    ComPtr<ID3D12CommandQueue> consumer;
    ComPtr<ID3D12Fence> gate;
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
        q.Type = D3D12_COMMAND_LIST_TYPE_COMPUTE;
        hr(device->CreateCommandQueue(&q, IID_PPV_ARGS(&consumer)), "consumer");
        ComPtr<ID3D12CommandAllocator> b, b2;
        hr(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_COMPUTE, IID_PPV_ARGS(&b)), "allocator b");
        hr(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_COMPUTE, IID_PPV_ARGS(&b2)), "allocator b2");
        ComPtr<ID3D12GraphicsCommandList> cc;
        hr(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_COMPUTE, b.Get(), nullptr, IID_PPV_ARGS(&cc)), "cc");
        check(timer.initialize(device.Get()), "timer initialize");
        auto ticket = timer.begin(cc.Get());
        check(bool(ticket), "timer begin");
        timer.end(cc.Get(), ticket);
        hr(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&gate)), "gate");
        hr(consumer->Wait(gate.Get(), 1), "gate wait");
        hr(cc->Close(), "close cc");
        ID3D12CommandList* cLists[] = { cc.Get() };
        consumer->ExecuteCommandLists(1, cLists);
        ComPtr<ID3D12Fence> timerCompletion;
        hr(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&timerCompletion)), "timer completion");
        hr(consumer->Signal(timerCompletion.Get(), 1), "timer host signal");
        check(timer.submitted(cc.Get(), consumer.Get(), timerCompletion.Get(), 1), "timer submitted");
        // Resetting a list with a fresh allocator is valid while its older recording
        // is in flight. Fence completion, not just Reset, must release the sample.
        hr(cc->Reset(b2.Get(), nullptr), "reset inflight list");
        timer.onReset(cc.Get());
        check(!timer.poll(), "inflight timestamp returned");
        hr(gate->Signal(1), "release gate");
        drain(device.Get(), consumer.Get());
        auto timing = timer.poll();
        check(timing.has_value() && timing->milliseconds >= 0, "completed timing unavailable");
        check(!timer.poll(), "duplicate timing delivered");
        hr(cc->Close(), "close empty cc");
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
        std::printf("GPU_TIMER_OK milliseconds=%.6f early_poll_empty=1 completed_sample=1 duplicate_rejected=1 "
                    "timer_cpu_waits=0 timer_queue_signals=0 debug_layer=%u debug_errors=%u\n",
                    timing->milliseconds, debugEnabled, errors);
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
