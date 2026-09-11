#include "GeometryTestDevice.h"
#include "../GeometryCommands.h"
#include "../IndirectBindings.h"
#include <d3dcompiler.h>

static ComPtr<ID3DBlob> compile(const char* code, const char* profile)
{
    ComPtr<ID3DBlob> result, errors;
    check(D3DCompile(code, strlen(code), nullptr, nullptr, nullptr, "main", profile, 0, 0, &result, &errors));
    return result;
}

int main()
{
    try
    {
        Device g;
        const D3D12_INDIRECT_ARGUMENT_DESC draw { D3D12_INDIRECT_ARGUMENT_TYPE_DRAW, {} };
        D3D12_COMMAND_SIGNATURE_DESC desc { 32, 1, &draw, 0 };
        ComPtr<ID3D12CommandSignature> unknown, pure, changed, compute;
        check(g.d->CreateCommandSignature(&desc, nullptr, IID_PPV_ARGS(&unknown)));
        require(GlassFg::StartGeometryCommands(g.d.Get()), "Public command observer");
        desc.ByteStride = 16;
        check(g.d->CreateCommandSignature(&desc, nullptr, IID_PPV_ARGS(&pure)));

        GlassFg::GeometryRoot root;
        D3D12_ROOT_PARAMETER param {};
        param.ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        param.Constants = { 0, 0, 4 };
        D3D12_ROOT_SIGNATURE_DESC rd { 1, &param, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_ALLOW_STREAM_OUTPUT };
        ComPtr<ID3DBlob> bytes, errors;
        check(D3D12SerializeRootSignature(&rd, D3D_ROOT_SIGNATURE_VERSION_1, &bytes, &errors));
        check(g.d->CreateRootSignature(0, bytes->GetBufferPointer(), bytes->GetBufferSize(),
                                       IID_PPV_ARGS(&root.original)));
        D3D12_ROOT_PARAMETER1 observedParam {};
        observedParam.ParameterType = param.ParameterType;
        observedParam.Constants = param.Constants;
        root.originalParameters.push_back(observedParam);
        param.Constants.Num32BitValues = 5;
        check(D3D12SerializeRootSignature(&rd, D3D_ROOT_SIGNATURE_VERSION_1, &bytes, &errors));
        check(g.d->CreateRootSignature(0, bytes->GetBufferPointer(), bytes->GetBufferSize(),
                                       IID_PPV_ARGS(&root.extended)));
        require(root.original.Get() != root.extended.Get(), "Distinct replay root");

        std::array<D3D12_INDIRECT_ARGUMENT_DESC, 2> args {};
        args[0].Type = D3D12_INDIRECT_ARGUMENT_TYPE_CONSTANT;
        args[0].Constant = { 0, 1, 2 };
        args[1] = draw;
        desc = { 24, 2, args.data(), 0 };
        check(g.d->CreateCommandSignature(&desc, root.original.Get(), IID_PPV_ARGS(&changed)));
        args[1].Type = D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH;
        desc.ByteStride = 20;
        check(g.d->CreateCommandSignature(&desc, root.original.Get(), IID_PPV_ARGS(&compute)));

        auto vertex = compile("cbuffer C:register(b0){float4 v;} float4 main():SV_Position{return v;}", "vs_5_0");
        auto cs = compile("[numthreads(1,1,1)] void main(){}", "cs_5_0");
        const D3D12_SO_DECLARATION_ENTRY so { 0, "SV_Position", 0, 0, 4, 0 };
        const UINT stride = 16;
        D3D12_GRAPHICS_PIPELINE_STATE_DESC pd {};
        pd.pRootSignature = root.original.Get();
        pd.VS = { vertex->GetBufferPointer(), vertex->GetBufferSize() };
        pd.StreamOutput = { &so, 1, &stride, 1, D3D12_SO_NO_RASTERIZED_STREAM };
        pd.SampleMask = UINT_MAX;
        pd.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
        pd.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
        pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_POINT;
        pd.SampleDesc.Count = 1;
        ComPtr<ID3D12PipelineState> pipeline, computePipeline;
        check(g.d->CreateGraphicsPipelineState(&pd, IID_PPV_ARGS(&pipeline)));
        D3D12_COMPUTE_PIPELINE_STATE_DESC cpd {};
        cpd.pRootSignature = root.original.Get();
        cpd.CS = { cs->GetBufferPointer(), cs->GetBufferSize() };
        check(g.d->CreateComputePipelineState(&cpd, IID_PPV_ARGS(&computePipeline)));

        std::array<UINT, 64> data {};
        data[8] = 19;
        data[9] = 23;
        data[11] = 1; // Zero vertices, one instance; root changes still reset.
        auto indirect = g.buffer(256, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
        upload(indirect.Get(), data.data(), 256);
        auto stream = g.buffer(256, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_COPY_DEST);
        auto readback = g.buffer(256, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST);
        g.begin();
        g.c->CopyBufferRegion(stream.Get(), 0, indirect.Get(), 0, 8);
        g.barrier(stream.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_STREAM_OUT);
        D3D12_STREAM_OUTPUT_BUFFER_VIEW view { stream->GetGPUVirtualAddress() + 64, 192,
                                               stream->GetGPUVirtualAddress() };
        g.c->SOSetTargets(0, 1, &view);
        g.c->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_POINTLIST);
        g.c->SetGraphicsRootSignature(root.original.Get());
        g.c->SetPipelineState(pipeline.Get());
        const float values[] { 1, 2, 3, 4 };
        const auto drawReplay = [&]
        {
            const auto* bindings = GlassFg::ReadGeometryBindings(g.c.Get());
            require(bindings && bindings->canReplay(root, pipeline.Get()), "Indirect invalidated graphics state");
            const auto saved = *bindings;
            saved.replay(g.c.Get(), root.extended.Get());
            saved.replay(g.c.Get(), root.original.Get());
            g.c->DrawInstanced(1, 1, 0, 0);
        };
        g.c->SetGraphicsRoot32BitConstants(0, 4, values, 0);
        g.c->ExecuteIndirect(pure.Get(), 1, indirect.Get(), 0, nullptr, 0);
        drawReplay();
        g.c->ExecuteIndirect(changed.Get(), 1, indirect.Get(), 32, nullptr, 0);
        drawReplay();
        g.c->SetGraphicsRoot32BitConstants(0, 4, values, 0);
        g.c->SetComputeRootSignature(root.original.Get());
        g.c->SetComputeRoot32BitConstants(0, 4, values, 0);
        g.c->SetPipelineState(computePipeline.Get());
        g.c->ExecuteIndirect(compute.Get(), 1, indirect.Get(), 32, nullptr, 0);
        g.c->SetPipelineState(pipeline.Get());
        drawReplay();
        g.c->ExecuteIndirect(unknown.Get(), 1, indirect.Get(), 0, nullptr, 0);
        const auto* rejected = GlassFg::ReadGeometryBindings(g.c.Get());
        require(rejected && !rejected->canReplay(root, pipeline.Get()), "Unobserved signature was admitted");
        g.barrier(stream.Get(), D3D12_RESOURCE_STATE_STREAM_OUT, D3D12_RESOURCE_STATE_COPY_SOURCE);
        g.c->CopyBufferRegion(readback.Get(), 0, stream.Get(), 0, 256);
        g.finish();
        void* mapped;
        D3D12_RANGE range { 0, 256 }, noWrite { 0, 0 };
        check(readback->Map(0, &range, &mapped));
        require(*static_cast<const UINT64*>(mapped) == 48, "Unexpected stream output count");
        const float expected[] { 1, 2, 3, 4, 1, 0, 0, 4, 1, 2, 3, 4 };
        require(!memcmp(static_cast<const char*>(mapped) + 64, expected, sizeof(expected)),
                "Original GPU root values differ after indirect/replay");
        readback->Unmap(0, &noWrite);
        const auto stats = GlassFg::GetGeometryCommandStats();
        require(stats.signatures == 3 && stats.indirectKnown == 3 && stats.indirectUnknown == 1,
                "Signature observation accounting");
        g.begin();
        g.c->SetGraphicsRootSignature(root.original.Get());
        g.c->SetPipelineState(pipeline.Get());
        const auto* reset = GlassFg::ReadGeometryBindings(g.c.Get());
        require(reset && reset->canReplay(root, pipeline.Get()), "Reset retained unknown signature rejection");
        g.finish();
        printf("PASS public_signatures=3 pure_draw=1 partial_constant_reset=1 compute_isolated=1 "
               "unknown_rejected=1 reset_recovery=1 original_gpu_values=12 game_hooks=0\n");
        return 0;
    }
    catch (const std::exception& e)
    {
        fprintf(stderr, "FAIL %s\n", e.what());
        return 1;
    }
}
