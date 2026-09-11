#include "GeometryTestDevice.h"
#include "../GeometryPipelineCache.h"
#include "../GeometryCreation.h"
#include "../GeometryCommands.h"
#include "../GeometryDrawCapture.h"
#include "../GeometryCoverageRecorder.h"
#include "ExperimentDrawCheck.h"
#include "ModuleRecorderCheck.h"
extern bool geometryFixturePacket;
extern bool geometryFixtureMissingIdentity;
extern std::uint32_t geometryFixtureFrame;
struct CaptureOwner final : GlassFg::GeometryDrawCaptureOwner
{
    GlassFg::GeometryPreparedDraw prepared;
    UINT completed = 0, rasterMatched = 0;
    bool enabled = false;
    bool prepare(ID3D12GraphicsCommandList* command, const GlassFg::GeometryDrawView&,
                 const GlassFg::GeometryIndexedArguments& args,
                 const std::shared_ptr<const GlassFg::GeometryPipelineEntry>&,
                 const GlassFg::GraphicsRootBindings&, GlassFg::GeometryPreparedDraw& out) noexcept override
    {
        if (!enabled || args.instances != 3)
            return false;
        const auto* raster = GlassFg::ReadGeometryRasterState(command);
        if (!raster || !raster->usable() || raster->viewport.Width != 160 || raster->viewport.Height != 112 ||
            raster->targetCount != 1 || !raster->targets[0].ptr || !raster->depth.ptr || raster->scissor.left != 0 ||
            raster->scissor.top != 0 || raster->scissor.right != 160 || raster->scissor.bottom != 112)
            return false;
        ++rasterMatched;
        out = prepared;
        return true;
    }
    void finish(ID3D12GraphicsCommandList*, bool recorded) noexcept override { completed += recorded; }
};
#pragma warning(push, 0)
#include <d3dx/d3dx12.h>
#pragma warning(pop)

static void rootInvalidation(const GlassFg::GeometryRoot& root, ID3D12PipelineState* original)
{
    GlassFg::GraphicsRootBindings state;
    UINT values[64] {};
    state.reset(original);
    state.setRoot(root.original.Get());
    state.constants(0, 64, values, 0);
    require(!state.canReplay(root, original), "Out-of-range constant data admitted");
    state.setRoot(root.original.Get());
    require(!state.canReplay(root, original), "Redundant root binding cleared live constants");
    state.setRoot(root.extended.Get());
    state.setRoot(root.original.Get());
    state.constants(0, 1, values, 3);
    require(state.canReplay(root, original), "Old constant mask survived a root change");
    state.table(63, { 1 });
    require(!state.canReplay(root, original), "Foreign root slot admitted");
    state.heapsChanged();
    require(state.canReplay(root, original), "Invalidated descriptor table remained live");
    state.table(0, { 2 });
    state.heapsChanged();
    state.constants(0, 1, values, 0);
    require(state.canReplay(root, original), "Invalidated table type corrupted new constants");
    state.address(63, D3D12_ROOT_PARAMETER_TYPE_CBV, 256);
    state.heapsChanged();
    require(!state.canReplay(root, original), "Heap switch discarded a root descriptor");
    state.invalidate();
    state.reset(original);
    state.setRoot(root.original.Get());
    state.constants(0, 2, values, 2);
    require(state.canReplay(root, original), "Reset retained old slots or invalidation");
}

static std::shared_ptr<const GlassFg::GeometryPipelineEntry>
observedPipeline(ID3D12Device* device, ID3D12PipelineState* original, const D3D12_GRAPHICS_PIPELINE_STATE_DESC& desc,
                 bool keepActive)
{
    auto extra = desc;
    extra.SampleMask = 0x7fffffff;
    ComPtr<ID3D12Device2> device2;
    check(device->QueryInterface(IID_PPV_ARGS(&device2)));
    CD3DX12_PIPELINE_STATE_STREAM state(extra);
    D3D12_PIPELINE_STATE_STREAM_DESC stream { sizeof(state), &state };
    ComPtr<ID3D12PipelineState> streamPso, excessPso, invalidPso;
    check(device2->CreatePipelineState(&stream, IID_PPV_ARGS(&streamPso)));
    extra.SampleMask = 1;
    check(device->CreateGraphicsPipelineState(&extra, IID_PPV_ARGS(&excessPso)));
    extra.NumRenderTargets = 9;
    require(FAILED(device->CreateGraphicsPipelineState(&extra, IID_PPV_ARGS(&invalidPso))) && !invalidPso,
            "Invalid original PSO creation result was changed");
    require(!GlassFg::FindGeometryPipeline(streamPso.Get()) && !GlassFg::FindGeometryPipeline(excessPso.Get()),
            "Stream or over-budget PSO unexpectedly admitted");
    const auto deadline = GetTickCount64() + 10000;
    while (GlassFg::GetGeometryCreationStats().cache.pending && GetTickCount64() < deadline)
        Sleep(1);
    const auto stats = GlassFg::GetGeometryCreationStats();
    auto lease = GlassFg::FindGeometryPipeline(original);
    if (!lease)
        throw std::runtime_error(stats.cache.lastError.empty() ? "Creation observer did not build pipeline"
                                                               : stats.cache.lastError);
    require(stats.active && stats.roots == 1 && stats.graphics == 2 && stats.streams == 1 && stats.cache.roots == 1 &&
                stats.cache.pipelines == 1 && stats.cache.ready == 1 && !stats.cache.rejected && !stats.cache.pending,
            "Creation observer missed or recursively captured a call");
    if (!keepActive)
    {
        GlassFg::StopGeometryCreation();
        require(!GlassFg::FindGeometryPipeline(original) && !GlassFg::GetGeometryCreationStats().active,
                "Stopped creation observer remained active");
    }
    return lease;
}

static std::shared_ptr<const GlassFg::GeometryPipelineEntry>
cachedPipeline(ID3D12Device* device, ID3D12RootSignature* root, ID3DBlob* serialized, ID3D12PipelineState* original,
               const D3D12_GRAPHICS_PIPELINE_STATE_DESC& desc, const std::filesystem::path& compiler)
{
    GlassFg::GeometryPipelineCache cache(device, compiler, { 1, 1, 4 * 1024 * 1024 });
    require(cache.rootCreated(root, 0, serialized->GetBufferPointer(), serialized->GetBufferSize()), "Cache root");
    require(cache.rootCreated(root, 0, serialized->GetBufferPointer(), serialized->GetBufferSize()),
            "Duplicate cache root");
    std::vector<std::byte> vs(desc.VS.BytecodeLength), ps(desc.PS.BytecodeLength);
    memcpy(vs.data(), desc.VS.pShaderBytecode, vs.size());
    memcpy(ps.data(), desc.PS.pShaderBytecode, ps.size());
    auto supplied = desc;
    supplied.VS = { vs.data(), vs.size() };
    supplied.PS = { ps.data(), ps.size() };
    require(cache.pipelineCreated(original, supplied), "Cache pipeline");
    require(cache.pipelineCreated(original, supplied), "Duplicate cache pipeline");
    std::fill(vs.begin(), vs.end(), std::byte {});
    std::fill(ps.begin(), ps.end(), std::byte {});
    const auto deadline = GetTickCount64() + 10000;
    while (cache.stats().pending && GetTickCount64() < deadline)
        Sleep(1); // Test-only wait.
    auto lease = cache.find(original);
    const auto stats = cache.stats();
    if (!lease)
        throw std::runtime_error(stats.lastError.empty() ? "Compiler worker did not complete" : stats.lastError);
    require(stats.roots == 1 && stats.pipelines == 1 && stats.ready == 1 && !stats.rejected && !stats.pending,
            "Cache accounting or duplicate compilation");
    require(lease->original.Get() == original && lease->description.VS.pShaderBytecode == lease->vertexBytes.data(),
            "Pipeline bytes not owned");
    cache.stop();
    require(!cache.find(original), "Stopped cache admitted a new draw");
    return lease; // Render after the cache and its worker have been destroyed.
}

// Original VS stream output supplies the reference geometry. Object order is
// deliberately changed every frame; original per-instance vertex input carries
// the synthetic identity. This fixture is not an engine identity provider.
static std::array<double, 2> referenceMotion(const Clip* now, const Clip* before, UINT x, UINT y, UINT w, UINT h)
{
    double best = -1e30, weights[3] {}, selectedW[3] {};
    UINT selected = 0;
    for (UINT triangle = 0; triangle < 2; ++triangle)
    {
        double sx[3], sy[3], cw[3];
        for (UINT k = 0; k < 3; ++k)
        {
            const auto& v = now[triangle * 3 + k].xyzw;
            cw[k] = v[3];
            sx[k] = (v[0] / cw[k] * .5 + .5) * w;
            sy[k] = (-v[1] / cw[k] * .5 + .5) * h;
        }
        const double ax = sx[1] - sx[0], ay = sy[1] - sy[0], bx = sx[2] - sx[0], by = sy[2] - sy[0];
        const double px = x + .5 - sx[0], py = y + .5 - sy[0], den = ax * by - ay * bx;
        double b[3];
        b[1] = (px * by - py * bx) / den;
        b[2] = (ax * py - ay * px) / den;
        b[0] = 1 - b[1] - b[2];
        const double score = std::min(b[0], std::min(b[1], b[2]));
        if (score > best)
        {
            best = score;
            selected = triangle * 3;
            memcpy(weights, b, sizeof(b));
            memcpy(selectedW, cw, sizeof(cw));
        }
    }
    require(best > -.002, "Reference outside original object");
    double previous[4] {};
    for (UINT k = 0; k < 3; ++k)
        for (UINT c = 0; c < 4; ++c)
            previous[c] += weights[k] / selectedW[k] * before[selected + k].xyzw[c];
    return { previous[0] / previous[3] * .5 + .5 - (x + .5) / w, -previous[1] / previous[3] * .5 + .5 - (y + .5) / h };
}

int wmain(int argc, wchar_t** argv)
{
    try
    {
        require(argc == 3 || (argc == 4 && (wcscmp(argv[3], L"--observe") == 0 || wcscmp(argv[3], L"--commands") == 0 ||
                                            wcscmp(argv[3], L"--mrt") == 0 || wcscmp(argv[3], L"--dual-mrt") == 0 ||
                                            wcscmp(argv[3], L"--coverage") == 0 ||
                                            wcscmp(argv[3], L"--capture-command") == 0 ||
                                            wcscmp(argv[3], L"--recorder") == 0 ||
                                            wcscmp(argv[3], L"--experiment") == 0 ||
                                            wcscmp(argv[3], L"--capture-module") == 0 ||
                                            wcscmp(argv[3], L"--module-recorder") == 0 ||
                                            wcscmp(argv[3], L"--controlled-recorder") == 0)),
                "GeometryInstances artifact-directory dxcompiler.dll [--observe|--commands|--mrt|--dual-mrt]");
        const bool coverageOnly = argc == 4 && wcscmp(argv[3], L"--coverage") == 0;
        const bool controlledRecorder = argc == 4 && wcscmp(argv[3], L"--controlled-recorder") == 0;
        const bool moduleRecorder = controlledRecorder || (argc == 4 && wcscmp(argv[3], L"--module-recorder") == 0);
        const bool recorder = moduleRecorder || (argc == 4 && wcscmp(argv[3], L"--recorder") == 0);
        const bool captureModule = argc == 4 && wcscmp(argv[3], L"--capture-module") == 0;
        const bool experiment = captureModule || (argc == 4 && wcscmp(argv[3], L"--experiment") == 0);
        const bool captureCommand = experiment || recorder || (argc == 4 && wcscmp(argv[3], L"--capture-command") == 0);
        static CaptureOwner captureOwner;
        const bool observed = argc == 4 && !coverageOnly;
        const bool dual = observed && wcscmp(argv[3], L"--dual-mrt") == 0;
        const bool mrt = dual || (observed && wcscmp(argv[3], L"--mrt") == 0);
        const bool commands = captureCommand || mrt || (observed && wcscmp(argv[3], L"--commands") == 0);
        if (captureCommand && !recorder && !captureModule)
            require(GlassFg::RegisterGeometryDrawCapture(&captureOwner), "Register capture owner");
        const std::filesystem::path dir(argv[1]);
        auto vs = read(dir / "instances.dxil");
        auto ps = read(dir / (dual ? "fixture-dual.dxil" : mrt ? "fixture-mrt.dxil" : "fixture-pixel.dxil"));
        Device g;
        ExperimentDrawCheck experimentCheck;
        ModuleRecorderCheck moduleRecorderCheck;
        if (controlledRecorder) moduleRecorderCheck.useControl();
        if (experiment) experimentCheck.start(g.d.Get(), dir, captureModule);
        const auto recorderOutput = moduleRecorder ? moduleRecorderCheck.start(g.d.Get(), dir, argv[2]) :
            std::filesystem::absolute(dir / ("recorder-" + std::to_string(GetTickCount64())));
        std::filesystem::path secondModuleOutput;
        if (recorder && !moduleRecorder)
        {
            std::filesystem::create_directories(recorderOutput);
            const auto request = recorderOutput / "request.txt";
            { std::ofstream file(request); file << recorderOutput.string() << '\n'; }
            GlassFg::StartGeometryCoverageRecorder(g.d.Get(), std::filesystem::absolute(argv[2]), request);
        }
        if (commands)
        {
            require(GlassFg::StartGeometryViews(g.d.Get()), "Install render-target descriptor observer");
            require(GlassFg::StartGeometryCommands(g.d.Get()), "Install public command observer");
        }
        struct Stop
        {
            ~Stop() { GlassFg::StopGeometryCreation(); }
        } stop;
        if (observed)
            require(GlassFg::StartGeometryCreation(g.d.Get(), std::filesystem::absolute(argv[2]),
                                                   { 1, 1, 4 * 1024 * 1024 }),
                    "Install real creation observer");
        D3D12_ROOT_PARAMETER parameter {};
        parameter.ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        parameter.Constants = { 0, 0, 4 };
        parameter.ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;
        D3D12_ROOT_SIGNATURE_DESC rootDesc { 1, &parameter, 0, nullptr,
                                             D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT |
                                                 D3D12_ROOT_SIGNATURE_FLAG_ALLOW_STREAM_OUTPUT };
        ComPtr<ID3DBlob> serialized, errors;
        check(D3D12SerializeRootSignature(&rootDesc, D3D_ROOT_SIGNATURE_VERSION_1, &serialized, &errors));
        ComPtr<ID3D12RootSignature> originalRoot;
        auto createRoot = [](ID3D12Device* device, UINT node, const void* data, SIZE_T bytes, REFIID iid, void** output)
        { return device->CreateRootSignature(node, data, bytes, iid, output); };
        check(GlassFg::CreateObservedGeometryRoot(createRoot, g.d.Get(), 0, serialized->GetBufferPointer(),
                                                  serialized->GetBufferSize(), IID_PPV_ARGS(&originalRoot)));
        constexpr UINT W = 160, H = 112, SOBytes = 4096, Slots = 64, HistoryBytes = Slots * sizeof(History);
        std::vector<unsigned char> expectedCoverage(recorder ? 9 * 3 * W * H : 0);
        constexpr UINT RoiLeft = 12, RoiTop = 10, RoiWidth = W - 24, RoiHeight = H - 20, RoiStride = RoiWidth + 4;
        constexpr UINT Segment = RoiStride * RoiHeight + 32, CapturePixels = 3 * Segment + 32,
                       CaptureBytes = CapturePixels * 32;
        constexpr UINT historyBase[] = { 9, 31, 49 };
        D3D12_DESCRIPTOR_HEAP_DESC hd { D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 8, D3D12_DESCRIPTOR_HEAP_FLAG_NONE, 0 };
        ComPtr<ID3D12DescriptorHeap> heap, depthHeap;
        check(g.d->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&heap)));
        hd = { D3D12_DESCRIPTOR_HEAP_TYPE_DSV, 1, D3D12_DESCRIPTOR_HEAP_FLAG_NONE, 0 };
        check(g.d->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&depthHeap)));
        const auto dsv = depthHeap->GetCPUDescriptorHandleForHeapStart();
        D3D12_RESOURCE_DESC texture {};
        texture.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        texture.Width = W;
        texture.Height = H;
        texture.DepthOrArraySize = texture.MipLevels = 1;
        texture.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
        texture.SampleDesc.Count = 1;
        texture.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
        D3D12_HEAP_PROPERTIES hp {};
        hp.Type = D3D12_HEAP_TYPE_DEFAULT;
        std::array<ComPtr<ID3D12Resource>, 5> colors;
        std::array<D3D12_CPU_DESCRIPTOR_HANDLE, 5> rtvs;
        const UINT increment = g.d->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
        for (UINT i = 0; i < colors.size(); ++i)
        {
            check(g.d->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &texture, D3D12_RESOURCE_STATE_RENDER_TARGET,
                                               nullptr, IID_PPV_ARGS(&colors[i])));
            rtvs[i] = { heap->GetCPUDescriptorHandleForHeapStart().ptr + i * increment };
            g.d->CreateRenderTargetView(colors[i].Get(), nullptr, rtvs[i]);
        }
        std::array<ComPtr<ID3D12Resource>, 2> auxiliary;
        std::array<D3D12_CPU_DESCRIPTOR_HANDLE, 2> auxiliaryRtvs;
        D3D12_CPU_DESCRIPTOR_HANDLE nullRtv { heap->GetCPUDescriptorHandleForHeapStart().ptr + 7 * increment };
        if (mrt)
        {
            for (UINT i = 0; i < auxiliary.size(); ++i)
            {
                check(g.d->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &texture,
                                                   D3D12_RESOURCE_STATE_RENDER_TARGET, nullptr,
                                                   IID_PPV_ARGS(&auxiliary[i])));
                auxiliaryRtvs[i] = { heap->GetCPUDescriptorHandleForHeapStart().ptr + (5 + i) * increment };
                g.d->CreateRenderTargetView(auxiliary[i].Get(), nullptr, auxiliaryRtvs[i]);
            }
            D3D12_RENDER_TARGET_VIEW_DESC nullView {};
            nullView.Format = texture.Format;
            nullView.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
            g.d->CreateRenderTargetView(nullptr, &nullView, nullRtv);
        }
        auto depthDesc = texture;
        depthDesc.Format = DXGI_FORMAT_D32_FLOAT;
        depthDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
        ComPtr<ID3D12Resource> depth;
        check(g.d->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &depthDesc, D3D12_RESOURCE_STATE_DEPTH_WRITE,
                                           nullptr, IID_PPV_ARGS(&depth)));
        g.d->CreateDepthStencilView(depth.Get(), nullptr, dsv);
        D3D12_INPUT_ELEMENT_DESC layout[] {
            { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
            { "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
            { "OBJECT_ID", 0, DXGI_FORMAT_R32_UINT, 1, 0, D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA, 1 }
        };
        D3D12_GRAPHICS_PIPELINE_STATE_DESC pd {};
        pd.pRootSignature = originalRoot.Get();
        pd.VS = { vs.data(), vs.size() };
        pd.PS = { ps.data(), ps.size() };
        auto& blend = pd.BlendState.RenderTarget[0];
        blend.BlendEnable = TRUE;
        blend.SrcBlend = D3D12_BLEND_ONE;
        blend.DestBlend = dual ? D3D12_BLEND_SRC1_COLOR : D3D12_BLEND_INV_SRC_ALPHA;
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
        pd.NumRenderTargets = mrt ? 3 : 1;
        pd.RTVFormats[0] = texture.Format;
        if (mrt)
            pd.RTVFormats[2] = texture.Format;
        pd.DSVFormat = depthDesc.Format;
        pd.SampleDesc.Count = 1;
        ComPtr<ID3D12PipelineState> original, capturePso, streamPso;
        check(g.d->CreateGraphicsPipelineState(&pd, IID_PPV_ARGS(&original)));
        auto lease = observed ? observedPipeline(g.d.Get(), original.Get(), pd, commands)
                              : cachedPipeline(g.d.Get(), originalRoot.Get(), serialized.Get(), original.Get(), pd,
                                               std::filesystem::absolute(argv[2]));
        const auto& root = *lease->root;
        if (captureModule) experimentCheck.prime(lease, argv[2]);
        rootInvalidation(root, original.Get());
        capturePso = lease->instrumented;
        if (coverageOnly)
        {
            GlassFg::GeometryCompiler compiler(std::filesystem::absolute(argv[2]));
            std::string error;
            if (FAILED(compiler.createCoverage(g.d.Get(), root, pd, capturePso, error)))
                throw std::runtime_error(error);
        }
        require(root.dwords == 22 && root.instanceSlot == 6, "Mapped root extension");
        D3D12_SO_DECLARATION_ENTRY so { 0, "SV_Position", 0, 0, 4, 0 };
        UINT soStride = sizeof(Clip);
        pd.StreamOutput = { &so, 1, &soStride, 1, 0 };
        check(g.d->CreateGraphicsPipelineState(&pd, IID_PPV_ARGS(&streamPso)));
        const Vertex vertices[] = {
            {}, {}, { -.35f, -.5f, 0, 0, 1 }, { -.35f, .5f, 0, 0, 0 }, { .35f, .5f, 0, 1, 0 }, { .35f, -.5f, 0, 1, 1 }
        };
        const uint16_t indices[] = { 0, 1, 2, 0, 2, 3 };
        auto vb = g.buffer(sizeof(vertices), D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
        auto ib = g.buffer(sizeof(indices), D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
        auto objects = g.buffer(10 * sizeof(UINT), D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
        auto mappingBuffer =
            g.buffer(10 * sizeof(GlassFg::GeometryInstance), D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
        auto pixelConstants = g.buffer(256, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
        upload(vb.Get(), vertices, sizeof(vertices));
        upload(ib.Get(), indices, sizeof(indices));
        D3D12_VERTEX_BUFFER_VIEW views[] { { vb->GetGPUVirtualAddress(), sizeof(vertices), sizeof(Vertex) },
                                           { objects->GetGPUVirtualAddress(), 40, 4 } };
        D3D12_INDEX_BUFFER_VIEW indexView { ib->GetGPUVirtualAddress(), sizeof(indices), DXGI_FORMAT_R16_UINT };
        auto capture = g.buffer(CaptureBytes, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_COPY_DEST,
                                D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
        auto stream = g.buffer(SOBytes, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_COPY_DEST);
        std::array<ComPtr<ID3D12Resource>, 2> history;
        for (auto& h : history)
            h = g.buffer(HistoryBytes, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_COPY_DEST,
                         D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
        auto zeros = g.buffer(CaptureBytes, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
        std::vector<char> zero(CaptureBytes);
        upload(zeros.Get(), zero.data(), zero.size());
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint {};
        UINT64 imageBytes;
        g.d->GetCopyableFootprints(&texture, 0, 1, 0, &footprint, nullptr, nullptr, &imageBytes);
        const UINT64 auxiliaryOffset = imageBytes * 5 + SOBytes + HistoryBytes + CaptureBytes;
        const UINT64 readBytes = auxiliaryOffset + (mrt ? 2 * imageBytes : 0);
        auto readback = g.buffer(readBytes, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST);
        ComPtr<ID3D12CommandSignature> censusSignature;
        ComPtr<ID3D12Resource> censusArguments;
        if (moduleRecorder)
        {
            D3D12_INDIRECT_ARGUMENT_DESC argument {}; argument.Type = D3D12_INDIRECT_ARGUMENT_TYPE_DRAW;
            D3D12_COMMAND_SIGNATURE_DESC signature { sizeof(D3D12_DRAW_ARGUMENTS), 1, &argument, 0 };
            check(g.d->CreateCommandSignature(&signature, nullptr, IID_PPV_ARGS(&censusSignature)));
            censusArguments = g.buffer(sizeof(D3D12_DRAW_ARGUMENTS), D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
            const D3D12_DRAW_ARGUMENTS empty { 0, 1, 0, 0 };
            upload(censusArguments.Get(), &empty, sizeof(empty));
        }
        g.begin();
        for (auto& h : history)
        {
            g.c->CopyBufferRegion(h.Get(), 0, zeros.Get(), 0, HistoryBytes);
            g.barrier(h.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        }
        g.c->CopyBufferRegion(capture.Get(), 0, zeros.Get(), 0, CaptureBytes);
        g.barrier(capture.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        g.c->ClearDepthStencilView(dsv, D3D12_CLEAR_FLAG_DEPTH, 1, 0, 0, nullptr);
        D3D12_RECT blocked { 0, 0, W / 2, H };
        g.c->ClearDepthStencilView(dsv, D3D12_CLEAR_FLAG_DEPTH, .3f, 0, 1, &blocked);
        g.finish();
        std::array<Clip, 18> last {};
        std::array<std::vector<char>, 2> priorHistory;
        std::vector<char> priorCapture;
        UINT64 checked = 0, overlaps = 0, exact = 0, recovered = 0, auxiliaryWritten = 0;
        double maximum = 0;
        for (UINT frame = 1; frame <= 8; ++frame)
        {
            if (moduleRecorder && frame == 5)
            {
                moduleRecorderCheck.selectNext(lease->identity, GlassFg::FindGeometryView(rtvs[1], 1)->resource);
                secondModuleOutput = moduleRecorderCheck.replace();
            }
            if (experiment && !captureModule && frame == 2) experimentCheck.prepare(argv[2]);
            if (recorder && !moduleRecorder && frame == 5)
            {
                g.begin(); // Discard the preceding recording before stopping.
                g.finish();
                const auto prefix = L"Local\\OptiScaler.Glass.Capture." + std::to_wstring(GetCurrentProcessId());
                const auto signal = [&](const wchar_t* suffix)
                {
                    HANDLE event = OpenEventW(EVENT_MODIFY_STATE, FALSE, (prefix + suffix).c_str());
                    require(event != nullptr, "Capture control event missing");
                    const bool sent = SetEvent(event) != FALSE;
                    CloseHandle(event);
                    require(sent, "Capture request failed");
                };
                const auto waitFile = [&](const std::filesystem::path& file)
                {
                    const auto until = GetTickCount64() + 10000;
                    while (!std::filesystem::exists(file) && GetTickCount64() < until) Sleep(10);
                    require(std::filesystem::exists(file), "Capture state transition timed out");
                };
                signal(L".Stop");
                waitFile(recorderOutput / "idle.txt");
                signal(L".Start");
                waitFile(recorderOutput / "request-2" / "active.txt");
            }
            if (recorder)
            {
                geometryFixtureFrame = frame;
                geometryFixtureMissingIdentity = frame >= 5;
            }
            UINT order[10] {};
            std::array<GlassFg::GeometryInstance, 10> mapping {};
            for (UINT i = 0; i < 3; ++i)
            {
                const UINT object = (i + frame) % 3;
                order[7 + i] = object;
                const UINT status = 8 + object * Segment;
                mapping[5 + i] = { historyBase[object],
                                   4,
                                   0,
                                   42 + object + (object == 2 && frame >= 3 ? 9u : 0u),
                                   RoiLeft,
                                   RoiTop,
                                   frame == 4 && object == 1 ? 1u : RoiWidth,
                                   RoiHeight,
                                   status + 1,
                                   RoiStride,
                                   CapturePixels,
                                   status };
                if (frame == 6 && object == 1)
                    mapping[5 + i] = {}; // Unknown object stays in the original draw.
            }
            GlassFg::InstanceHistoryConstants hc { 5, 10, Slots, 0, 3, 0, frame, frame - 1 };
            require(hc.valid(mapping, Slots, CapturePixels), "Instance allocation validation");
            auto invalid = hc;
            invalid.mappingBase = 9;
            require(!invalid.valid(mapping, Slots, CapturePixels), "Out-of-range mapping admitted");
            auto badMap = mapping;
            badMap[5].statusIndex = badMap[5].pixelBase;
            require(!hc.valid(badMap, Slots, CapturePixels), "Status/pixel overlap admitted");
            badMap = mapping;
            const auto activeIndex = mapping[5].inactive() ? 6u : 5u;
            badMap[activeIndex].generation = 0;
            require(!hc.valid(badMap, Slots, CapturePixels), "Malformed inactive mapping admitted");
            badMap = {};
            require(!hc.valid(badMap, Slots, CapturePixels), "Entirely inactive draw admitted");
            upload(objects.Get(), order, sizeof(order));
            if (coverageOnly)
                for (auto& entry : mapping)
                {
                    if (entry.inactive())
                        continue;
                    entry.historyBase = entry.vertices = entry.vertexOrigin = 0;
                    entry.pixelBase = (entry.statusIndex + 1) * 32;
                    entry.pixelCapacity = CapturePixels * 32;
                    require(entry.validCoverage(CapturePixels), "Coverage bit allocation validation");
                }
            upload(mappingBuffer.Get(), mapping.data(), sizeof(mapping));
            GlassFg::MaterialCaptureConstants pc { 0, 0, 1.f / W, 1.f / H, 0, 0, frame,         0,
                                                   0, 0, W,       H,       0, W, CapturePixels, 0 };
            upload(pixelConstants.Get(), &pc, sizeof(pc));
            const float fc[] { frame * .31f, frame * .023f, frame * -.017f, 0 };
            const UINT current = frame & 1, previous = current ^ 1;
            g.begin();
            if (moduleRecorder) moduleRecorderCheck.collect();
            g.barrier(capture.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_DEST);
            if (coverageOnly)
                g.c->CopyBufferRegion(capture.Get(), 0, zeros.Get(), 0, CaptureBytes);
            for (UINT object = 0; object < 3; ++object)
                g.c->CopyBufferRegion(capture.Get(), (8 + object * Segment) * 32, zeros.Get(), 0, 32);
            g.barrier(capture.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            if (frame == 3)
            {
                g.barrier(history[previous].Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                          D3D12_RESOURCE_STATE_COPY_DEST);
                g.c->CopyBufferRegion(history[previous].Get(), (historyBase[0] + 3) * 32 + 16, zeros.Get(), 0, 4);
                g.barrier(history[previous].Get(), D3D12_RESOURCE_STATE_COPY_DEST,
                          D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            }
            g.barrier(history[current].Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                      D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            g.c->CopyBufferRegion(stream.Get(), 0, zeros.Get(), 0, SOBytes);
            g.barrier(stream.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_STREAM_OUT);
            D3D12_VIEWPORT viewport { 0, 0, float(W), float(H), 0, 1 };
            D3D12_RECT scissor { 0, 0, W, H };
            g.c->RSSetViewports(1, &viewport);
            g.c->RSSetScissorRects(1, &scissor);
            g.c->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            g.c->IASetVertexBuffers(0, 2, views);
            g.c->IASetIndexBuffer(&indexView);
            for (UINT pass = 0; pass < 5; ++pass)
            {
                const bool instrument = pass == 1;
                const bool directInstrument = instrument && !captureCommand;
                g.c->SetGraphicsRootSignature(directInstrument ? root.extended.Get() : originalRoot.Get());
                g.c->SetGraphicsRoot32BitConstants(0, 4, fc, 0);
                if (commands)
                {
                    g.c->SetGraphicsRoot32BitConstants(0, 2, fc, 0);
                    UINT bits;
                    memcpy(&bits, fc + 2, sizeof(bits));
                    g.c->SetGraphicsRoot32BitConstant(0, bits, 2);
                    memcpy(&bits, fc + 3, sizeof(bits));
                    g.c->SetGraphicsRoot32BitConstant(0, bits, 3);
                }
                g.c->SetPipelineState(directInstrument ? capturePso.Get() : pass == 0 ? streamPso.Get() : original.Get());
                if (captureCommand && instrument)
                {
                    captureOwner.prepared = { capturePso.Get(), hc, history[previous]->GetGPUVirtualAddress(),
                        history[current]->GetGPUVirtualAddress(), pixelConstants->GetGPUVirtualAddress(),
                        capture->GetGPUVirtualAddress(), mappingBuffer->GetGPUVirtualAddress() };
                    if (experimentCheck.pipeline()) captureOwner.prepared.pipeline = experimentCheck.pipeline();
                    if (captureModule) experimentCheck.configure(captureOwner.prepared);
                    captureOwner.enabled = true;
                    geometryFixturePacket = true;
                }
                if (directInstrument)
                {
                    g.c->SetGraphicsRoot32BitConstants(root.constantsSlot, 8, &hc, 0);
                    g.c->SetGraphicsRootShaderResourceView(root.previousSlot,
                                                           history[previous]->GetGPUVirtualAddress());
                    g.c->SetGraphicsRootUnorderedAccessView(root.currentSlot, history[current]->GetGPUVirtualAddress());
                    g.c->SetGraphicsRootConstantBufferView(root.materialSlot, pixelConstants->GetGPUVirtualAddress());
                    g.c->SetGraphicsRootUnorderedAccessView(root.captureSlot, capture->GetGPUVirtualAddress());
                    g.c->SetGraphicsRootShaderResourceView(root.instanceSlot, mappingBuffer->GetGPUVirtualAddress());
                }
                D3D12_STREAM_OUTPUT_BUFFER_VIEW streamView {};
                if (!pass)
                    streamView = { stream->GetGPUVirtualAddress() + 256, SOBytes - 256,
                                   stream->GetGPUVirtualAddress() };
                g.c->SOSetTargets(0, 1, &streamView);
                g.c->OMSetRenderTargets(1, &rtvs[pass], FALSE, &dsv);
                if (mrt)
                {
                    const UINT auxiliaryIndex = pass == 1 ? 1 : 0;
                    D3D12_CPU_DESCRIPTOR_HANDLE targets[] { rtvs[pass], nullRtv, auxiliaryRtvs[auxiliaryIndex] };
                    g.c->OMSetRenderTargets(3, targets, FALSE, &dsv);
                    const float auxiliaryClear[] { .125f, .25f, .375f, .5f };
                    g.c->ClearRenderTargetView(auxiliaryRtvs[auxiliaryIndex], auxiliaryClear, 0, nullptr);
                }
                const float clear[4] {};
                g.c->ClearRenderTargetView(rtvs[pass], clear, 0, nullptr);
                if (pass < 2)
                {
                    g.c->DrawIndexedInstanced(6, 3, 0, 2, 7);
                    geometryFixturePacket = false;
                    captureOwner.enabled = false;
                    if (moduleRecorder && pass == 0)
                    {
                        g.c->DrawInstanced(0, 1, 0, 0);
                        g.c->ExecuteIndirect(censusSignature.Get(), 1, censusArguments.Get(), 0, nullptr, 0);
                    }
                }
                else
                {
                    UINT index = 0;
                    while (order[7 + index] != pass - 2)
                        ++index;
                    if (commands)
                    {
                        const auto* bindings = GlassFg::ReadGeometryBindings(g.c.Get());
                        require(bindings && bindings->canReplay(root, original.Get()),
                                "Public graphics bindings missing");
                        // Test-only snapshot. Restore after deliberately changing
                        // the root; the following GPU result must remain exact.
                        auto saved = *bindings;
                        saved.replay(g.c.Get(), root.extended.Get());
                        saved.replay(g.c.Get(), originalRoot.Get());
                        geometryFixturePacket = true;
                    }
                    g.c->DrawIndexedInstanced(6, 1, 0, 2, 7 + index);
                    geometryFixturePacket = false;
                }
                g.barrier(colors[pass].Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
                D3D12_TEXTURE_COPY_LOCATION src {}, dst {};
                src.pResource = colors[pass].Get();
                src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
                dst.pResource = readback.Get();
                dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
                dst.PlacedFootprint = footprint;
                dst.PlacedFootprint.Offset = pass * imageBytes;
                g.c->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
                g.barrier(colors[pass].Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
                if (mrt && pass < 2)
                {
                    g.barrier(auxiliary[pass].Get(), D3D12_RESOURCE_STATE_RENDER_TARGET,
                              D3D12_RESOURCE_STATE_COPY_SOURCE);
                    src.pResource = auxiliary[pass].Get();
                    dst.PlacedFootprint.Offset = auxiliaryOffset + pass * imageBytes;
                    g.c->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
                    g.barrier(auxiliary[pass].Get(), D3D12_RESOURCE_STATE_COPY_SOURCE,
                              D3D12_RESOURCE_STATE_RENDER_TARGET);
                }
            }
            g.barrier(stream.Get(), D3D12_RESOURCE_STATE_STREAM_OUT, D3D12_RESOURCE_STATE_COPY_SOURCE);
            g.c->CopyBufferRegion(readback.Get(), 5 * imageBytes, stream.Get(), 0, SOBytes);
            g.barrier(stream.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
            g.barrier(history[current].Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
            g.c->CopyBufferRegion(readback.Get(), 5 * imageBytes + SOBytes, history[current].Get(), 0, HistoryBytes);
            g.barrier(history[current].Get(), D3D12_RESOURCE_STATE_COPY_SOURCE,
                      D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            g.barrier(capture.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
            g.c->CopyBufferRegion(readback.Get(), 5 * imageBytes + SOBytes + HistoryBytes, capture.Get(), 0,
                                  CaptureBytes);
            g.barrier(capture.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            g.finish();
            if (recorder || captureModule)
            {
                ID3D12CommandList* submitted[] { g.c.Get() };
                GlassFg::NotifyGeometryCaptureSubmit(g.q.Get(), 1, submitted);
            }
            if (commands)
                require(!GlassFg::ReadGeometryBindings(g.c.Get()), "Closed recording remained readable");
            void* data;
            D3D12_RANGE readRange { 0, SIZE_T(readBytes) };
            check(readback->Map(0, &readRange, &data));
            const auto* bytes = static_cast<const char*>(data);
            require(!memcmp(bytes, bytes + imageBytes, size_t(imageBytes)),
                    "Per-instance capture changed original color/depth/discard");
            if (mrt)
            {
                require(!memcmp(bytes + auxiliaryOffset, bytes + auxiliaryOffset + imageBytes, size_t(imageBytes)),
                        "Capture changed the original auxiliary MRT");
                const float clear[] { .125f, .25f, .375f, .5f };
                for (UINT y = 0; y < H; ++y)
                    for (UINT x = 0; x < W; ++x)
                        auxiliaryWritten +=
                            memcmp(bytes + auxiliaryOffset + y * footprint.Footprint.RowPitch + x * sizeof(clear),
                                   clear, sizeof(clear)) != 0;
            }
            exact += W * H;
            if (recorder)
            {
                for (UINT i = 0; i < 3; ++i)
                    for (UINT y = 0; y < H; ++y)
                        for (UINT x = 0; x < W; ++x)
                        {
                            const auto* color = reinterpret_cast<const float*>(bytes + (order[7 + i] + 2) * imageBytes +
                                y * footprint.Footprint.RowPitch + x * 16);
                            expectedCoverage[((frame * 3 + i) * H + y) * W + x] = color[3] > 0;
                        }
                D3D12_RANGE noWrite { 0, 0 };
                readback->Unmap(0, &noWrite);
                Sleep(200); // Independent fixture gives the diagnostic worker time to compile.
                continue;
            }
            require(*reinterpret_cast<const UINT64*>(bytes + 5 * imageBytes) == 18 * sizeof(Clip),
                    "Original SO byte count");
            const auto* emitted = reinterpret_cast<const Clip*>(bytes + 5 * imageBytes + 256);
            const auto* hist = reinterpret_cast<const History*>(bytes + 5 * imageBytes + SOBytes);
            const auto* pixels =
                reinterpret_cast<const CaptureRecord*>(bytes + 5 * imageBytes + SOBytes + HistoryBytes);
            if (coverageOnly)
            {
                require(!memcmp(hist, zero.data(), HistoryBytes), "Coverage-only draw wrote vertex history");
                const auto* words = reinterpret_cast<const UINT*>(pixels);
                for (UINT i = 0; i < 3; ++i)
                {
                    const UINT object = order[7 + i];
                    const auto& entry = mapping[5 + i];
                    if (entry.inactive())
                        continue;
                    require(words[entry.statusIndex] == (frame == 4 && object == 1 ? 1u : 0u),
                            "Coverage escape flag mismatch");
                    for (UINT y = entry.top; y < entry.top + entry.height; ++y)
                        for (UINT x = entry.left; x < entry.left + entry.width; ++x)
                        {
                            const auto* color = reinterpret_cast<const float*>(
                                bytes + (object + 2) * imageBytes + y * footprint.Footprint.RowPitch + x * 16);
                            const UINT bit = entry.pixelBase + (y - entry.top) * entry.stride + x - entry.left;
                            const bool actual = (words[bit / 32] & (1u << (bit % 32))) != 0;
                            require(actual == (color[3] > 0), "Coverage bit differs from original object material");
                            checked += actual;
                        }
                }
                D3D12_RANGE noWrite { 0, 0 };
                readback->Unmap(0, &noWrite);
                continue;
            }
            std::array<Clip, 18> now {};
            for (UINT i = 0; i < 3; ++i)
                memcpy(now.data() + order[7 + i] * 6, emitted + i * 6, 6 * sizeof(Clip));
            for (UINT i = 0; i < 3; ++i)
            {
                const UINT object = order[7 + i];
                const auto& entry = mapping[5 + i];
                if (entry.inactive())
                {
                    const auto offset = historyBase[object] * 32;
                    require(!priorHistory[current].empty() && !memcmp(reinterpret_cast<const char*>(hist) + offset,
                                                                      priorHistory[current].data() + offset, 4 * 32),
                            "Inactive object wrote vertex history");
                    const auto status = 8 + object * Segment;
                    require(!priorCapture.empty() &&
                                !memcmp(reinterpret_cast<const char*>(pixels + status + 1),
                                        priorCapture.data() + (status + 1) * 32, (Segment - 1) * 32),
                            "Inactive object wrote captured pixels");
                    require(!memcmp(pixels, zero.data(), 8 * 32) && !memcmp(pixels + status, zero.data(), 32),
                            "Inactive object wrote a foreign or own status record");
                    continue;
                }
                for (UINT v = 0; v < 6; ++v)
                {
                    const auto& vertex = hist[entry.historyBase + indices[v]];
                    require(vertex.frame == frame && vertex.generation == entry.generation &&
                                !memcmp(vertex.clip, now[object * 6 + v].xyzw, 16),
                            "Rebatched object history mismatch");
                }
                UINT expectedFlags = frame == 1 || (frame == 3 && object != 1) ? GlassFg::GeometryMissingHistory : 0;
                if (frame == 4 && object == 1)
                    expectedFlags = GlassFg::GeometryEscapedBounds;
                if (frame == 7 && object == 1)
                    expectedFlags = GlassFg::GeometryMissingHistory;
                const UINT actualFlags = *reinterpret_cast<const UINT*>(&pixels[entry.statusIndex]);
                if (actualFlags != expectedFlags)
                    printf("FLAG frame=%u object=%u expected=%u actual=%u\n", frame, object, expectedFlags,
                           actualFlags);
                require(actualFlags == expectedFlags, "Object rejection flag mismatch");
                if (expectedFlags)
                    continue;
                for (UINT y = 0; y < H; ++y)
                    for (UINT x = 0; x < W; ++x)
                    {
                        const auto* originalPixel = reinterpret_cast<const float*>(
                            bytes + (object + 2) * imageBytes + y * footprint.Footprint.RowPitch + x * 16);
                        const bool covered = originalPixel[3] > 0;
                        const bool inRect = x >= entry.left && y >= entry.top && x < entry.left + entry.width &&
                                            y < entry.top + entry.height;
                        require(!covered || inRect, "Fixture unexpectedly escapes rectangle");
                        if (!inRect)
                            continue;
                        const auto& pixel = pixels[entry.pixelBase + (y - entry.top) * entry.stride + x - entry.left];
                        require((pixel.frame == frame) == covered,
                                "Individual original material contour/depth coverage mismatch");
                        if (!covered)
                            continue;
                        const auto mv = referenceMotion(now.data() + object * 6, last.data() + object * 6, x, y, W, H);
                        for (UINT c = 0; c < 2; ++c)
                        {
                            const double error = std::abs(pixel.motion[c] - mv[c]) * (c ? H : W);
                            maximum = std::max(maximum, error);
                            require(error < .004, "Rebatched object motion mismatch");
                        }
                        for (float t : pixel.transmission)
                            require(std::abs(t - (1 - originalPixel[3])) < 3e-7,
                                    "Cross-object transmission contamination");
                        require(std::abs(pixel.depth - (.4f + object * .03f)) < 1e-6, "Object depth contamination");
                        for (UINT other = object + 1; other < 3; ++other)
                        {
                            const auto* p = reinterpret_cast<const float*>(bytes + (other + 2) * imageBytes +
                                                                           y * footprint.Footprint.RowPitch + x * 16);
                            overlaps += p[3] > 0;
                        }
                        ++checked;
                        if (frame == 8 && object == 1)
                            ++recovered;
                    }
            }
            last = now;
            priorHistory[current].assign(reinterpret_cast<const char*>(hist),
                                         reinterpret_cast<const char*>(hist) + HistoryBytes);
            priorCapture.assign(reinterpret_cast<const char*>(pixels),
                                reinterpret_cast<const char*>(pixels) + CaptureBytes);
            D3D12_RANGE noWrite { 0, 0 };
            readback->Unmap(0, &noWrite);
        }
        if (recorder)
        {
            g.begin(); // Discard the last recording as well as waiting for execution.
            g.finish();
            if (moduleRecorder) moduleRecorderCheck.finish();
            std::vector<std::filesystem::path> captureDirectories { recorderOutput };
            captureDirectories.push_back(moduleRecorder ? secondModuleOutput : recorderOutput / "request-2");
            for (const auto& captureDirectory : captureDirectories)
            {
            const auto done = captureDirectory / "objects-0.done";
            const auto deadline = GetTickCount64() + 10000;
            while (!std::filesystem::exists(done) && GetTickCount64() < deadline)
                Sleep(10);
            require(std::filesystem::exists(done), "Diagnostic recorder did not complete");
            std::ifstream provenanceFile(captureDirectory / "objects-0.draw");
            const std::string provenance((std::istreambuf_iterator<char>(provenanceFile)), {});
            require(provenance.find("engine_mesh_shape=0\n") != std::string::npos &&
                        provenance.find("view_identity_proven=0\n") != std::string::npos &&
                        provenance.find("topology_history_proven=0\n") != std::string::npos &&
                        provenance.find("draw_indices=6\n") != std::string::npos &&
                        provenance.find("draw_instances=3\n") != std::string::npos &&
                        provenance.find("base_vertex=2\n") != std::string::npos &&
                        provenance.find("start_instance=7\n") != std::string::npos,
                    "Draw provenance lost or fixture incorrectly claims engine history");
            std::ifstream metadata(captureDirectory / "objects-0.csv");
            std::string line;
            std::getline(metadata, line);
            std::getline(metadata, line);
            const auto frame = std::stoul(line);
            require(frame >= 1 && frame <= 8, "Recorder frame not from fixture");
            if (moduleRecorder)
            {
                const auto bytes = read(captureDirectory / "objects-0.targets.bin");
                std::array<GlassExperimentTarget, 9> observedTargets {};
                require(bytes.size() == sizeof(observedTargets), "Target metadata size mismatch");
                memcpy(observedTargets.data(), bytes.data(), bytes.size());
                const auto& colorTarget = observedTargets[0];
                const auto& depthTarget = observedTargets[8];
                require(colorTarget.size == sizeof(colorTarget) && depthTarget.size == sizeof(depthTarget) &&
                            colorTarget.address == reinterpret_cast<uint64_t>(colors[1].Get()) &&
                            depthTarget.address == reinterpret_cast<uint64_t>(depth.Get()) &&
                            colorTarget.resource && depthTarget.resource && colorTarget.resource != depthTarget.resource &&
                            colorTarget.handle == rtvs[1].ptr && depthTarget.handle == dsv.ptr &&
                            colorTarget.defaultDescriptor && depthTarget.defaultDescriptor,
                        "Captured target bindings differ from actual draw resources");
            }
            require(captureDirectory == recorderOutput ? frame <= 4 : frame >= 5,
                    "Capture request reused an earlier session's recording");
            const auto data = read(captureDirectory / "objects-0.bin");
            constexpr UINT words = 1 + (W * H + 31) / 32;
            require(data.size() == 5 * words * 4, "Recorder allocation size");
            for (UINT i = 0; i < 3; ++i)
                for (UINT p = 0; p < W * H; ++p)
                {
                    UINT word;
                    memcpy(&word, data.data() + (i * words + 1 + p / 32) * 4, 4);
                    const unsigned expected = frame >= 5 && i == 1 ? 0u : expectedCoverage[(frame * 3 + i) * W * H + p];
                    require(((word >> (p % 32)) & 1) == expected,
                            "Recorded engine-mapped material coverage differs from original draw");
                }
            require(provenance.find("coverage_format=2\n") != std::string::npos &&
                        provenance.find("reference_same_draw=1\n") != std::string::npos &&
                        provenance.find("reference_object_mapping=0\n") != std::string::npos,
                    "Missing same-draw independent reference metadata");
            require(provenance.find("recording_epoch=0\n") == std::string::npos &&
                        provenance.find("recording_epoch=") != std::string::npos &&
                        read(captureDirectory / "objects-0.vs.dxil") == vs &&
                        read(captureDirectory / "objects-0.ps.dxil") == ps,
                    "Original same-draw shader/recording provenance lost");
            unsigned missingMapped = 0;
            for (UINT p = 0; p < W * H; ++p)
            {
                bool originalCovered = false, mapped = false;
                for (UINT i = 0; i < 3; ++i)
                {
                    originalCovered |= expectedCoverage[(frame * 3 + i) * W * H + p] != 0;
                    const auto word = reinterpret_cast<const UINT*>(data.data())[i * words + 1 + p / 32];
                    mapped |= ((word >> (p % 32)) & 1) != 0;
                }
                for (UINT reference = 3; reference < 5; ++reference)
                {
                    const auto word = reinterpret_cast<const UINT*>(data.data())[reference * words + 1 + p / 32];
                    require((((word >> (p % 32)) & 1) != 0) == originalCovered,
                            "Same-draw reference lost original material pixels");
                }
                missingMapped += originalCovered && !mapped;
            }
            require(frame >= 5 ? missingMapped > 20 : missingMapped == 0,
                    "Reference did not distinguish deliberately missing object mapping");
            }
            if (moduleRecorder)
            {
                if (controlledRecorder) printf("PASS event_control=1 submission_admission=1 duplicate_load_rollback=1 asynchronous_retirement=1 same_process_replacement=1 game_hooks=0\n");
                printf("PASS independent_module_recorder=1 worker_prepared_resources=1 saved_original_samples=%u "
                       "original_pixels=%llu same_draw_reference=1 module_generations=2 actual_unload=2 "
                       "missing_mapping_detected=1 motion_produced=0 game_hooks=0\n", 6 * W * H, exact);
                return 0;
            }
            printf("PASS recorder_worker=1 completion_and_discard=1 requests_same_process=2 original_material_samples=%u original_pixels=%llu "
                   "same_draw_reference=1 missing_mapping_detected=1 motion_produced=0 game_hooks=0\n", 2 * 3 * W * H, exact);
            return 0;
        }
        if (coverageOnly)
        {
            require(checked > 1000, "Insufficient coverage samples");
            printf("PASS object_bit_coverage=1 original_material_discard=1 original_pixels=%llu covered_samples=%llu "
                   "vertex_history_writes=0 motion_produced=0 game_hooks=0\n", exact, checked);
            return 0;
        }
        require(checked > 1000 && overlaps > 100, "Insufficient per-object overlap coverage");
        require(recovered > 100, "Inactive object did not recover after fresh history");
        if (mrt)
        {
            require(dual ? auxiliaryWritten == 0 : auxiliaryWritten > 1000,
                    "Auxiliary MRT coverage or dual-source untouched target changed");
            printf("PASS mrt_slots=3 dual_source=%u auxiliary_equal_pixels=%llu auxiliary_written=%llu\n", dual, exact,
                   auxiliaryWritten);
        }
        if (commands)
        {
            const auto stats = GlassFg::GetGeometryCommandStats();
            require(stats.active && !stats.capacityRejected && stats.indexed == 40 &&
                        stats.packets == (captureCommand ? 32u : 24u) &&
                        stats.identities == (captureCommand ? 48u : 24u) &&
                        stats.pipelinesReady == (captureCommand ? 32u : 24u) &&
                        stats.bindingsReady == (captureCommand ? 32u : 24u),
                    "Actual public draw/packet/pipeline observation mismatch");
            if (captureCommand && !captureModule)
                require(captureOwner.completed == 8 && captureOwner.rasterMatched == 8 &&
                            stats.captureRecorded == 8 && !stats.captureRejected,
                        "Capture command not recorded exactly once");
            printf("PASS public_command_observer=1 restored_root_pixels=%llu indexed=%llu packets=%llu "
                   "bindings_ready=%llu game_hooks=0\n",
                   exact, stats.indexed, stats.packets, stats.bindingsReady);
        }
        if (experiment)
        {
            experimentCheck.beforeFinalDiscard();
            g.begin(); // Discard the final module-PSO recording before unloading.
            g.finish();
            experimentCheck.verify();
        }
        printf("PASS compiler_worker=1 retained_pipeline_lease=1 observed_creation=%u instance_rebatch=1 "
               "isolated_contours=1 inactive_instances=1 inactive_recovery=1 opaque_depth_rejection=1 "
               "partial_history_flag=1 "
               "generation_flag=1 escaped_bounds_flag=1 original_pixels=%llu motion_pixels=%llu "
               "overlapping_samples=%llu max_motion_error_px=%.9f\n",
               observed, exact, checked, overlaps, maximum);
        return 0;
    }
    catch (const std::exception& e)
    {
        fprintf(stderr, "FAIL %s\n", e.what());
        return 1;
    }
}
