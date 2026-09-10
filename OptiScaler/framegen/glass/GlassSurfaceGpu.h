#pragma once
#include <windows.h>
#include <d3d12.h>
#include <d3dcompiler.h>
#include <cstdio>
#include <cstring>
#include <vector>

class GlassSurfaceGpu
{
  public:
    GlassSurfaceGpu() = default;
    GlassSurfaceGpu(const GlassSurfaceGpu&) = delete;
    GlassSurfaceGpu& operator=(const GlassSurfaceGpu&) = delete;
    // The owner must finish all submitted command lists before calling this.
    void releaseAfterGpuDrain()
    {
        for (auto& r : inputs)
        {
            if (r)
                r->Release();
            r = nullptr;
        }
        for (auto** r : { &output, &depthOutput, &selection, &previousColor, &previousSurface })
        {
            if (*r)
                (*r)->Release();
            *r = nullptr;
        }
        if (pipeline)
            pipeline->Release();
        pipeline = nullptr;
        if (root)
            root->Release();
        root = nullptr;
        if (heap)
            heap->Release();
        heap = nullptr;
        history = false;
        device = nullptr;
    }
    // Owned snapshots: motion, HUDless, opaque depth, auxiliary surface depth.
    ID3D12Resource *inputs[4] {}, *output {}, *depthOutput {}, *selection {}, *previousColor {}, *previousSurface {};
    unsigned width {}, height {}, colorWidth {}, colorHeight {}, sequence {};
    bool history {};
    bool initialize(ID3D12Device* d, const D3D12_RESOURCE_DESC* descriptions, const wchar_t* shader, FILE* log)
    {
        if (device || !d || !descriptions || !shader || !log)
            return false;
        device = d;
        report = log;
        width = (unsigned) descriptions[0].Width;
        height = descriptions[0].Height;
        colorWidth = (unsigned) descriptions[1].Width;
        colorHeight = descriptions[1].Height;
        if (!width || !height || colorWidth > 3840 || colorHeight > 2160)
            return false;
        for (unsigned i = 0; i < 4; i++)
        {
            auto desc = descriptions[i == 3 ? 2 : i];
            desc.Alignment = 0;
            desc.Flags = D3D12_RESOURCE_FLAG_NONE;
            if (!texture(desc, D3D12_RESOURCE_STATE_COPY_DEST, &inputs[i]))
                return false;
        }
        auto desc = descriptions[0];
        desc.Alignment = 0;
        desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS | D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
        if (!texture(desc, D3D12_RESOURCE_STATE_COPY_DEST, &output))
            return false;
        desc = descriptions[2];
        desc.Alignment = 0;
        desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS | D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
        if (!texture(desc, D3D12_RESOURCE_STATE_COPY_DEST, &depthOutput))
            return false;
        desc.Format = DXGI_FORMAT_R16_FLOAT;
        desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        if (!texture(desc, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, &selection))
            return false;
        desc = descriptions[1];
        desc.Alignment = 0;
        desc.Flags = D3D12_RESOURCE_FLAG_NONE;
        if (!texture(desc, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, &previousColor))
            return false;
        desc = descriptions[2];
        desc.Alignment = 0;
        desc.Flags = D3D12_RESOURCE_FLAG_NONE;
        if (!texture(desc, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, &previousSurface))
            return false;
        D3D12_DESCRIPTOR_HEAP_DESC hd {};
        hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        hd.NumDescriptors = 9;
        hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        if (FAILED(device->CreateDescriptorHeap(&hd, __uuidof(ID3D12DescriptorHeap), (void**) &heap)))
            return false;
        increment = device->GetDescriptorHandleIncrementSize(hd.Type);
        ID3D12Resource* reads[] = { inputs[1], previousColor, inputs[0], inputs[2], inputs[3], previousSurface };
        DXGI_FORMAT formats[] = { DXGI_FORMAT_R8G8B8A8_UNORM,     DXGI_FORMAT_R8G8B8A8_UNORM,
                                  DXGI_FORMAT_R16G16B16A16_FLOAT, DXGI_FORMAT_R32_FLOAT,
                                  DXGI_FORMAT_R32_FLOAT,          DXGI_FORMAT_R32_FLOAT };
        for (unsigned i = 0; i < 6; i++)
        {
            D3D12_SHADER_RESOURCE_VIEW_DESC v {};
            v.Format = formats[i];
            v.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
            v.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            v.Texture2D.MipLevels = 1;
            device->CreateShaderResourceView(reads[i], &v, cpu(i));
        }
        ID3D12Resource* writes[] = { output, depthOutput, selection };
        DXGI_FORMAT writeFormats[] = { DXGI_FORMAT_R16G16B16A16_FLOAT, DXGI_FORMAT_R32_FLOAT, DXGI_FORMAT_R16_FLOAT };
        for (unsigned i = 0; i < 3; i++)
        {
            D3D12_UNORDERED_ACCESS_VIEW_DESC v {};
            v.Format = writeFormats[i];
            v.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
            device->CreateUnorderedAccessView(writes[i], nullptr, &v, cpu(6 + i));
        }
        D3D12_DESCRIPTOR_RANGE ranges[2] {};
        ranges[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        ranges[0].NumDescriptors = 6;
        ranges[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
        ranges[1].NumDescriptors = 3;
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
            fprintf(report, "ROOT %s\n", (char*) errors->GetBufferPointer());
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
        size_t bytes = ftell(file);
        rewind(file);
        std::vector<char> code(bytes);
        bool read = fread(code.data(), 1, bytes, file) == bytes;
        fclose(file);
        if (!read)
            return false;
        blob = nullptr;
        errors = nullptr;
        hr = D3DCompile(code.data(), code.size(), "glass-surface-candidate.hlsl", nullptr, nullptr, "ApplySurface",
                        "cs_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3 | D3DCOMPILE_ENABLE_STRICTNESS, 0, &blob, &errors);
        if (errors)
        {
            fprintf(report, "SHADER %s\n", (char*) errors->GetBufferPointer());
            errors->Release();
        }
        if (FAILED(hr))
            return false;
        D3D12_COMPUTE_PIPELINE_STATE_DESC pd {};
        pd.pRootSignature = root;
        pd.CS = { blob->GetBufferPointer(), blob->GetBufferSize() };
        hr = device->CreateComputePipelineState(&pd, __uuidof(ID3D12PipelineState), (void**) &pipeline);
        blob->Release();
        if (FAILED(hr))
            return false;
        fprintf(report, "SURFACE_GPU_READY %ux%u color=%ux%u compute_passes=1\n", width, height, colorWidth,
                colorHeight);
        fflush(report);
        return true;
    }
    void dispatch(ID3D12GraphicsCommandList* cmd, float scaleX, float scaleY, float jitterX, float jitterY,
                  const float* matrix, bool reset, bool identity, bool validate = true, bool advance = true)
    {
        for (auto r : inputs)
            transition(cmd, r, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        transition(cmd, output, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        transition(cmd, depthOutput, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        transition(cmd, selection, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                   D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        struct Constants
        {
            unsigned w, h;
            float jx, jy, sx, sy, cx, cy;
            unsigned valid, identity, validate, pad;
            float matrix[16];
        };
        static_assert(sizeof(Constants) == 28 * 4);
        Constants c { width,
                      height,
                      jitterX,
                      jitterY,
                      scaleX / width,
                      scaleY / height,
                      1.f / colorWidth,
                      1.f / colorHeight,
                      history && !reset,
                      identity,
                      validate,
                      0,
                      {} };
        memcpy(c.matrix, matrix, 64);
        cmd->SetDescriptorHeaps(1, &heap);
        cmd->SetComputeRootSignature(root);
        cmd->SetComputeRootDescriptorTable(0, gpu(0));
        cmd->SetComputeRootDescriptorTable(1, gpu(6));
        cmd->SetComputeRoot32BitConstants(2, 28, &c, 0);
        cmd->SetPipelineState(pipeline);
        cmd->Dispatch((width + 7) / 8, (height + 7) / 8, 1);
        transition(cmd, output, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_DEST);
        transition(cmd, depthOutput, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_DEST);
        transition(cmd, selection, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                   D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        transition(cmd, inputs[0], D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
        transition(cmd, inputs[2], D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
        transition(cmd, inputs[1], D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
        transition(cmd, inputs[3], D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
        if (advance)
            advanceHistory(cmd);
    }
    void advanceHistory(ID3D12GraphicsCommandList* cmd)
    {
        ID3D12Resource* historyTargets[] = { previousColor, previousSurface };
        unsigned indices[] = { 1, 3 };
        for (unsigned i = 0; i < 2; i++)
        {
            auto input = inputs[indices[i]], previous = historyTargets[i];
            transition(cmd, input, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COPY_SOURCE);
            transition(cmd, previous, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
            cmd->CopyResource(previous, input);
            transition(cmd, previous, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            transition(cmd, input, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
        }
        history = true;
        ++sequence;
    }
    static void transition(ID3D12GraphicsCommandList* cmd, ID3D12Resource* r, D3D12_RESOURCE_STATES before,
                           D3D12_RESOURCE_STATES after)
    {
        D3D12_RESOURCE_BARRIER b {};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition = { r, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, before, after };
        cmd->ResourceBarrier(1, &b);
    }

  private:
    ID3D12Device* device {};
    FILE* report {};
    ID3D12DescriptorHeap* heap {};
    ID3D12RootSignature* root {};
    ID3D12PipelineState* pipeline {};
    unsigned increment {};
    D3D12_CPU_DESCRIPTOR_HANDLE cpu(unsigned i)
    {
        auto h = heap->GetCPUDescriptorHandleForHeapStart();
        h.ptr += i * increment;
        return h;
    }
    D3D12_GPU_DESCRIPTOR_HANDLE gpu(unsigned i)
    {
        auto h = heap->GetGPUDescriptorHandleForHeapStart();
        h.ptr += i * increment;
        return h;
    }
    bool texture(D3D12_RESOURCE_DESC d, D3D12_RESOURCE_STATES state, ID3D12Resource** r)
    {
        D3D12_HEAP_PROPERTIES h {};
        h.Type = D3D12_HEAP_TYPE_DEFAULT;
        h.CreationNodeMask = h.VisibleNodeMask = 1;
        auto hr = device->CreateCommittedResource(&h, D3D12_HEAP_FLAG_NONE, &d, state, nullptr,
                                                  __uuidof(ID3D12Resource), (void**) r);
        if (FAILED(hr))
            fprintf(report, "TEXTURE_ERROR %08x\n", (unsigned) hr);
        return SUCCEEDED(hr);
    }
};
