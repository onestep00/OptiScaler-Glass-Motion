#include "GeometryTestDevice.h"
#include "../GeometryObservationCache.h"
#include "../ExperimentPipelineService.h"
#include <d3dcompiler.h>

int main()
{
    try
    {
        Device device;
        D3D12_ROOT_SIGNATURE_DESC rd {};
        ComPtr<ID3DBlob> serialized, error, vs, ps;
        check(D3D12SerializeRootSignature(&rd, D3D_ROOT_SIGNATURE_VERSION_1, &serialized, &error));
        ComPtr<ID3D12RootSignature> root;
        check(device.d->CreateRootSignature(0, serialized->GetBufferPointer(), serialized->GetBufferSize(),
                                           IID_PPV_ARGS(&root)));
        const char shader[] = "float4 VS(uint id:SV_VertexID):SV_Position{return float4(id==1?1:-1,id==2?1:-1,0.5,1);}"
                              "float4 PS():SV_Target{return float4(1,0,0,1);}";
        check(D3DCompile(shader, sizeof(shader), nullptr, nullptr, nullptr, "VS", "vs_5_0", 0, 0, &vs, &error));
        check(D3DCompile(shader, sizeof(shader), nullptr, nullptr, nullptr, "PS", "ps_5_0", 0, 0, &ps, &error));
        D3D12_GRAPHICS_PIPELINE_STATE_DESC d {};
        d.pRootSignature = root.Get();
        d.VS = { vs->GetBufferPointer(), vs->GetBufferSize() };
        d.PS = { ps->GetBufferPointer(), ps->GetBufferSize() };
        d.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
        d.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
        d.DepthStencilState.DepthEnable = TRUE;
        d.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
        d.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS;
        d.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
        d.NumRenderTargets = 1; d.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
        d.DSVFormat = DXGI_FORMAT_D32_FLOAT; d.SampleDesc.Count = 1; d.SampleMask = UINT_MAX;
        d.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        ComPtr<ID3D12PipelineState> original, second;
        check(device.d->CreateGraphicsPipelineState(&d, IID_PPV_ARGS(&original)));
        check(device.d->CreateGraphicsPipelineState(&d, IID_PPV_ARGS(&second)));
        GlassFg::ExperimentPipelineLease retained;
        {
            GlassFg::GeometryObservationCache cache(1, 1024 * 1024);
            require(cache.observe(original.Get(), d), "Depth-writing descriptor not observed");
            require(cache.observe(original.Get(), d), "Duplicate observation rejected");
            require(!cache.observe(second.Get(), d), "Entry budget exceeded");
            retained = cache.find(original.Get());
            require(retained && !retained->instrumented && !retained->root->extended, "Observation became replayable");
            require(!memcmp(retained->description.VS.pShaderBytecode, d.VS.pShaderBytecode, d.VS.BytecodeLength),
                    "Observed shader differs");
            memset(vs->GetBufferPointer(), 0, vs->GetBufferSize());
            require(memcmp(retained->description.VS.pShaderBytecode, d.VS.pShaderBytecode, d.VS.BytecodeLength) != 0,
                    "Shader bytes were borrowed");
            GlassFg::GeometryObservationCache tiny(1, 1);
            require(!tiny.observe(second.Get(), d), "Byte budget exceeded");
            d.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
            require(!cache.observe(original.Get(), d), "Read-only pass admitted into depth-writing observation");
        }
        void* token = GlassFg::RetainExperimentPipeline(&retained);
        GlassExperimentPipelineView view {}; view.size = sizeof(view);
        require(token && GlassFg::ViewExperimentPipeline(token, &view) && view.originalRoot == root.Get() &&
                !view.extendedRoot && view.descriptor, "Observation lease lost original descriptor");
        GlassFg::ReleaseExperimentPipeline(token);
        puts("GEOMETRY_OBSERVATION_OK owned_bytes bounded_cache original_only retained_after_cache");
        return 0;
    }
    catch (const std::exception& e) { fprintf(stderr, "%s\n", e.what()); return 1; }
}
