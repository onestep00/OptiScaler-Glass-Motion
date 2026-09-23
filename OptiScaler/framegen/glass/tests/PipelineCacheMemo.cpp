// Contract fixture for the GeometryPipelineCache lookup memo and for the
// pipelines of a vertex shader the graft catalog refused.
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
// The refused VS (Glass/grafts/refused.bin) adds two properties:
//   4. a refused VS whose material rewrite fails is published for the coverage
//      report without a capture variant, and neither pipelineCreated(...,
//      vertexOnly) nor RequestGeometryVertexCapture prepares one for it
//   5. a vertex-only request for a refused VS on a pipeline the cache never
//      admitted fails on the compiler worker: nothing is published and a
//      repeated request is refused
//
// Usage: PipelineCacheMemo <fixtureDir> <dxcompiler.dll>
#include "GeometryTestDevice.h"
#include "Util.h"
#include "../GeometryCreation.h"
#include "../GeometryHealth.h"
#include "../GeometryPipelineCache.h"
#include "../NativeGraftCatalog.h"
#include <bcrypt.h>
#include <algorithm>
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

bool waitCreation()
{
    const auto deadline = GetTickCount64() + 180000;
    while (GlassFg::GetGeometryCreationStats().cache.pending)
    {
        if (GetTickCount64() > deadline)
            return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return true;
}

// Lists vertexShader in the fixture module's Glass/grafts/refused.bin
// ("GGREFS01", one record, reason VehicleObjectMotion) and loads the catalog.
// The catalog reads its files once per process, so the file is removed at
// once: the module directory of GeometryCommandFixture.cpp stays without a
// catalog for the other fixtures that share it.
void refuseVertexShader(const std::vector<char>& vertexShader)
{
    std::array<std::uint8_t, 32> digest {};
    BCRYPT_ALG_HANDLE provider = nullptr;
    require(BCRYPT_SUCCESS(BCryptOpenAlgorithmProvider(&provider, BCRYPT_SHA256_ALGORITHM, nullptr, 0)),
            "SHA-256 provider");
    const bool hashed = BCRYPT_SUCCESS(BCryptHash(provider, nullptr, 0,
                                                  reinterpret_cast<PUCHAR>(const_cast<char*>(vertexShader.data())),
                                                  static_cast<ULONG>(vertexShader.size()), digest.data(),
                                                  static_cast<ULONG>(digest.size())));
    BCryptCloseAlgorithmProvider(provider, 0);
    require(hashed, "refused VS hash");
    const auto grafts = Util::DllPath().parent_path() / L"Glass" / L"grafts";
    std::filesystem::create_directories(grafts);
    {
        const std::uint32_t header[2] { 1, 0 }; // count, reserved
        const auto reason = static_cast<std::uint32_t>(GlassFg::NativeGraftRefusal::VehicleObjectMotion);
        std::ofstream file(grafts / L"refused.bin", std::ios::binary);
        file.write("GGREFS01", 8);
        file.write(reinterpret_cast<const char*>(header), sizeof(header));
        file.write(reinterpret_cast<const char*>(digest.data()), digest.size());
        file.write(reinterpret_cast<const char*>(&reason), sizeof(reason));
        file.close();
        require(bool(file), "refused.bin write");
    }
    const bool loaded = GlassFg::FindNativeGraftRefusal(digest) == GlassFg::NativeGraftRefusal::VehicleObjectMotion &&
                        GlassFg::NativeGraftRefusalCount() == 1;
    // Empty directories only: anything else under the module directory stays.
    std::error_code ignored;
    std::filesystem::remove(grafts / L"refused.bin", ignored);
    std::filesystem::remove(grafts, ignored);
    std::filesystem::remove(grafts.parent_path(), ignored);
    std::filesystem::remove(grafts.parent_path().parent_path(), ignored);
    require(loaded, "refused.bin fixture was not loaded");
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
        // fixture.dxil stands in for a refused vehicle VS; no other fixture that
        // links the graft catalog draws with it. The catalog loads here, before
        // a cache worker hashes any VS.
        const auto refusedVs = read(directory / "fixture.dxil");
        refuseVertexShader(refusedVs);
        step("refused-catalog");

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

        // Refused VS with a multiplicative blend: the creation filter admits any
        // enabled blend, the material rewrite supports no equation that reads
        // the destination colour, so only the refusal publishes the pipeline.
        D3D12_INPUT_ELEMENT_DESC refusedLayout[] {
            { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
            { "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 }
        };
        auto refusedDesc = pd;
        refusedDesc.VS = { refusedVs.data(), refusedVs.size() };
        refusedDesc.InputLayout = { refusedLayout, 2 };
        refusedDesc.BlendState.RenderTarget[0].SrcBlend = D3D12_BLEND_DEST_COLOR;
        refusedDesc.BlendState.RenderTarget[0].DestBlend = D3D12_BLEND_ZERO;
        ComPtr<ID3D12PipelineState> refused;
        check(g.d->CreateGraphicsPipelineState(&refusedDesc, IID_PPV_ARGS(&refused)));
        {
            GlassFg::GeometryPipelineCache cache(g.d.Get(), compiler, limits);
            require(cache.rootCreated(root.Get(), 0, serialized->GetBufferPointer(), serialized->GetBufferSize()),
                    "root registration");
            require(cache.pipelineCreated(refused.Get(), refusedDesc), "refused pipeline registration");
            require(waitReady(cache, 1), "refused pipeline was not published");
            const auto entry = cache.find(refused.Get());
            require(entry && entry->graftKind == GlassFg::GeometryGraftKind::Refused && !entry->instrumented &&
                        !entry->packed,
                    "refused pipeline was not published without a capture variant");
            std::vector<GlassFg::GeometryPipelineCoverage> coverage;
            GlassFg::GeometryPipelineCache::readCoverage(coverage);
            require(std::ranges::any_of(coverage,
                                        [&](const GlassFg::GeometryPipelineCoverage& item) {
                                            return item.identity == entry->identity &&
                                                   item.kind == GlassFg::GeometryGraftKind::Refused;
                                        }),
                    "coverage report does not list the refused pipeline");
            require(!cache.pipelineCreated(refused.Get(), refusedDesc, true),
                    "vertex-only request on the refused pipeline reported success");
            const auto stats = cache.stats();
            require(!stats.pending && stats.pipelines == 1 && cache.find(refused.Get()) == entry &&
                        !entry->vertexOnlyCapture && !entry->instrumented,
                    "vertex-only request requeued the refused pipeline");
            step("refused-published");
        }

        {
            // The resident creation service: its cache sees only hooked
            // creations, and its request service is the one the Experiment
            // modules call.
            require(GlassFg::StartGeometryCreation(g.d.Get(), compiler), "creation observer startup");
            GlassFg::ObserveGeometryRoot(g.d.Get(), 0, serialized->GetBufferPointer(), serialized->GetBufferSize(),
                                         root.Get());
            // Depth writes keep the second pipeline out of the cache at
            // creation; the observation cache still keeps its descriptor.
            auto writing = refusedDesc;
            writing.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
            ComPtr<ID3D12PipelineState> admitted, unadmitted;
            check(g.d->CreateGraphicsPipelineState(&refusedDesc, IID_PPV_ARGS(&admitted)));
            check(g.d->CreateGraphicsPipelineState(&writing, IID_PPV_ARGS(&unadmitted)));
            require(waitCreation(), "creation cache did not finish");
            const auto entry = GlassFg::FindGeometryPipeline(admitted.Get());
            require(entry && entry->graftKind == GlassFg::GeometryGraftKind::Refused && !entry->instrumented &&
                        !entry->packed,
                    "hooked refused pipeline was not published without a capture variant");
            const auto known = GlassFg::GetGeometryCreationStats().cache;
            require(!GlassFg::RequestGeometryVertexCapture(admitted.Get()),
                    "vertex capture request on the refused pipeline succeeded");
            const auto answered = GlassFg::GetGeometryCreationStats().cache;
            require(!answered.pending && answered.pipelines == known.pipelines &&
                        GlassFg::FindGeometryPipeline(admitted.Get()) == entry && !entry->vertexOnlyCapture &&
                        !entry->instrumented,
                    "vertex capture request prepared the refused pipeline");
            step("refused-request");

            require(!GlassFg::FindGeometryPipeline(unadmitted.Get()) &&
                        GlassFg::FindObservedGeometryPipeline(unadmitted.Get()),
                    "depth-writing refused pipeline was not observed only");
            const auto refusals = GlassFg::ReadGeometryGraft(GlassFg::GraftRefused);
            // Queued: the refusal lookup hashes the VS, which only the compiler
            // worker does.
            require(GlassFg::RequestGeometryVertexCapture(unadmitted.Get()), "vertex-only request was not queued");
            require(waitCreation(), "vertex-only job did not finish");
            const auto refusedJob = GlassFg::GetGeometryCreationStats().cache;
            require(!GlassFg::FindGeometryPipeline(unadmitted.Get()) && refusedJob.ready == answered.ready &&
                        refusedJob.rejected == answered.rejected + 1 &&
                        GlassFg::ReadGeometryGraft(GlassFg::GraftRefused) == refusals + 1,
                    "vertex-only job for the refused VS was not refused");
            require(!GlassFg::RequestGeometryVertexCapture(unadmitted.Get()) &&
                        !GlassFg::GetGeometryCreationStats().cache.pending,
                    "refused vertex-only job was queued again");
            GlassFg::StopGeometryCreation();
            step("refused-vertex-only");
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
