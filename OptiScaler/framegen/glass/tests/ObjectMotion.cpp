// Independent D3D12 rasterizer for caller-supplied object/camera histories.
// Per-object targets deliberately retain overlapping layers. Diagnostic cost
// and synthetic IDs are not a production capture strategy or engine identity.
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
#include <cstring>
#include <cmath>
using Microsoft::WRL::ComPtr;
static void check(HRESULT hr)
{
    if (FAILED(hr))
        throw std::runtime_error("D3D HRESULT " + std::to_string(static_cast<unsigned>(hr)));
}
static std::vector<char> read(const std::filesystem::path& path)
{
    const auto size = std::filesystem::file_size(path);
    if (!size || size > 1024 * 1024)
        throw std::runtime_error("Invalid fixture input size");
    std::vector<char> bytes(static_cast<size_t>(size));
    std::ifstream file(path, std::ios::binary);
    if (!file.read(bytes.data(), static_cast<std::streamsize>(bytes.size())))
        throw std::runtime_error("Cannot read fixture");
    return bytes;
}
int wmain(int argc, wchar_t** argv)
{
    try
    {
        if (argc != 4)
            throw std::runtime_error("Usage: ObjectMotion.exe shader input-directory new-output-directory");
        const std::filesystem::path input(argv[2]), output(argv[3]);
        if (std::filesystem::exists(output))
            throw std::runtime_error("Output directory already exists");
        unsigned width = 0, height = 0, objects = 0;
        std::ifstream config(input / "config.txt");
        if (!(config >> width >> height >> objects) || !width || !height || width > 2048 || height > 2048 ||
            !objects || objects > 8 || UINT64(width) * height * objects > 8 * 1024 * 1024)
            throw std::runtime_error("Invalid fixture dimensions/count");
        std::string extra;
        if (config >> extra)
            throw std::runtime_error("Extra fixture configuration");
        ComPtr<IDXGIFactory4> factory;
        check(CreateDXGIFactory1(IID_PPV_ARGS(&factory)));
        ComPtr<ID3D12Device> device;
        for (unsigned i = 0; !device; ++i)
        {
            ComPtr<IDXGIAdapter1> adapter;
            if (factory->EnumAdapters1(i, &adapter) == DXGI_ERROR_NOT_FOUND)
                break;
            DXGI_ADAPTER_DESC1 desc {};
            check(adapter->GetDesc1(&desc));
            if (desc.VendorId == 0x10de && !(desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE))
                check(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&device)));
        }
        if (!device)
            throw std::runtime_error("NVIDIA adapter unavailable");
        D3D12_COMMAND_QUEUE_DESC qd {};
        qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        ComPtr<ID3D12CommandQueue> queue;
        check(device->CreateCommandQueue(&qd, IID_PPV_ARGS(&queue)));
        ComPtr<ID3D12CommandAllocator> allocator;
        check(device->CreateCommandAllocator(qd.Type, IID_PPV_ARGS(&allocator)));
        ComPtr<ID3D12GraphicsCommandList> cmd;
        check(device->CreateCommandList(0, qd.Type, allocator.Get(), nullptr, IID_PPV_ARGS(&cmd)));
        D3D12_ROOT_PARAMETER parameter {};
        parameter.ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
        parameter.Descriptor = { 0, 0 };
        D3D12_ROOT_SIGNATURE_DESC rd { 1, &parameter, 0, nullptr,
                                      D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT };
        ComPtr<ID3DBlob> rootBytes, errors;
        check(D3D12SerializeRootSignature(&rd, D3D_ROOT_SIGNATURE_VERSION_1, &rootBytes, &errors));
        ComPtr<ID3D12RootSignature> root;
        check(device->CreateRootSignature(0, rootBytes->GetBufferPointer(), rootBytes->GetBufferSize(), IID_PPV_ARGS(&root)));
        auto compile = [&](const char* entry, const char* target)
        {
            ComPtr<ID3DBlob> shader, error;
            auto hr = D3DCompileFromFile(argv[1], nullptr, D3D_COMPILE_STANDARD_FILE_INCLUDE, entry, target,
                                        D3DCOMPILE_ENABLE_STRICTNESS | D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &shader, &error);
            if (error)
                printf("%s\n", static_cast<const char*>(error->GetBufferPointer()));
            check(hr);
            return shader;
        };
        auto vs = compile("VS", "vs_5_0"), ps = compile("PS", "ps_5_0");
        const D3D12_INPUT_ELEMENT_DESC layout[] = {
            { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
            { "POSITION", 1, DXGI_FORMAT_R32G32B32_FLOAT, 0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
            { "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 24, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 }
        };
        D3D12_GRAPHICS_PIPELINE_STATE_DESC pd {};
        pd.pRootSignature = root.Get();
        pd.VS = { vs->GetBufferPointer(), vs->GetBufferSize() };
        pd.PS = { ps->GetBufferPointer(), ps->GetBufferSize() };
        pd.InputLayout = { layout, 3 };
        pd.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
        pd.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
        pd.RasterizerState.DepthClipEnable = TRUE;
        pd.DepthStencilState.DepthEnable = TRUE;
        pd.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
        pd.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS;
        pd.DSVFormat = DXGI_FORMAT_D32_FLOAT;
        pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        constexpr unsigned TargetCount = 3;
        pd.NumRenderTargets = TargetCount;
        pd.SampleMask = UINT_MAX;
        pd.SampleDesc.Count = 1;
        for (unsigned i = 0; i < TargetCount; ++i)
        {
            pd.RTVFormats[i] = DXGI_FORMAT_R32G32B32A32_FLOAT;
            pd.BlendState.RenderTarget[i].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
        }
        ComPtr<ID3D12PipelineState> pso;
        check(device->CreateGraphicsPipelineState(&pd, IID_PPV_ARGS(&pso)));
        auto make = [&](D3D12_RESOURCE_DESC desc, D3D12_HEAP_TYPE type, D3D12_RESOURCE_STATES state)
        {
            D3D12_HEAP_PROPERTIES hp {};
            hp.Type = type;
            hp.CreationNodeMask = hp.VisibleNodeMask = 1;
            ComPtr<ID3D12Resource> resource;
            check(device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &desc, state, nullptr, IID_PPV_ARGS(&resource)));
            return resource;
        };
        auto buffer = [&](UINT64 bytes, D3D12_HEAP_TYPE type, D3D12_RESOURCE_STATES state)
        {
            D3D12_RESOURCE_DESC bd {};
            bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
            bd.Width = bytes;
            bd.Height = bd.SampleDesc.Count = 1;
            bd.DepthOrArraySize = bd.MipLevels = 1;
            bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
            return make(bd, type, state);
        };
        auto upload = [&](const std::vector<char>& bytes, UINT64 allocated)
        {
            auto resource = buffer(allocated, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
            void* mapped = nullptr;
            D3D12_RANGE empty { 0, 0 };
            check(resource->Map(0, &empty, &mapped));
            memset(mapped, 0, static_cast<size_t>(allocated));
            memcpy(mapped, bytes.data(), bytes.size());
            resource->Unmap(0, nullptr);
            return resource;
        };
        D3D12_DESCRIPTOR_HEAP_DESC hd {};
        hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
        hd.NumDescriptors = TargetCount * objects;
        ComPtr<ID3D12DescriptorHeap> rtvHeap;
        check(device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&rtvHeap)));
        const auto rtvStart = rtvHeap->GetCPUDescriptorHandleForHeapStart();
        const auto rtvIncrement = device->GetDescriptorHandleIncrementSize(hd.Type);
        hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
        hd.NumDescriptors = 1;
        ComPtr<ID3D12DescriptorHeap> dsvHeap;
        check(device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&dsvHeap)));
        const auto dsv = dsvHeap->GetCPUDescriptorHandleForHeapStart();
        D3D12_RESOURCE_DESC td {};
        td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        td.Width = width;
        td.Height = height;
        td.DepthOrArraySize = td.MipLevels = 1;
        td.SampleDesc.Count = 1;
        td.Format = DXGI_FORMAT_D32_FLOAT;
        td.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
        auto depth = make(td, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_DEPTH_WRITE);
        device->CreateDepthStencilView(depth.Get(), nullptr, dsv);
        td.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
        td.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint {};
        UINT64 readbackBytes;
        device->GetCopyableFootprints(&td, 0, 1, 0, &footprint, nullptr, nullptr, &readbackBytes);
        std::vector<ComPtr<ID3D12Resource>> resources, readbacks;
        D3D12_VIEWPORT vp { 0, 0, static_cast<float>(width), static_cast<float>(height), 0, 1 };
        D3D12_RECT scissor { 0, 0, static_cast<LONG>(width), static_cast<LONG>(height) };
        cmd->SetGraphicsRootSignature(root.Get());
        cmd->SetPipelineState(pso.Get());
        cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        cmd->RSSetViewports(1, &vp);
        cmd->RSSetScissorRects(1, &scissor);
        for (unsigned object = 0; object < objects; ++object)
        {
            auto constants = read(input / ("constants-" + std::to_string(object) + ".bin"));
            auto vertices = read(input / ("vertices-" + std::to_string(object) + ".bin"));
            if (constants.size() != 288 || vertices.size() % (32 * 3) || vertices.size() / 32 > 65535)
                throw std::runtime_error("Invalid object record");
            for (const auto* bytes : { &constants, &vertices })
                for (size_t offset = 0; offset < bytes->size(); offset += sizeof(float))
                {
                    float value;
                    memcpy(&value, bytes->data() + offset, sizeof(value));
                    if (!std::isfinite(value))
                        throw std::runtime_error("Nonfinite object record");
                }
            auto cb = upload(constants, 512), vb = upload(vertices, vertices.size());
            cmd->SetGraphicsRootConstantBufferView(0, cb->GetGPUVirtualAddress());
            D3D12_VERTEX_BUFFER_VIEW view { vb->GetGPUVirtualAddress(), static_cast<UINT>(vertices.size()), 32 };
            cmd->IASetVertexBuffers(0, 1, &view);
            resources.push_back(cb);
            resources.push_back(vb);
            std::array<ComPtr<ID3D12Resource>, TargetCount> targets;
            std::array<D3D12_CPU_DESCRIPTOR_HANDLE, TargetCount> rtvs;
            for (unsigned i = 0; i < TargetCount; ++i)
            {
                targets[i] = make(td, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_RENDER_TARGET);
                rtvs[i].ptr = rtvStart.ptr + SIZE_T(object * TargetCount + i) * rtvIncrement;
                device->CreateRenderTargetView(targets[i].Get(), nullptr, rtvs[i]);
                const float clear[] = { 0, 0, 0, 0 };
                cmd->ClearRenderTargetView(rtvs[i], clear, 0, nullptr);
            }
            cmd->ClearDepthStencilView(dsv, D3D12_CLEAR_FLAG_DEPTH, 1, 0, 0, nullptr);
            cmd->OMSetRenderTargets(TargetCount, rtvs.data(), FALSE, &dsv);
            // Split triangles across calls with the same object record. A draw
            // number is not a mesh/object boundary and must not create a seam.
            const auto count = static_cast<UINT>(vertices.size() / 32);
            for (UINT start = 0; start < count; start += 3)
                cmd->DrawInstanced(3, 1, start, 0);
            for (unsigned i = 0; i < TargetCount; ++i)
            {
                D3D12_RESOURCE_BARRIER barrier {};
                barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
                barrier.Transition = { targets[i].Get(), D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
                                       D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE };
                cmd->ResourceBarrier(1, &barrier);
                auto readback = buffer(readbackBytes, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST);
                D3D12_TEXTURE_COPY_LOCATION dst {}, src {};
                dst.pResource = readback.Get();
                dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
                dst.PlacedFootprint = footprint;
                src.pResource = targets[i].Get();
                src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
                cmd->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
                resources.push_back(targets[i]);
                readbacks.push_back(readback);
            }
        }
        check(cmd->Close());
        ComPtr<ID3D12Fence> fence;
        check(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)));
        HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (!event)
            throw std::runtime_error("Event creation failed");
        const auto eventStatus = fence->SetEventOnCompletion(1, event);
        if (FAILED(eventStatus))
        {
            CloseHandle(event);
            check(eventStatus);
        }
        ID3D12CommandList* lists[] = { cmd.Get() };
        queue->ExecuteCommandLists(1, lists);
        if (FAILED(queue->Signal(fence.Get(), 1)))
        {
            fprintf(stderr, "GPU signal failed after submission; ending fixture process\n");
            ExitProcess(3);
        }
        auto waited = WaitForSingleObject(event, 30000);
        CloseHandle(event);
        if (waited != WAIT_OBJECT_0)
        {
            // Do not release submitted resources whose completion is unknown.
            fprintf(stderr, "GPU completion unavailable; ending fixture process\n");
            ExitProcess(3);
        }
        check(device->GetDeviceRemovedReason());
        check(cmd->Reset(allocator.Get(), nullptr));
        check(cmd->Close());
        std::filesystem::create_directories(output);
        for (unsigned i = 0; i < readbacks.size(); ++i)
        {
            void* mapped = nullptr;
            check(readbacks[i]->Map(0, nullptr, &mapped));
            const char* names[] = { "motion-", "coverage-", "weights-" };
            const auto name = std::string(names[i % TargetCount]) + std::to_string(i / TargetCount) + ".bin";
            std::ofstream file(output / name, std::ios::binary);
            for (unsigned y = 0; y < height; ++y)
                file.write(static_cast<const char*>(mapped) + footprint.Offset + SIZE_T(y) * footprint.Footprint.RowPitch,
                           static_cast<std::streamsize>(width) * 16);
            readbacks[i]->Unmap(0, nullptr);
            if (!file)
                throw std::runtime_error("Output write failed");
        }
        printf("OBJECT_MOTION objects=%u width=%u height=%u separate_layers=1 game_attachment=0\n", objects, width, height);
        return 0;
    }
    catch (const std::exception& error)
    {
        fprintf(stderr, "%s\n", error.what());
        return 1;
    }
}
