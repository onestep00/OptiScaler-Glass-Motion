#pragma once
#include "GlassSurfaceGpu.h"

class GlassRegionGpu
{
    ID3D12Device* device {};
    ID3D12DescriptorHeap* heap {};
    ID3D12RootSignature* root {};
    ID3D12PipelineState* pipelines[4] {};
    ID3D12Resource* buffers[4] {};
    unsigned increment {}, width {}, height {}, cells {};
    D3D12_CPU_DESCRIPTOR_HANDLE cpu(unsigned index)
    {
        auto h = heap->GetCPUDescriptorHandleForHeapStart();
        h.ptr += index * increment;
        return h;
    }
    D3D12_GPU_DESCRIPTOR_HANDLE gpu(unsigned index)
    {
        auto h = heap->GetGPUDescriptorHandleForHeapStart();
        h.ptr += index * increment;
        return h;
    }
    static void barrier(ID3D12GraphicsCommandList* cmd)
    {
        D3D12_RESOURCE_BARRIER b {};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
        cmd->ResourceBarrier(1, &b);
    }

  public:
    GlassRegionGpu() = default;
    GlassRegionGpu(const GlassRegionGpu&) = delete;
    GlassRegionGpu& operator=(const GlassRegionGpu&) = delete;
    // Release before the surface object, after all submitted work has completed.
    void releaseAfterGpuDrain()
    {
        for (auto& r : buffers)
        {
            if (r)
                r->Release();
            r = nullptr;
        }
        for (auto& p : pipelines)
        {
            if (p)
                p->Release();
            p = nullptr;
        }
        if (root)
            root->Release();
        root = nullptr;
        if (heap)
            heap->Release();
        heap = nullptr;
        device = nullptr;
    }
    ID3D12Resource* failureBuffer() { return buffers[3]; }
    bool initialize(ID3D12Device* d, GlassSurfaceGpu& surface, const wchar_t* shader, FILE* log)
    {
        if (device || !d || !shader || !log)
            return false;
        device = d;
        width = surface.width;
        height = surface.height;
        cells = ((width + 3) / 4) * ((height + 3) / 4);
        D3D12_DESCRIPTOR_HEAP_DESC hd {};
        hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        hd.NumDescriptors = 14;
        hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        if (FAILED(device->CreateDescriptorHeap(&hd, __uuidof(ID3D12DescriptorHeap), (void**) &heap)))
            return false;
        increment = device->GetDescriptorHandleIncrementSize(hd.Type);
        ID3D12Resource* reads[] = { surface.inputs[1], surface.previousColor,   surface.inputs[0], surface.inputs[2],
                                    surface.inputs[3], surface.previousSurface, surface.selection };
        DXGI_FORMAT formats[] = { DXGI_FORMAT_R8G8B8A8_UNORM,     DXGI_FORMAT_R8G8B8A8_UNORM,
                                  DXGI_FORMAT_R16G16B16A16_FLOAT, DXGI_FORMAT_R32_FLOAT,
                                  DXGI_FORMAT_R32_FLOAT,          DXGI_FORMAT_R32_FLOAT,
                                  DXGI_FORMAT_R16_FLOAT };
        for (unsigned i = 0; i < 7; i++)
        {
            D3D12_SHADER_RESOURCE_VIEW_DESC v {};
            v.Format = formats[i];
            v.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
            v.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            v.Texture2D.MipLevels = 1;
            device->CreateShaderResourceView(reads[i], &v, cpu(i));
        }
        const unsigned strides[] = { 24, 4, 24, 4 };
        for (unsigned i = 0; i < 4; i++)
        {
            unsigned count = i == 3 ? 1 : cells;
            D3D12_RESOURCE_DESC desc {};
            desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
            desc.Width = (UINT64) strides[i] * count;
            desc.Height = desc.DepthOrArraySize = desc.MipLevels = desc.SampleDesc.Count = 1;
            desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
            desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
            D3D12_HEAP_PROPERTIES h {};
            h.Type = D3D12_HEAP_TYPE_DEFAULT;
            h.CreationNodeMask = h.VisibleNodeMask = 1;
            if (FAILED(device->CreateCommittedResource(&h, D3D12_HEAP_FLAG_NONE, &desc,
                                                       D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr,
                                                       __uuidof(ID3D12Resource), (void**) &buffers[i])))
                return false;
            D3D12_UNORDERED_ACCESS_VIEW_DESC v {};
            v.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
            v.Buffer.NumElements = count;
            v.Buffer.StructureByteStride = strides[i];
            device->CreateUnorderedAccessView(buffers[i], nullptr, &v, cpu(9 + i));
        }
        ID3D12Resource* outputs[] = { surface.output, surface.depthOutput, surface.selection };
        DXGI_FORMAT writeFormats[] = { DXGI_FORMAT_R16G16B16A16_FLOAT, DXGI_FORMAT_R32_FLOAT, DXGI_FORMAT_R16_FLOAT };
        for (unsigned i = 0; i < 3; i++)
        {
            D3D12_UNORDERED_ACCESS_VIEW_DESC v {};
            v.Format = writeFormats[i];
            v.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
            device->CreateUnorderedAccessView(outputs[i], nullptr, &v, cpu(i == 2 ? 13 : 7 + i));
        }
        D3D12_DESCRIPTOR_RANGE ranges[2] {};
        ranges[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        ranges[0].NumDescriptors = 7;
        ranges[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
        ranges[1].NumDescriptors = 7;
        D3D12_ROOT_PARAMETER roots[3] {};
        for (unsigned i = 0; i < 2; i++)
        {
            roots[i].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
            roots[i].DescriptorTable = { 1, &ranges[i] };
        }
        roots[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        roots[2].Constants = { 0, 0, 28 };
        D3D12_STATIC_SAMPLER_DESC sampler {};
        sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
        sampler.AddressU = sampler.AddressV = sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        sampler.MaxLOD = D3D12_FLOAT32_MAX;
        sampler.ComparisonFunc = D3D12_COMPARISON_FUNC_ALWAYS;
        sampler.MaxAnisotropy = 1;
        D3D12_ROOT_SIGNATURE_DESC rd { 3, roots, 1, &sampler, D3D12_ROOT_SIGNATURE_FLAG_NONE };
        ID3DBlob *blob = nullptr, *errors = nullptr;
        auto hr = D3D12SerializeRootSignature(&rd, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &errors);
        if (errors)
        {
            fprintf(log, "REGION_ROOT %s\n", (char*) errors->GetBufferPointer());
            errors->Release();
        }
        if (FAILED(hr))
            return false;
        hr = device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(),
                                         __uuidof(ID3D12RootSignature), (void**) &root);
        blob->Release();
        if (FAILED(hr))
            return false;
        FILE* file = _wfopen(shader, L"rb");
        if (!file)
            return false;
        fseek(file, 0, SEEK_END);
        size_t length = ftell(file);
        rewind(file);
        std::vector<char> code(length);
        bool ok = fread(code.data(), 1, length, file) == length;
        fclose(file);
        if (!ok)
            return false;
        const char* entries[] = { "BuildCells", "JoinCells", "AccumulateRegions", "ApplyRegions" };
        for (unsigned i = 0; i < 4; i++)
        {
            blob = nullptr;
            errors = nullptr;
            hr = D3DCompile(code.data(), code.size(), "glass-region.hlsl", nullptr, nullptr, entries[i], "cs_5_0",
                            D3DCOMPILE_OPTIMIZATION_LEVEL3 | D3DCOMPILE_ENABLE_STRICTNESS, 0, &blob, &errors);
            if (errors)
            {
                fprintf(log, "REGION_SHADER %s %s\n", entries[i], (char*) errors->GetBufferPointer());
                errors->Release();
            }
            if (FAILED(hr))
                return false;
            D3D12_COMPUTE_PIPELINE_STATE_DESC pd {};
            pd.pRootSignature = root;
            pd.CS = { blob->GetBufferPointer(), blob->GetBufferSize() };
            hr = device->CreateComputePipelineState(&pd, __uuidof(ID3D12PipelineState), (void**) &pipelines[i]);
            blob->Release();
            if (FAILED(hr))
                return false;
        }
        fprintf(log, "REGION_GPU_READY cells=%u passes=4 seed_passes=1\n", cells);
        return true;
    }
    void dispatch(ID3D12GraphicsCommandList* cmd, GlassSurfaceGpu& surface, float sx, float sy, float jx, float jy,
                  const float* matrix, bool reset, float strength = 1.f)
    {
        for (auto r : surface.inputs)
            GlassSurfaceGpu::transition(cmd, r, D3D12_RESOURCE_STATE_COPY_DEST,
                                        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        struct Constants
        {
            unsigned w, h;
            float jx, jy, sx, sy, cx, cy;
            unsigned valid, identity, validate;
            float strength;
            float matrix[16];
        };
        Constants c { width,
                      height,
                      jx,
                      jy,
                      sx / width,
                      sy / height,
                      1.f / surface.colorWidth,
                      1.f / surface.colorHeight,
                      surface.history && !reset,
                      0,
                      1,
                      strength,
                      {} };
        memcpy(c.matrix, matrix, 64);
        cmd->SetDescriptorHeaps(1, &heap);
        cmd->SetComputeRootSignature(root);
        cmd->SetComputeRootDescriptorTable(0, gpu(0));
        cmd->SetComputeRootDescriptorTable(1, gpu(7));
        cmd->SetComputeRoot32BitConstants(2, 28, &c, 0);
        for (unsigned i = 0; i < 3; i++)
        {
            cmd->SetPipelineState(pipelines[i]);
            cmd->Dispatch(((width + 3) / 4 + 7) / 8, ((height + 3) / 4 + 7) / 8, 1);
            barrier(cmd);
        }
        GlassSurfaceGpu::transition(cmd, surface.output, D3D12_RESOURCE_STATE_COPY_DEST,
                                    D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        GlassSurfaceGpu::transition(cmd, surface.depthOutput, D3D12_RESOURCE_STATE_COPY_DEST,
                                    D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        GlassSurfaceGpu::transition(cmd, surface.selection, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                                    D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        cmd->SetPipelineState(pipelines[3]);
        cmd->Dispatch((width + 7) / 8, (height + 7) / 8, 1);
        GlassSurfaceGpu::transition(cmd, surface.output, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                                    D3D12_RESOURCE_STATE_COPY_DEST);
        GlassSurfaceGpu::transition(cmd, surface.depthOutput, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                                    D3D12_RESOURCE_STATE_COPY_DEST);
        GlassSurfaceGpu::transition(cmd, surface.selection, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                                    D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        for (auto r : surface.inputs)
            GlassSurfaceGpu::transition(cmd, r, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                                        D3D12_RESOURCE_STATE_COPY_DEST);
        surface.advanceHistory(cmd);
    }
};
