#include "../ExperimentRecording.h"
#include "ExperimentGpuPayload.h"
#include <cstdio>
using Microsoft::WRL::ComPtr;
void require(bool value, const char* reason) { if (!value) throw std::runtime_error(reason); }
void check(HRESULT result) { require(SUCCEEDED(result), "D3D12 call failed"); }
int wmain(int argc, wchar_t** argv)
{
    ComPtr<ID3D12Device> device;
    ComPtr<ID3D12CommandQueue> queue;
    ComPtr<ID3D12Fence> gate, completed;
    try
    {
        require(argc == 2, "ExperimentGpu <fixture directory>");
        const auto dir = std::filesystem::canonical(argv[1]);
        check(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&device)));
        D3D12_COMMAND_QUEUE_DESC qd {};
        check(device->CreateCommandQueue(&qd, IID_PPV_ARGS(&queue)));
        check(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&gate)));
        check(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&completed)));
        ComPtr<ID3D12CommandAllocator> firstAllocator, secondAllocator;
        check(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&firstAllocator)));
        check(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&secondAllocator)));
        ComPtr<ID3D12GraphicsCommandList> command;
        check(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, firstAllocator.Get(), nullptr,
                                       IID_PPV_ARGS(&command)));
        D3D12_HEAP_PROPERTIES heap {}; heap.Type = D3D12_HEAP_TYPE_READBACK;
        D3D12_RESOURCE_DESC desc {}; desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        desc.Width = 1024; desc.Height = 1; desc.DepthOrArraySize = desc.MipLevels = 1; desc.SampleDesc.Count = 1;
        desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        ComPtr<ID3D12Resource> output;
        check(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_COPY_DEST,
                                             nullptr, IID_PPV_ARGS(&output)));
        GlassFg::ExperimentRuntime runtime;
        GlassExperimentHost host { sizeof(host), GLASS_EXPERIMENT_ABI, GlassExperimentFg, device.Get() };
        const auto a = dir / "experiment-gpu-a.dll", b = dir / "experiment-gpu-b.dll";
        runtime.replace(a, host);
        auto frame = runtime.beginFrame(1, 3);
        GlassFg::ExperimentRecording use(frame.retain(), command.Get(), 1);
        ExperimentGpuPayload payload { command.Get(), output.Get(), 0 };
        GlassExperimentEvent event { sizeof(event), GlassExperimentFg, 1, 1, 3, 1, sizeof(payload), &payload };
        require(frame.dispatch(event) == 1, "A first phase failed");
        runtime.replace(b, host);
        for (UINT phase = 2; phase <= 3; ++phase)
        {
            event.phase = phase; payload.offset = (phase - 1) * 256;
            require(frame.dispatch(event) == 1, "GPU phases mixed module generation");
        }
        check(command->Close());
        require(!use.retire(), "Unsubmitted recording released before discard");
        check(queue->Wait(gate.Get(), 1)); // Test-only artificial GPU delay.
        ID3D12CommandList* lists[] { command.Get() };
        queue->ExecuteCommandLists(1, lists);
        check(queue->Signal(completed.Get(), 1));
        require(use.submitted(completed.Get(), 1), "Completion ownership rejected");
        frame = {};
        require(runtime.collect() == 0 && GetModuleHandleW(a.c_str()), "In-flight GPU module unloaded");
        check(command->Reset(secondAllocator.Get(), nullptr));
        require(!use.discard(command.Get(), 2), "Wrong recording epoch discarded");
        require(use.discard(command.Get(), 1), "Actual reset rejected");
        require(!use.retire() && runtime.collect() == 0, "Reset treated as GPU completion");
        check(gate->Signal(1));
        const auto wait = [&](UINT64 value)
        {
            HANDLE done = CreateEventW(nullptr, FALSE, FALSE, nullptr);
            require(done != nullptr, "Completion event failed");
            check(completed->SetEventOnCompletion(value, done));
            const DWORD result = WaitForSingleObject(done, 15000);
            CloseHandle(done);
            require(result == WAIT_OBJECT_0, "GPU completion timeout");
            check(device->GetDeviceRemovedReason());
        };
        wait(1);
        require(use.retire() && runtime.collect() == 1 && !GetModuleHandleW(a.c_str()), "A not safely retired");
        auto frameB = runtime.beginFrame(2, 1);
        GlassFg::ExperimentRecording useB(frameB.retain(), command.Get(), 2);
        payload.offset = 768; event.frame = 2; event.phase = event.phaseCount = 1;
        require(frameB.dispatch(event) == 2, "B GPU work failed");
        check(command->Close());
        queue->ExecuteCommandLists(1, lists);
        check(queue->Signal(completed.Get(), 2));
        require(useB.submitted(completed.Get(), 2), "B completion rejected");
        frameB = {}; runtime.disable(); wait(2);
        require(!useB.retire(), "GPU completion treated as recording discard");
        check(firstAllocator->Reset());
        check(command->Reset(firstAllocator.Get(), nullptr));
        require(useB.discard(command.Get(), 2) && useB.retire(), "B reset/completion did not retire");
        check(command->Close());
        require(runtime.collect() == 1 && !GetModuleHandleW(b.c_str()), "B remained loaded");
        runtime.replace(a, host);
        auto unsubmitted = runtime.beginFrame(3, 1);
        GlassFg::ExperimentRecording unused(unsubmitted.retain(), command.Get(), 3);
        check(firstAllocator->Reset());
        check(command->Reset(firstAllocator.Get(), nullptr));
        event.frame = 3; payload.offset = 0;
        require(unsubmitted.dispatch(event) == 1, "Unsubmitted fixture draw failed");
        check(command->Close());
        unsubmitted = {}; runtime.disable();
        require(!unused.retire() && runtime.collect() == 0, "Unsubmitted recording lost its module");
        check(firstAllocator->Reset());
        check(command->Reset(firstAllocator.Get(), nullptr));
        require(unused.discard(command.Get(), 3) && unused.retire(), "Discarded unsubmitted recording retained");
        check(command->Close());
        require(runtime.collect() == 1, "Unsubmitted module retirement failed");
        void* bytes = nullptr; D3D12_RANGE range { 0, 1024 };
        check(output->Map(0, &range, &bytes));
        for (unsigned i = 0; i < 256; ++i)
            require(static_cast<unsigned*>(bytes)[i] == (i < 192 ? 1u : 2u), "Module-owned GPU data corrupted");
        D3D12_RANGE noWrite { 0, 0 }; output->Unmap(0, &noWrite);
        puts("PASS real_gpu_module_storage=1 exact_words=256 phases_old_module=3 reset_before_completion_retained=1 "
             "completion_before_reset_retained=1 unsubmitted_discard=1 wrong_epoch_rejected=1 actual_unload=3 game_hooks=0");
        return 0;
    }
    catch (const std::exception& error)
    {
        if (gate) gate->Signal(1);
        fprintf(stderr, "FAIL %s\n", error.what()); return 1;
    }
}
