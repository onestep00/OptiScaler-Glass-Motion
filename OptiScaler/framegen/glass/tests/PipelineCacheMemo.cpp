// Contract fixture for the GeometryPipelineCache lookup memo.
//
// find() runs for every indexed draw that carries a candidate pipeline, so it
// memoizes ready entries in a thread_local table and skips the shared_lock when
// (epoch, key, generation) still matches. The in-game counters cannot see that
// fast path, and the offline benchmark only measures it. This fixture pins the
// three properties the memo depends on:
//   1. a published entry is returned, and a repeated lookup returns the same
//      entry through the memo path
//   2. a rebuilt cache (new epoch) never answers from a destroyed cache's memo
//      even when the allocator hands back the same pipeline-state address
//   3. stop() must invalidate the memo of the same cache (generation bump)
//
// Usage: PipelineCacheMemo <fixtureDir> <dxcompiler.dll>
#include "GeometryTestDevice.h"
#include "../GeometryPipelineCache.h"
#include <chrono>
#include <cstdio>
#include <memory>
#include <string>
#include <thread>

namespace
{
void step(const char* name)
{
    std::printf("STEP %s\n", name);
    std::fflush(stdout);
}

bool waitReady(const GlassFg::GeometryPipelineCache& cache, std::uint64_t expected)
{
    const auto deadline = GetTickCount64() + 180000;
    for (;;)
    {
        const auto stats = cache.stats();
        if (stats.pending == 0)
            return stats.ready == expected;
        if (GetTickCount64() > deadline)
            return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}
} // namespace

int wmain(int argc, wchar_t** argv)
{
    try
    {
        require(argc >= 3, "usage: PipelineCacheMemo <fixtureDir> <dxcompiler.dll>");
        const std::filesystem::path directory = argv[1];
        const std::filesystem::path compiler = std::filesystem::absolute(argv[2]);
        {
            ComPtr<ID3D12Debug> debug;
            if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug))))
                debug->EnableDebugLayer();
        }
        Device g;
        // instances.dxil carries the per-instance input signature this layout
        // declares; fixture-pixel.dxil is the pixel stage the cache pairs it
        // with. Both come from build_geometry_shader.ps1.
        const auto vs = read(directory / "instances.dxil");
        const auto ps = read(directory / "fixture-pixel.dxil");
        step("shaders");

        D3D12_ROOT_PARAMETER parameter {};
        parameter.ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        parameter.Constants = { 0, 0, 4 };
        parameter.ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;
        D3D12_ROOT_SIGNATURE_DESC rootDesc { 1, &parameter, 0, nullptr,
                                             D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT };
        ComPtr<ID3DBlob> serialized, errors;
        check(D3D12SerializeRootSignature(&rootDesc, D3D_ROOT_SIGNATURE_VERSION_1, &serialized, &errors));
        ComPtr<ID3D12RootSignature> root;
        check(g.d->CreateRootSignature(0, serialized->GetBufferPointer(), serialized->GetBufferSize(),
                                       IID_PPV_ARGS(&root)));
        step("root-signature");

        D3D12_INPUT_ELEMENT_DESC layout[] {
            { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
            { "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
            { "OBJECT_ID", 0, DXGI_FORMAT_R32_UINT, 1, 0, D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA, 1 }
        };
        D3D12_GRAPHICS_PIPELINE_STATE_DESC pd {};
        pd.pRootSignature = root.Get();
        pd.VS = { vs.data(), vs.size() };
        pd.PS = { ps.data(), ps.size() };
        auto& blend = pd.BlendState.RenderTarget[0];
        blend.BlendEnable = TRUE;
        blend.SrcBlend = D3D12_BLEND_SRC_ALPHA;
        blend.DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
        blend.BlendOp = blend.BlendOpAlpha = D3D12_BLEND_OP_ADD;
        blend.SrcBlendAlpha = D3D12_BLEND_ONE;
        blend.DestBlendAlpha = D3D12_BLEND_ZERO;
        blend.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
        pd.SampleMask = UINT_MAX;
        pd.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
        pd.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
        pd.RasterizerState.DepthClipEnable = TRUE;
        pd.DepthStencilState.DepthEnable = TRUE;
        pd.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
        pd.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS;
        pd.InputLayout = { layout, 3 };
        pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        pd.NumRenderTargets = 1;
        pd.RTVFormats[0] = DXGI_FORMAT_R32G32B32A32_FLOAT;
        pd.DSVFormat = DXGI_FORMAT_D32_FLOAT;
        pd.SampleDesc.Count = 1;
        ComPtr<ID3D12PipelineState> state;
        const HRESULT hr = g.d->CreateGraphicsPipelineState(&pd, IID_PPV_ARGS(&state));
        if (FAILED(hr))
        {
            std::printf("PSO_FAIL hr=0x%08lX\n", (unsigned long) hr);
            std::fflush(stdout);
            check(hr);
        }
        step("pipeline-state");

        auto* const identity = state.Get();
        const GlassFg::GeometryCacheLimits limits { 4, 8, 64 * 1024 * 1024 };
        std::shared_ptr<const GlassFg::GeometryPipelineEntry> published;
        {
            GlassFg::GeometryPipelineCache cache(g.d.Get(), compiler, limits);
            require(cache.rootCreated(root.Get(), 0, serialized->GetBufferPointer(), serialized->GetBufferSize()),
                    "root registration");
            require(cache.pipelineCreated(identity, pd), "pipeline registration");
            require(waitReady(cache, 1), "pipeline did not become ready");
            published = cache.find(identity);
            require(bool(published), "published lookup returned null");
            // The second call is the memo path: same epoch, same key, same
            // generation.
            require(cache.find(identity) == published, "repeated lookup returned a different entry");
            require(!cache.find(nullptr), "null lookup resolved");
            step("published");
        }
        step("cache-destroyed");

        {
            // New cache, same pipeline-state address: only the epoch separates
            // this cache from the memo the destroyed cache left behind.
            GlassFg::GeometryPipelineCache cache(g.d.Get(), compiler, limits);
            require(!cache.find(identity), "new cache answered from a destroyed cache's memo");
            step("epoch");
            require(cache.rootCreated(root.Get(), 0, serialized->GetBufferPointer(), serialized->GetBufferSize()),
                    "root registration");
            require(cache.pipelineCreated(identity, pd), "pipeline registration");
            require(waitReady(cache, 1), "pipeline did not become ready");
            require(bool(cache.find(identity)), "republished lookup returned null");
            cache.stop();
            require(!cache.find(identity), "stopped cache answered from the memo");
            step("stopped");
        }
        std::printf("PIPELINE_CACHE_MEMO_OK\n");
        std::fflush(stdout);
        return 0;
    }
    catch (const std::exception& error)
    {
        std::printf("PIPELINE_CACHE_MEMO_FAILED %s\n", error.what());
        std::fflush(stdout);
        return 1;
    }
}
