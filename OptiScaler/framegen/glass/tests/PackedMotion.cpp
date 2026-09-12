#include "GeometryTestDevice.h"

namespace
{
constexpr uint32_t CandidateCount = 7;
constexpr uint32_t PixelCount = 5;

uint64_t pack(uint32_t depth, int32_t motionX, int32_t motionY, uint32_t weight, uint32_t objectId)
{
    return (uint64_t(depth & 0x3ffff) << 46) | (uint64_t(uint32_t(motionX) & 0x7ff) << 35) |
           (uint64_t(uint32_t(motionY) & 0x7ff) << 24) | (uint64_t(weight & 0xff) << 16) |
           uint64_t(objectId & 0xffff);
}

uint64_t candidate(uint32_t layer, uint32_t pixel)
{
    const uint32_t depth = 1 + ((layer * 37 + pixel * 11) % 97);
    const int32_t motionX = int32_t(layer * 23 + pixel * 7) - 96;
    const int32_t motionY = 80 - int32_t(layer * 19 + pixel * 5);
    return pack(depth, motionX, motionY, layer * 29 + pixel * 3, layer * 101 + pixel * 17 + 1);
}
}

int wmain(int argc, wchar_t** argv)
{
    try
    {
        require(argc == 2, "Expected packed-motion compute DXIL");
        Device g;

        D3D12_FEATURE_DATA_SHADER_MODEL shaderModel {D3D_SHADER_MODEL_6_6};
        check(g.d->CheckFeatureSupport(D3D12_FEATURE_SHADER_MODEL, &shaderModel, sizeof(shaderModel)));
        require(shaderModel.HighestShaderModel >= D3D_SHADER_MODEL_6_6, "Shader Model 6.6 unavailable");
        D3D12_FEATURE_DATA_D3D12_OPTIONS1 options {};
        check(g.d->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS1, &options, sizeof(options)));
        require(options.Int64ShaderOps, "Int64 shader operations unavailable");

        D3D12_ROOT_PARAMETER parameters[2] {};
        parameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        parameters[0].Constants.Num32BitValues = 4;
        parameters[0].Constants.ShaderRegister = 0;
        parameters[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        parameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
        parameters[1].Descriptor.ShaderRegister = 0;
        parameters[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        D3D12_ROOT_SIGNATURE_DESC rootDesc {};
        rootDesc.NumParameters = _countof(parameters);
        rootDesc.pParameters = parameters;
        ComPtr<ID3DBlob> rootBytes, rootErrors;
        check(D3D12SerializeRootSignature(&rootDesc, D3D_ROOT_SIGNATURE_VERSION_1, &rootBytes, &rootErrors));
        ComPtr<ID3D12RootSignature> root;
        check(g.d->CreateRootSignature(0, rootBytes->GetBufferPointer(), rootBytes->GetBufferSize(), IID_PPV_ARGS(&root)));

        const auto shader = read(argv[1]);
        D3D12_COMPUTE_PIPELINE_STATE_DESC pipelineDesc {};
        pipelineDesc.pRootSignature = root.Get();
        pipelineDesc.CS = {shader.data(), shader.size()};
        ComPtr<ID3D12PipelineState> pipeline;
        check(g.d->CreateComputePipelineState(&pipelineDesc, IID_PPV_ARGS(&pipeline)));

        constexpr uint64_t bytes = PixelCount * sizeof(uint64_t);
        const std::array<uint64_t, PixelCount> zero {};
        auto initial = g.buffer(bytes, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
        auto output = g.buffer(bytes, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_COPY_DEST,
                               D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
        auto readback = g.buffer(bytes, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST);
        upload(initial.Get(), zero.data(), bytes);

        g.begin();
        g.c->CopyBufferRegion(output.Get(), 0, initial.Get(), 0, bytes);
        g.barrier(output.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        g.c->SetComputeRootSignature(root.Get());
        g.c->SetPipelineState(pipeline.Get());
        const uint32_t constants[4] {CandidateCount, PixelCount, 0, 0};
        g.c->SetComputeRoot32BitConstants(0, _countof(constants), constants, 0);
        g.c->SetComputeRootUnorderedAccessView(1, output->GetGPUVirtualAddress());
        const uint32_t invocationCount = CandidateCount * PixelCount;
        g.c->Dispatch((invocationCount + 7) / 8, 1, 1);
        D3D12_RESOURCE_BARRIER uav {};
        uav.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
        uav.UAV.pResource = output.Get();
        g.c->ResourceBarrier(1, &uav);
        g.barrier(output.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
        g.c->CopyBufferRegion(readback.Get(), 0, output.Get(), 0, bytes);
        g.finish();

        const uint64_t* actual = nullptr;
        const D3D12_RANGE readRange {0, bytes};
        check(readback->Map(0, &readRange, reinterpret_cast<void**>(const_cast<uint64_t**>(&actual))));
        for (uint32_t pixel = 0; pixel < PixelCount; ++pixel)
        {
            uint64_t expected = 0;
            for (uint32_t layer = 0; layer < CandidateCount; ++layer)
                expected = (std::max)(expected, candidate(layer, pixel));
            require(actual[pixel] == expected, "Packed nearest-layer arbitration mismatch");
        }
        const D3D12_RANGE noWrite {0, 0};
        readback->Unmap(0, &noWrite);
        printf("PACKED_MOTION_GPU_OK sm=0x%x int64=1 pixels=%u candidates=%u bytes_per_pixel=8\n",
               uint32_t(shaderModel.HighestShaderModel), PixelCount, CandidateCount);
        return 0;
    }
    catch (const std::exception& error)
    {
        fprintf(stderr, "%s\n", error.what());
        return 1;
    }
}
