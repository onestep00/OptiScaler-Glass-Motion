#pragma once
#include "GlassControls.h"
#include "PackedMotionCapture.h"
#include <d3d12.h>
#include <d3dcompiler.h>
#include <cstdio>
#include <vector>

namespace GlassFg
{
class PackedMotionGpu
{
    ID3D12Device* device = nullptr;
    ID3D12DescriptorHeap* heap = nullptr;
    ID3D12RootSignature* root = nullptr;
    ID3D12PipelineState* pipeline = nullptr;
    ID3D12Resource* motion = nullptr;
    ID3D12Resource* depth = nullptr;
    ID3D12Resource* selection = nullptr;
    unsigned width = 0, height = 0, increment = 0;

    D3D12_CPU_DESCRIPTOR_HANDLE cpu(unsigned index) const
    {
        auto value = heap->GetCPUDescriptorHandleForHeapStart();
        value.ptr += SIZE_T(index) * increment;
        return value;
    }
    D3D12_GPU_DESCRIPTOR_HANDLE gpu(unsigned index) const
    {
        auto value = heap->GetGPUDescriptorHandleForHeapStart();
        value.ptr += UINT64(index) * increment;
        return value;
    }
    static void transition(ID3D12GraphicsCommandList* command, ID3D12Resource* resource,
                           D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after)
    {
        D3D12_RESOURCE_BARRIER barrier {};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition = { resource, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, before, after };
        command->ResourceBarrier(1, &barrier);
    }
    bool createTexture(D3D12_RESOURCE_DESC desc, D3D12_RESOURCE_STATES state, ID3D12Resource** output)
    {
        D3D12_HEAP_PROPERTIES properties {};
        properties.Type = D3D12_HEAP_TYPE_DEFAULT;
        desc.Alignment = 0;
        return SUCCEEDED(device->CreateCommittedResource(&properties, D3D12_HEAP_FLAG_NONE, &desc, state, nullptr,
                                                         IID_PPV_ARGS(output)));
    }
    // Compiles the compose shader and swaps the compute PSO. Safe to call again
    // from the live debug channel; the old PSO stays until the new one exists.
    bool compilePipeline(const wchar_t* shader, FILE* log)
    {
        FILE* file = _wfopen(shader, L"rb");
        if (!file)
            return false;
        std::fseek(file, 0, SEEK_END);
        const auto size = std::ftell(file);
        std::rewind(file);
        std::vector<char> source(size > 0 ? static_cast<size_t>(size) : 0);
        const bool read = size > 0 && std::fread(source.data(), 1, source.size(), file) == source.size();
        std::fclose(file);
        if (!read)
            return false;
        ID3DBlob* code = nullptr;
        ID3DBlob* errors = nullptr;
        auto result = D3DCompile(source.data(), source.size(), "glass-object-motion.hlsl", nullptr, nullptr,
                                 "ApplyObjectMotion", "cs_5_0",
                                 D3DCOMPILE_OPTIMIZATION_LEVEL3 | D3DCOMPILE_ENABLE_STRICTNESS, 0, &code, &errors);
        if (errors)
        {
            std::fprintf(log, "OBJECT_SHADER %s\n", static_cast<const char*>(errors->GetBufferPointer()));
            errors->Release();
        }
        if (FAILED(result))
            return false;
        D3D12_COMPUTE_PIPELINE_STATE_DESC pipelineDescription {};
        pipelineDescription.pRootSignature = root;
        pipelineDescription.CS = { code->GetBufferPointer(), code->GetBufferSize() };
        ID3D12PipelineState* created = nullptr;
        result = device->CreateComputePipelineState(&pipelineDescription, IID_PPV_ARGS(&created));
        code->Release();
        if (FAILED(result))
            return false;
        if (pipeline)
            pipeline->Release();
        pipeline = created;
        return true;
    }

  public:
    PackedMotionGpu() = default;
    PackedMotionGpu(const PackedMotionGpu&) = delete;
    PackedMotionGpu& operator=(const PackedMotionGpu&) = delete;

    bool initialize(ID3D12Device* value, const D3D12_RESOURCE_DESC& motionDescription,
                    const D3D12_RESOURCE_DESC& depthDescription, const wchar_t* shader, FILE* log)
    {
        if (device || !value || !shader || !log || motionDescription.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D ||
            depthDescription.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || !motionDescription.Width ||
            motionDescription.Width > 32768 || !motionDescription.Height || motionDescription.Height > 32768 ||
            motionDescription.Width != depthDescription.Width || motionDescription.Height != depthDescription.Height ||
            motionDescription.Format != DXGI_FORMAT_R16G16B16A16_FLOAT ||
            (depthDescription.Format != DXGI_FORMAT_R32_FLOAT && depthDescription.Format != DXGI_FORMAT_R32_TYPELESS))
            return false;
        device = value;
        width = static_cast<unsigned>(motionDescription.Width);
        height = motionDescription.Height;

        auto outputMotion = motionDescription;
        outputMotion.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        if (!createTexture(outputMotion, D3D12_RESOURCE_STATE_COPY_DEST, &motion))
            return false;
        auto outputDepth = depthDescription;
        outputDepth.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        if (!createTexture(outputDepth, D3D12_RESOURCE_STATE_COPY_DEST, &depth))
            return false;
        auto selected = depthDescription;
        selected.Format = DXGI_FORMAT_R16_FLOAT;
        selected.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        if (!createTexture(selected, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, &selection))
            return false;

        D3D12_DESCRIPTOR_HEAP_DESC heapDescription {};
        heapDescription.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        heapDescription.NumDescriptors = 3;
        heapDescription.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        if (FAILED(device->CreateDescriptorHeap(&heapDescription, IID_PPV_ARGS(&heap))))
            return false;
        increment = device->GetDescriptorHandleIncrementSize(heapDescription.Type);
        ID3D12Resource* outputs[] { motion, depth, selection };
        DXGI_FORMAT formats[] { DXGI_FORMAT_R16G16B16A16_FLOAT, DXGI_FORMAT_R32_FLOAT, DXGI_FORMAT_R16_FLOAT };
        for (unsigned i = 0; i < 3; ++i)
        {
            D3D12_UNORDERED_ACCESS_VIEW_DESC view {};
            view.Format = formats[i];
            view.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
            device->CreateUnorderedAccessView(outputs[i], nullptr, &view, cpu(i));
        }

        D3D12_DESCRIPTOR_RANGE range {};
        range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
        range.NumDescriptors = 3;
        D3D12_ROOT_PARAMETER parameters[3] {};
        parameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
        parameters[0].Descriptor = { 0, 0 };
        parameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        parameters[1].DescriptorTable = { 1, &range };
        parameters[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        parameters[2].Constants = { 0, 0, 8 };
        D3D12_ROOT_SIGNATURE_DESC rootDescription { 3, parameters, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_NONE };
        ID3DBlob* serialized = nullptr;
        ID3DBlob* errors = nullptr;
        auto result = D3D12SerializeRootSignature(&rootDescription, D3D_ROOT_SIGNATURE_VERSION_1, &serialized, &errors);
        if (errors)
        {
            std::fprintf(log, "OBJECT_ROOT %s\n", static_cast<const char*>(errors->GetBufferPointer()));
            errors->Release();
        }
        if (FAILED(result))
            return false;
        result = device->CreateRootSignature(0, serialized->GetBufferPointer(), serialized->GetBufferSize(),
                                             IID_PPV_ARGS(&root));
        serialized->Release();
        if (FAILED(result))
            return false;

        if (!compilePipeline(shader, log))
            return false;
        std::fprintf(log, "OBJECT_MOTION_GPU_READY %ux%u passes=1 edge_samples_max=32\n", width, height);
        std::fflush(log);
        return true;
    }

    bool reload(const wchar_t* shader, FILE* log)
    {
        if (!device || !root || !shader || !log)
            return false;
        if (!compilePipeline(shader, log))
            return false;
        std::fprintf(log, "OBJECT_MOTION_GPU_RELOADED %ux%u\n", width, height);
        std::fflush(log);
        return true;
    }

    bool dispatch(ID3D12GraphicsCommandList* command, const PackedMotionFrame& packed,
                  ID3D12Resource* originalMotion, ID3D12Resource* originalDepth,
                  D3D12_RESOURCE_STATES motionState, D3D12_RESOURCE_STATES depthState,
                  float scaleX, float scaleY, Controls controls)
    {
        if (!pipeline || !command || !packed || packed.width != width || packed.height != height ||
            !originalMotion || !originalDepth || scaleX <= 0 || scaleY <= 0)
            return false;
        if (motionState != D3D12_RESOURCE_STATE_COPY_SOURCE)
            transition(command, originalMotion, motionState, D3D12_RESOURCE_STATE_COPY_SOURCE);
        command->CopyResource(motion, originalMotion);
        if (motionState != D3D12_RESOURCE_STATE_COPY_SOURCE)
            transition(command, originalMotion, D3D12_RESOURCE_STATE_COPY_SOURCE, motionState);
        if (depthState != D3D12_RESOURCE_STATE_COPY_SOURCE)
            transition(command, originalDepth, depthState, D3D12_RESOURCE_STATE_COPY_SOURCE);
        command->CopyResource(depth, originalDepth);
        if (depthState != D3D12_RESOURCE_STATE_COPY_SOURCE)
            transition(command, originalDepth, D3D12_RESOURCE_STATE_COPY_SOURCE, depthState);

        transition(command, motion, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        transition(command, depth, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        transition(command, selection, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                   D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        struct Constants
        {
            unsigned width, height;
            float scaleX, scaleY;
            unsigned edgeWidth;
            float interiorStrength;
            unsigned debug, reserved;
        } constants { width, height, scaleX, scaleY, (std::min)(controls.edgeWidth, 4u), controls.coverage(), 0, 0 };
        static_assert(sizeof(Constants) == 32);
        command->SetDescriptorHeaps(1, &heap);
        command->SetComputeRootSignature(root);
        command->SetComputeRootShaderResourceView(0, packed.resource->GetGPUVirtualAddress());
        command->SetComputeRootDescriptorTable(1, gpu(0));
        command->SetComputeRoot32BitConstants(2, 8, &constants, 0);
        command->SetPipelineState(pipeline);
        // The packed raster writes this buffer as a UAV/ROV during the draw, so
        // the compute read needs an explicit UAV barrier. Missing it is the
        // hazard that correlated with the 2026-09-14 driver resets.
        D3D12_RESOURCE_BARRIER packedBarrier {};
        packedBarrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
        packedBarrier.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
        packedBarrier.UAV.pResource = packed.resource;
        command->ResourceBarrier(1, &packedBarrier);
        // Staged coverage: run the real dispatch over the configured top rows so
        // a pathological cost cannot time out the GPU; the rest of the frame
        // keeps the copied original motion. Raise GlassFG/PackedRows after a
        // clean run; the INI and settings UI control this without a rebuild.
        const auto rows = (std::min)(height, (std::max)(1u, controls.packedRows));
        command->Dispatch((width + 7) / 8, (rows + 7) / 8, 1);
        transition(command, motion, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_DEST);
        transition(command, depth, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_DEST);
        transition(command, selection, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                   D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        return true;
    }

    ID3D12Resource* motionOutput() const { return motion; }
    ID3D12Resource* depthOutput() const { return depth; }
    ID3D12Resource* selectionOutput() const { return selection; }
    void releaseAfterGpuDrain()
    {
        for (auto** resource : { &motion, &depth, &selection })
        {
            if (*resource)
                (*resource)->Release();
            *resource = nullptr;
        }
        if (pipeline) pipeline->Release();
        if (root) root->Release();
        if (heap) heap->Release();
        pipeline = nullptr; root = nullptr; heap = nullptr; device = nullptr;
    }
};
} // namespace GlassFg
