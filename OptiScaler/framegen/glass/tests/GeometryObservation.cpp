#include "GeometryTestDevice.h"
#include "../GeometryObservationCache.h"
#include "../ExperimentPipelineService.h"
#include <d3dcompiler.h>
#ifdef GLASS_OBSERVATION_HOST
#include "../GeometryCreation.h"
#include "../GeometryCommands.h"
#include "../ExperimentCensusBridge.h"
static ID3D12PipelineState* observedOriginal = nullptr;
static uint64_t expectedAddress = 0;
static bool censusOkay = false;
#ifdef GLASS_OBSERVATION_NATIVE
extern bool geometryFixturePacket;
static bool nativeTestActive = false;
static struct NativeOwner final : GlassFg::GeometryDrawCaptureOwner
{
    bool received = false;
    bool prepare(ID3D12GraphicsCommandList*, const GlassFg::GeometryDrawView& draw,
                 const GlassFg::GeometryIndexedArguments&, const GlassFg::ExperimentPipelineLease& pipeline,
                 const GlassFg::GraphicsRootBindings&, GlassFg::GeometryPreparedDraw&) noexcept override
    {
        received = pipeline && pipeline->vertexOnlyCapture && pipeline->instrumented && pipeline->root->extended &&
                   pipeline->original.Get() == observedOriginal && !draw.objects.empty();
        return false; // Test dispatch only. No GPU resources or replacement are reserved.
    }
    void finish(ID3D12GraphicsCommandList*, bool) noexcept override {}
} nativeOwner;
#endif
#ifdef GLASS_OBSERVATION_MODULE
static const GlassExperimentApi* bindingsApi = nullptr;
static void* bindingsContext = nullptr;
static bool moduleAccepted = false;
#endif
static const GlassFg::ExperimentCensusObserver census {
    nullptr, [](void*) noexcept { return true; },
    [](void*, const GlassExperimentEvent& event) noexcept
    {
#ifdef GLASS_OBSERVATION_NATIVE
        if (nativeTestActive)
        {
#ifdef GLASS_OBSERVATION_MODULE
            if (bindingsApi) bindingsApi->event(bindingsContext, &event);
#endif
            return;
        }
#endif
        if (event.kind != GlassExperimentCensus || event.payloadVersion != GLASS_EXPERIMENT_CENSUS_VERSION ||
            event.payloadBytes != sizeof(GlassExperimentCensusInput)) return;
        const auto& input = *static_cast<const GlassExperimentCensusInput*>(event.payload);
        if (input.draw.originalPipeline != observedOriginal) return;
        const auto& draw = input.draw;
        GlassExperimentBinding slot {}; slot.size = sizeof(slot);
        GlassExperimentPipelineView view {}; view.size = sizeof(view);
        void* token = draw.pipelineAccess.retain ? draw.pipelineAccess.retain(draw.pipelineAccess.source) : nullptr;
        const bool layout = token && draw.pipelineAccess.view(token, &view) && view.rootParameterCount == 2 &&
            view.rootParameterBytes == sizeof(D3D12_ROOT_PARAMETER1) && view.rootParameters &&
            static_cast<const D3D12_ROOT_PARAMETER1*>(view.rootParameters)[0].Descriptor.ShaderRegister == 7;
        censusOkay = layout && draw.descriptor && draw.pipelineIdentity && !draw.rootReplayable && draw.bindingAt &&
            draw.bindingAt(draw.bindingSource, 0, &slot) && slot.type == D3D12_ROOT_PARAMETER_TYPE_CBV &&
            slot.address == expectedAddress && !slot.knownConstants &&
            !GlassFg::FindGeometryPipeline(observedOriginal) && GlassFg::FindObservedGeometryPipeline(observedOriginal);
        if (token) draw.pipelineAccess.release(token);
#ifdef GLASS_OBSERVATION_MODULE
        moduleAccepted = bindingsApi && bindingsApi->event(bindingsContext, &event) == 1;
#endif
    }
};
#endif

int main()
{
    try
    {
        Device device;
#ifdef GLASS_OBSERVATION_HOST
        const auto compiler = std::filesystem::absolute("work/glass-optiscaler-source/OptiScaler/shaders/shader_tools/dxcompiler.dll");
        require(GlassFg::StartGeometryCreation(device.d.Get(), compiler), "Creation observer startup");
        require(GlassFg::StartGeometryCommands(device.d.Get()), "Command observer startup");
        require(GlassFg::RegisterExperimentCensus(&census), "Census registration");
#ifdef GLASS_OBSERVATION_MODULE
        const auto modulePath = std::filesystem::absolute("work/glass-optiscaler-source/artifacts/glass-tests/experiment-bindings.dll");
        const auto outputPath = modulePath.parent_path() / ("binding-module-test-" + std::to_string(GetCurrentProcessId()) + "-" + std::to_string(GetTickCount64()));
        auto configPath = modulePath; configPath.replace_extension(".config");
        {
            std::ofstream config(configPath); config << outputPath.generic_string() << '\n';
#ifdef GLASS_OBSERVATION_NATIVE
            config << "prepare-vertex-v1 " << GetCurrentProcessId() << " 9223372036854775809\n";
#endif
        }
        HMODULE bindingsModule = LoadLibraryW(modulePath.c_str());
        require(bindingsModule != nullptr, "Load binding module");
        const auto query = reinterpret_cast<GlassExperimentQueryFn>(GetProcAddress(bindingsModule, "GlassExperimentQuery"));
        bindingsApi = query ? query() : nullptr;
        const GlassExperimentHost host { sizeof(host), GLASS_EXPERIMENT_ABI, GlassExperimentCensus, device.d.Get() };
        require(bindingsApi && bindingsApi->create(&host, &bindingsContext) == 0, "Create binding recorder");
#endif
#endif
        D3D12_ROOT_SIGNATURE_DESC rd {};
        D3D12_DESCRIPTOR_RANGE range { D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 10, 0, 0 };
        D3D12_ROOT_PARAMETER parameters[2] {};
        parameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
        parameters[0].Descriptor = { 7, 0 };
        parameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        parameters[1].DescriptorTable = { 1, &range };
        rd.NumParameters = 2; rd.pParameters = parameters;
        ComPtr<ID3DBlob> serialized, error, vs, ps;
        check(D3D12SerializeRootSignature(&rd, D3D_ROOT_SIGNATURE_VERSION_1, &serialized, &error));
        const auto* serializedStart = static_cast<const char*>(serialized->GetBufferPointer());
        const std::vector<char> serializedCopy(serializedStart, serializedStart + serialized->GetBufferSize());
        ComPtr<ID3D12RootSignature> root;
        check(device.d->CreateRootSignature(0, serialized->GetBufferPointer(), serialized->GetBufferSize(),
                                           IID_PPV_ARGS(&root)));
#ifdef GLASS_OBSERVATION_HOST
        GlassFg::ObserveGeometryRoot(device.d.Get(), 0, serialized->GetBufferPointer(), serialized->GetBufferSize(), root.Get());
#endif
        const char shader[] = "float4 VS(uint id:SV_VertexID):SV_Position{return float4(id==1?1:-1,id==2?1:-1,0.5,1);}"
                              "float4 PS():SV_Target{return float4(1,0,0,1);}";
        check(D3DCompile(shader, sizeof(shader), nullptr, nullptr, nullptr, "VS", "vs_5_0", 0, 0, &vs, &error));
        check(D3DCompile(shader, sizeof(shader), nullptr, nullptr, nullptr, "PS", "ps_5_0", 0, 0, &ps, &error));
#ifdef GLASS_OBSERVATION_NATIVE
        const auto nativeCode = read("work/glass-optiscaler-source/artifacts/glass-tests/observation-native-vs.dxil");
        vs.Reset(); check(D3DCreateBlob(nativeCode.size(), &vs));
        memcpy(vs->GetBufferPointer(), nativeCode.data(), nativeCode.size());
        const auto nativePixel = read("work/glass-optiscaler-source/artifacts/glass-tests/observation-native-ps.dxil");
        ps.Reset(); check(D3DCreateBlob(nativePixel.size(), &ps));
        memcpy(ps->GetBufferPointer(), nativePixel.data(), nativePixel.size());
#endif
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
#ifdef GLASS_OBSERVATION_RETRY
        {
            const auto retryPixel = read("work/glass-optiscaler-source/artifacts/glass-tests/observation-depth-ps.dxil");
            auto retryDesc = d;
            retryDesc.PS = { retryPixel.data(), retryPixel.size() };
            retryDesc.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
            auto& blend = retryDesc.BlendState.RenderTarget[0];
            blend.BlendEnable = TRUE;
            blend.SrcBlend = D3D12_BLEND_SRC_ALPHA; blend.DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
            blend.BlendOp = blend.BlendOpAlpha = D3D12_BLEND_OP_ADD;
            blend.SrcBlendAlpha = D3D12_BLEND_ONE; blend.DestBlendAlpha = D3D12_BLEND_ZERO;
            ComPtr<ID3D12PipelineState> retryPso;
            check(device.d->CreateGraphicsPipelineState(&retryDesc, IID_PPV_ARGS(&retryPso)));
            GlassFg::GeometryPipelineCache cache(device.d.Get(), std::filesystem::absolute(
                "work/glass-optiscaler-source/OptiScaler/shaders/shader_tools/dxcompiler.dll"));
            const auto drain = [&] {
                const auto deadline = GetTickCount64() + 10000;
                while (cache.stats().pending && GetTickCount64() < deadline) Sleep(1);
                require(!cache.stats().pending, "Retry worker timed out");
            };
            require(cache.rootCreated(root.Get(), 0, serialized->GetBufferPointer(), serialized->GetBufferSize()), "Retry root");
            require(cache.pipelineCreated(retryPso.Get(), retryDesc), "Initial material request");
            drain();
            const auto failed = cache.stats();
            require(failed.rejected == 1 && !cache.find(retryPso.Get()), "Depth-output material must fail");
            require(!cache.pipelineCreated(retryPso.Get(), retryDesc), "Failed material reported success");
            require(cache.pipelineCreated(retryPso.Get(), retryDesc, true), "Vertex recovery request");
            drain();
            const auto recovered = cache.find(retryPso.Get());
            const auto after = cache.stats();
            require(recovered && recovered->vertexOnlyCapture && recovered->instrumented,
                    "Vertex recovery not prepared");
            require(after.pipelines == failed.pipelines && after.retainedBytes == failed.retainedBytes &&
                    after.ready == 1 && after.rejected == 1, "Recovery duplicated retained payload");
            require(!memcmp(recovered->description.PS.pShaderBytecode, retryPixel.data(), retryPixel.size()),
                    "Recovery changed original pixel shader");
            require(cache.pipelineCreated(retryPso.Get(), retryDesc, true) && !cache.stats().pending,
                    "Prepared recovery requeued");
            puts("VERTEX_RECOVERY_OK material_failed vertex_ready same_payload original_pixel_preserved");
        }
#endif
        {
            GlassFg::GeometryObservationCache cache;
            auto readOnly = d;
            readOnly.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
            ComPtr<ID3D12PipelineState> readOnlyPso, noDepthPso;
            check(device.d->CreateGraphicsPipelineState(&readOnly, IID_PPV_ARGS(&readOnlyPso)));
            require(cache.observe(readOnlyPso.Get(), readOnly), "Read-only depth observation missing");
            auto noDepth = readOnly;
            noDepth.DepthStencilState.DepthEnable = FALSE;
            noDepth.DSVFormat = DXGI_FORMAT_UNKNOWN;
            check(device.d->CreateGraphicsPipelineState(&noDepth, IID_PPV_ARGS(&noDepthPso)));
            require(cache.observe(noDepthPso.Get(), noDepth), "Depth-disabled observation missing");
            for (auto* pso : { readOnlyPso.Get(), noDepthPso.Get() })
            {
                auto entry = cache.find(pso);
                require(entry && !entry->instrumented && !entry->root->extended,
                        "Additional observation became replayable");
                require(entry->description.DepthStencilState.DepthWriteMask == D3D12_DEPTH_WRITE_MASK_ZERO,
                        "Observation changed depth writes");
            }
            require(!cache.find(noDepthPso.Get())->description.DepthStencilState.DepthEnable,
                    "Observation changed depth enable");
        }
#ifdef GLASS_OBSERVATION_HOST
        observedOriginal = original.Get();
#ifdef GLASS_OBSERVATION_NATIVE
        auto observedLease = GlassFg::FindObservedGeometryPipeline(original.Get());
#endif
        auto constantBuffer = device.buffer(256, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
        expectedAddress = constantBuffer->GetGPUVirtualAddress();
        device.begin();
        device.c->SetGraphicsRootSignature(root.Get());
        device.c->SetGraphicsRootConstantBufferView(0, expectedAddress);
        device.c->SetPipelineState(original.Get());
        device.c->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        device.c->DrawInstanced(0, 0, 0, 0); // Exercise forwarding; no raster work or submission.
        check(device.c->Close());
        require(censusOkay, "Original-only census delivery or replay isolation failed");
#ifdef GLASS_OBSERVATION_NATIVE
        void* nativeToken = GlassFg::RetainExperimentPipeline(&observedLease);
        GlassExperimentPipelineView nativeView {}; nativeView.size = sizeof(nativeView);
        require(GlassFg::ViewExperimentPipeline(nativeToken, &nativeView) && nativeView.requestVertexCapture &&
                !nativeView.extendedRoot, "Missing original-only request service");
#ifndef GLASS_OBSERVATION_MODULE
        require(nativeView.requestVertexCapture(nativeToken) == 1, "Explicit vertex request rejected");
#endif
        GlassFg::ExperimentPipelineLease preparedNative;
        const auto deadline = GetTickCount64() + 10000;
        while (!(preparedNative = GlassFg::FindGeometryPipeline(original.Get())) && GetTickCount64() < deadline)
            Sleep(1); // Independent control-thread compiler wait only.
        require(preparedNative && preparedNative->vertexOnlyCapture && preparedNative->instrumented &&
                preparedNative->root->extended, "Native worker preparation failed");
        const auto requested = GlassFg::GetGeometryCreationStats().cache.pipelines;
        require(nativeView.requestVertexCapture(nativeToken) == 1 &&
                GlassFg::GetGeometryCreationStats().cache.pipelines == requested, "Request was not deduplicated");
        require(!memcmp(preparedNative->description.PS.pShaderBytecode, ps->GetBufferPointer(), ps->GetBufferSize()),
                "Native original PS changed in cache");
        require(GlassFg::RegisterGeometryDrawCapture(&nativeOwner), "Native owner registration");
        nativeTestActive = true; geometryFixturePacket = true;
        device.begin();
        device.c->SetGraphicsRootSignature(root.Get());
        device.c->SetGraphicsRootConstantBufferView(0, expectedAddress);
        device.c->SetPipelineState(original.Get());
        const uint16_t nativeIndices[] { 0, 1, 2, 0, 2, 3 };
        auto nativeIb = device.buffer(sizeof(nativeIndices), D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
        upload(nativeIb.Get(), nativeIndices, sizeof(nativeIndices));
        D3D12_INDEX_BUFFER_VIEW nativeIv { nativeIb->GetGPUVirtualAddress(), sizeof(nativeIndices), DXGI_FORMAT_R16_UINT };
        device.c->IASetIndexBuffer(&nativeIv);
        device.c->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        device.c->DrawIndexedInstanced(6, 1, 0, 2, 7);
        check(device.c->Close()); geometryFixturePacket = false;
        require(nativeOwner.received, "Native prepared entry did not reach actual indexed hook owner");
#endif
        GlassFg::StopGeometryCreation();
        require(!GlassFg::FindObservedGeometryPipeline(original.Get()), "Stopped observation remained active");
#ifdef GLASS_OBSERVATION_NATIVE
        require(nativeView.requestVertexCapture(nativeToken) == 0, "Stopped host accepted native request");
        GlassFg::ReleaseExperimentPipeline(nativeToken);
        puts("NATIVE_REQUEST_OK explicit deduplicated worker_compiled indexed_owner_reached stopped_rejected");
#endif
#ifdef GLASS_OBSERVATION_MODULE
        require(moduleAccepted, "Binding module rejected actual census");
        bindingsApi->destroy(bindingsContext); bindingsContext = nullptr;
        const auto csv = read(outputPath / "bindings.csv");
        std::string text(csv.begin(), csv.end()); std::erase(text, '\r');
        require(text.find(",0,2," + std::to_string(expectedAddress) + ",0\n") != std::string::npos,
                "Saved CBV address differs");
        const auto done = read(outputPath / "bindings.done");
        std::string completion(done.begin(), done.end()); std::erase(completion, '\r');
#ifdef GLASS_OBSERVATION_NATIVE
        require(completion.find("prepare_queued=1\nprepare_request_result=1\nprepared_pipeline_observed=1\n") != std::string::npos,
                "Module worker request or prepared observation missing");
        require(completion.find("rows=2\npipelines=2\n") != std::string::npos,
#else
        require(completion.find("rows=1\npipelines=1\n") != std::string::npos,
#endif
                "Module count/completion invalid");
        require(read(outputPath / "9223372036854775809.root.bin") == serializedCopy, "Saved root bytes differ");
        require(FreeLibrary(bindingsModule) != 0, "Binding module unload");
        printf("BINDING_MODULE_OK %s\n", outputPath.string().c_str());
#endif
#endif
        GlassFg::GraphicsRootBindings bindings;
        GlassFg::GraphicsRootBindings::BorrowedSlot slot;
        bindings.reset(); bindings.setRoot(root.Get());
        require(!bindings.observe(0, root.Get(), slot), "Unset binding observed");
        bindings.address(0, D3D12_ROOT_PARAMETER_TYPE_CBV, 256);
        require(bindings.observe(0, root.Get(), slot) && slot.address == 256 && !slot.constants, "CBV observation");
        bindings.table(1, { 512 }); bindings.heapsChanged();
        require(!bindings.observe(1, root.Get(), slot) && bindings.observe(0, root.Get(), slot), "Heap invalidation");
        const UINT constants[] { 17, 23 };
        bindings.constants(2, 2, constants, 3);
        require(bindings.observe(2, root.Get(), slot) && slot.known == 24 && slot.constants &&
                slot.constants[3] == 17 && slot.constants[4] == 23 && !slot.address, "Partial constant observation");
        bindings.invalidate();
        require(!bindings.observe(2, root.Get(), slot) && !slot.constants, "Invalidated binding leaked");
        bindings.reset(); bindings.setRoot(root.Get());
        require(!bindings.observe(0, root.Get(), slot), "Reset binding leaked");
        GlassFg::ExperimentPipelineLease retained;
        {
            GlassFg::GeometryObservationCache cache(1, 1024 * 1024);
            require(cache.rootCreated(root.Get(), serialized->GetBufferPointer(), serialized->GetBufferSize()), "Root layout observation");
            require(cache.observe(original.Get(), d), "Depth-writing descriptor not observed");
            require(cache.observe(original.Get(), d), "Duplicate observation rejected");
            require(!cache.observe(second.Get(), d), "Entry budget exceeded");
            retained = cache.find(original.Get());
            require(retained && !retained->instrumented && !retained->root->extended, "Observation became replayable");
            memset(serialized->GetBufferPointer(), 0, serialized->GetBufferSize());
            require(retained->root->originalSerialized.size() == serializedCopy.size() &&
                    !memcmp(retained->root->originalSerialized.data(), serializedCopy.data(), serializedCopy.size()),
                    "Original serialized root not owned");
            require(retained->root->originalParameters.size() == 2 &&
                    retained->root->originalParameters[0].Descriptor.ShaderRegister == 7 &&
                    retained->root->originalParameters[1].DescriptorTable.pDescriptorRanges == retained->root->ranges[1].data() &&
                    retained->root->ranges[1][0].BaseShaderRegister == 10, "Root layout not owned");
            require(!memcmp(retained->description.VS.pShaderBytecode, d.VS.pShaderBytecode, d.VS.BytecodeLength),
                    "Observed shader differs");
            memset(vs->GetBufferPointer(), 0, vs->GetBufferSize());
            require(memcmp(retained->description.VS.pShaderBytecode, d.VS.pShaderBytecode, d.VS.BytecodeLength) != 0,
                    "Shader bytes were borrowed");
            GlassFg::GeometryObservationCache tiny(1, 1);
            require(!tiny.observe(second.Get(), d), "Byte budget exceeded");
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
