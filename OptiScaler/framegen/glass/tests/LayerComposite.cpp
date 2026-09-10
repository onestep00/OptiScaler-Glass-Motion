// Standalone D3D12 shader runner. Caller-owned float4 files, no game attachment.
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <d3dcompiler.h>
#include <wrl/client.h>
#include <array>
#include <vector>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <cstdio>
#include <cmath>
#include <cstring>
using Microsoft::WRL::ComPtr;
static void check(HRESULT hr)
{
    if (FAILED(hr))
        throw std::runtime_error("D3D operation " + std::to_string(static_cast<unsigned>(hr)));
}
int wmain(int argc, wchar_t** argv)
{
    try
    {
        if (argc != 7 && argc != 8)
            throw std::runtime_error("shader input-directory width height phase output-file [admitted]");
        const std::filesystem::path folder(argv[2]), output(argv[6]);
        if (std::filesystem::exists(output))
            throw std::runtime_error("Output already exists");
        const unsigned width = std::stoul(argv[3]), height = std::stoul(argv[4]);
        const float phase = std::stof(argv[5]);
        if (!width || !height || width > 8192 || height > 8192 || !std::isfinite(phase) || phase < 0 || phase > 1)
            throw std::runtime_error("Invalid dimensions or phase");
        ComPtr<IDXGIFactory4> factory;
        check(CreateDXGIFactory1(IID_PPV_ARGS(&factory)));
        ComPtr<ID3D12Device> device;
        for (unsigned i = 0;; ++i)
        {
            ComPtr<IDXGIAdapter1> adapter;
            if (factory->EnumAdapters1(i, &adapter) == DXGI_ERROR_NOT_FOUND)
                break;
            DXGI_ADAPTER_DESC1 desc {};
            check(adapter->GetDesc1(&desc));
            if (desc.VendorId == 0x10de && !(desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE))
            {
                check(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&device)));
                break;
            }
        }
        if (!device)
            throw std::runtime_error("NVIDIA adapter unavailable");
        ComPtr<ID3D12CommandQueue> queue;
        D3D12_COMMAND_QUEUE_DESC qd {};
        qd.Type = D3D12_COMMAND_LIST_TYPE_COMPUTE;
        check(device->CreateCommandQueue(&qd, IID_PPV_ARGS(&queue)));
        ComPtr<ID3D12CommandAllocator> allocator;
        check(device->CreateCommandAllocator(qd.Type, IID_PPV_ARGS(&allocator)));
        ComPtr<ID3D12GraphicsCommandList> cmd;
        check(device->CreateCommandList(0, qd.Type, allocator.Get(), nullptr, IID_PPV_ARGS(&cmd)));
        D3D12_DESCRIPTOR_RANGE ranges[2] = { { D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 8, 0, 0, 0 },
                                             { D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1, 0, 0, 8 } };
        D3D12_ROOT_PARAMETER parameters[2] {};
        parameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        parameters[0].DescriptorTable = { 2, ranges };
        parameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        parameters[1].Constants = { 0, 0, 4 };
        D3D12_STATIC_SAMPLER_DESC sampler {};
        sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
        sampler.AddressU = sampler.AddressV = sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        sampler.ComparisonFunc = D3D12_COMPARISON_FUNC_ALWAYS;
        sampler.MaxLOD = D3D12_FLOAT32_MAX;
        D3D12_ROOT_SIGNATURE_DESC rd { 2, parameters, 1, &sampler, D3D12_ROOT_SIGNATURE_FLAG_NONE };
        ComPtr<ID3DBlob> rb, errors, shader;
        check(D3D12SerializeRootSignature(&rd, D3D_ROOT_SIGNATURE_VERSION_1, &rb, &errors));
        ComPtr<ID3D12RootSignature> root;
        check(device->CreateRootSignature(0, rb->GetBufferPointer(), rb->GetBufferSize(), IID_PPV_ARGS(&root)));
        auto hr =
            D3DCompileFromFile(argv[1], nullptr, D3D_COMPILE_STANDARD_FILE_INCLUDE, "Composite", "cs_5_0",
                               D3DCOMPILE_ENABLE_STRICTNESS | D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &shader, &errors);
        if (errors)
            printf("%s\n", static_cast<const char*>(errors->GetBufferPointer()));
        check(hr);
        D3D12_COMPUTE_PIPELINE_STATE_DESC pd {};
        pd.pRootSignature = root.Get();
        pd.CS = { shader->GetBufferPointer(), shader->GetBufferSize() };
        ComPtr<ID3D12PipelineState> pso;
        check(device->CreateComputePipelineState(&pd, IID_PPV_ARGS(&pso)));
        D3D12_DESCRIPTOR_HEAP_DESC hd {};
        hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        hd.NumDescriptors = 9;
        hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        ComPtr<ID3D12DescriptorHeap> heap;
        check(device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&heap)));
        auto handle = heap->GetCPUDescriptorHandleForHeapStart();
        const auto increment = device->GetDescriptorHandleIncrementSize(hd.Type);
        D3D12_RESOURCE_DESC td {};
        td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        td.Width = width;
        td.Height = height;
        td.DepthOrArraySize = td.MipLevels = 1;
        td.SampleDesc.Count = 1;
        td.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp {};
        UINT64 bytes;
        device->GetCopyableFootprints(&td, 0, 1, 0, &fp, nullptr, nullptr, &bytes);
        auto make = [&](D3D12_RESOURCE_DESC desc, D3D12_HEAP_TYPE type, D3D12_RESOURCE_STATES state)
        {
            D3D12_HEAP_PROPERTIES hp {};
            hp.Type = type;
            hp.CreationNodeMask = hp.VisibleNodeMask = 1;
            ComPtr<ID3D12Resource> resource;
            check(device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &desc, state, nullptr,
                                                  IID_PPV_ARGS(&resource)));
            return resource;
        };
        auto transition = [&](ID3D12Resource* resource, D3D12_RESOURCE_STATES from, D3D12_RESOURCE_STATES to)
        {
            D3D12_RESOURCE_BARRIER b {};
            b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            b.Transition = { resource, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, from, to };
            cmd->ResourceBarrier(1, &b);
        };
        D3D12_RESOURCE_DESC bd {};
        bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        bd.Width = bytes;
        bd.DepthOrArraySize = bd.MipLevels = 1;
        bd.Height = bd.SampleDesc.Count = 1;
        bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        std::array<ComPtr<ID3D12Resource>, 9> textures;
        std::array<ComPtr<ID3D12Resource>, 8> uploads;
        for (unsigned i = 0; i < 9; ++i)
        {
            if (i == 8)
                td.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
            textures[i] = make(td, D3D12_HEAP_TYPE_DEFAULT,
                               i == 8 ? D3D12_RESOURCE_STATE_UNORDERED_ACCESS : D3D12_RESOURCE_STATE_COPY_DEST);
            if (i == 8)
                device->CreateUnorderedAccessView(textures[i].Get(), nullptr, nullptr, handle);
            else
            {
                D3D12_SHADER_RESOURCE_VIEW_DESC view {};
                view.Format = td.Format;
                view.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
                view.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
                view.Texture2D.MipLevels = 1;
                device->CreateShaderResourceView(textures[i].Get(), &view, handle);
                const auto file = folder / ("input-" + std::to_string(i) + ".bin");
                if (std::filesystem::file_size(file) != UINT64(width) * height * 16)
                    throw std::runtime_error("Input length mismatch");
                uploads[i] = make(bd, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
                void* data;
                check(uploads[i]->Map(0, nullptr, &data));
                std::ifstream stream(file, std::ios::binary);
                for (unsigned y = 0; y < height; ++y)
                    if (!stream.read(static_cast<char*>(data) + fp.Offset + y * fp.Footprint.RowPitch,
                                     size_t(width) * 16))
                        throw std::runtime_error("Input read failed");
                uploads[i]->Unmap(0, nullptr);
                D3D12_TEXTURE_COPY_LOCATION dst {}, src {};
                dst.pResource = textures[i].Get();
                dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
                src.pResource = uploads[i].Get();
                src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
                src.PlacedFootprint = fp;
                cmd->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
                transition(textures[i].Get(), D3D12_RESOURCE_STATE_COPY_DEST,
                           D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            }
            handle.ptr += increment;
        }
        ID3D12DescriptorHeap* heaps[] = { heap.Get() };
        cmd->SetDescriptorHeaps(1, heaps);
        cmd->SetComputeRootSignature(root.Get());
        cmd->SetComputeRootDescriptorTable(0, heap->GetGPUDescriptorHandleForHeapStart());
        const unsigned admitted = argc == 8 ? std::stoul(argv[7]) : 1;
        struct
        {
            unsigned width, height;
            float phase;
            unsigned admitted;
        } constants { width, height, phase, admitted };
        cmd->SetComputeRoot32BitConstants(1, 4, &constants, 0);
        cmd->SetPipelineState(pso.Get());
        cmd->Dispatch((width + 7) / 8, (height + 7) / 8, 1);
        transition(textures[8].Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
        auto readback = make(bd, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST);
        D3D12_TEXTURE_COPY_LOCATION dst {}, src {};
        dst.pResource = readback.Get();
        dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        dst.PlacedFootprint = fp;
        src.pResource = textures[8].Get();
        src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        cmd->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
        check(cmd->Close());
        ID3D12CommandList* lists[] = { cmd.Get() };
        queue->ExecuteCommandLists(1, lists);
        ComPtr<ID3D12Fence> fence;
        check(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)));
        check(queue->Signal(fence.Get(), 1));
        HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (!event)
            throw std::runtime_error("Event creation failed");
        check(fence->SetEventOnCompletion(1, event));
        const auto wait = WaitForSingleObject(event, 30000);
        CloseHandle(event);
        if (wait != WAIT_OBJECT_0)
            throw std::runtime_error("GPU completion timeout");
        check(device->GetDeviceRemovedReason());
        void* data;
        check(readback->Map(0, nullptr, &data));
        std::ofstream stream(output, std::ios::binary);
        for (unsigned y = 0; y < height; ++y)
            stream.write(static_cast<char*>(data) + fp.Offset + y * fp.Footprint.RowPitch, size_t(width) * 16);
        stream.close();
        readback->Unmap(0, nullptr);
        if (!stream)
            throw std::runtime_error("Output write failed");
        printf("LAYER_COMPOSITE pixels=%u phase=%.2f game_attachment=0\n", width * height, phase);
        return 0;
    }
    catch (const std::exception& error)
    {
        printf("FAILED %s\n", error.what());
        return 1;
    }
}
