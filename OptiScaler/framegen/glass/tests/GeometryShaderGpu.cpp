#include "GeometryTestDevice.h"
static void checkRoots(ID3D12Device* device)
{
    std::array<D3D12_ROOT_PARAMETER1, 36> parameters {};
    std::array<D3D12_DESCRIPTOR_RANGE1, 36> ranges {};
    for (UINT i = 0; i < parameters.size(); ++i)
    {
        ranges[i] = { D3D12_DESCRIPTOR_RANGE_TYPE_SRV,
                      i == 35 ? UINT_MAX : 4u,
                      0,
                      i + 40,
                      D3D12_DESCRIPTOR_RANGE_FLAG_DESCRIPTORS_VOLATILE | D3D12_DESCRIPTOR_RANGE_FLAG_DATA_VOLATILE,
                      D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND };
        parameters[i].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        parameters[i].DescriptorTable = { 1, &ranges[i] };
        parameters[i].ShaderVisibility = i < 18 ? D3D12_SHADER_VISIBILITY_VERTEX : D3D12_SHADER_VISIBILITY_PIXEL;
    }
    D3D12_VERSIONED_ROOT_SIGNATURE_DESC desc {};
    desc.Version = D3D_ROOT_SIGNATURE_VERSION_1_1;
    desc.Desc_1_1 = { 36, parameters.data(), 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT };
    auto create = [&](const D3D12_VERSIONED_ROOT_SIGNATURE_DESC& definition, bool expected)
    {
        ComPtr<ID3DBlob> blob, errors;
        check(D3D12SerializeVersionedRootSignature(&definition, &blob, &errors));
        ComPtr<ID3D12RootSignature> original;
        check(device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&original)));
        GlassFg::GeometryRoot root;
        std::string error;
        auto hr = GlassFg::CreateGeometryRoot(device, original.Get(), 0, blob->GetBufferPointer(),
                                              blob->GetBufferSize(), root, error);
        require(SUCCEEDED(hr) == expected, "Root admission mismatch");
        if (!expected)
        {
            require(!root.extended && !error.empty(), "Failed root creation changed output");
            return;
        }
        require(root.dwords == 52 && root.originalParameters.size() == 36 && root.captureSlot == 40,
                "36-table root did not fit in 52 DWORDs");
        for (UINT i = 0; i < parameters.size(); ++i)
        {
            const auto& a = root.originalParameters[i];
            require(a.ParameterType == parameters[i].ParameterType &&
                        a.ShaderVisibility == parameters[i].ShaderVisibility &&
                        a.DescriptorTable.NumDescriptorRanges == 1 &&
                        !memcmp(a.DescriptorTable.pDescriptorRanges, &ranges[i], sizeof(ranges[i])),
                    "Original table changed");
        }
    };
    create(desc, true);
    ranges[0].RegisterSpace = 31;
    create(desc, false);
    ranges[0].RegisterSpace = 40;
    D3D12_ROOT_PARAMETER1 large {};
    large.ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    large.Constants = { 0, 0, 49 };
    desc.Desc_1_1.NumParameters = 1;
    desc.Desc_1_1.pParameters = &large;
    create(desc, false);
    puts("PASS root_tables=36 extended_dwords=52 original_ranges_preserved=1 collision_and_budget_rejected=1");
}
static void checkCameraBounds()
{
    std::array<std::byte, 848> bytes {};
    const std::int32_t origin[] = { 100 * 131072, -20 * 131072, 3 * 131072 };
    const float matrix[] = { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, .2f, 0, 0, 1, 0 };
    const float extent[] = { 2560, 1440 }, jitter[] = { .000244140625f, .000303819455f };
    memcpy(bytes.data() + 28 * 16, matrix, sizeof(matrix));
    memcpy(bytes.data() + 38 * 16, origin, sizeof(origin));
    memcpy(bytes.data() + 47 * 16, extent, sizeof(extent));
    memcpy(bytes.data() + 51 * 16, jitter, sizeof(jitter));
    GlassFg::GeometryCamera camera;
    require(GlassFg::DecodeCyberpunkCamera(bytes, camera), "Camera decoding rejected");
    require(camera.fixedOrigin[1] == -20 * 131072 && camera.jitterUv[0] * 2560 == .3125f &&
                std::abs(camera.jitterUv[1] * 1440 + .21875f) < 1.e-7f,
            "Camera fixed origin or jitter convention");
    D3D12_VIEWPORT viewport { 32, 16, 320, 180, 0, 1 };
    D3D12_RECT rect {};
    const std::array<float, 6> bounds { 99, -21, 7, 101, -19, 9 };
    require(GlassFg::ProjectGeometryBounds(camera, bounds, viewport, 2, rect) == GlassFg::BoundProjection::Visible &&
                rect.left == 150 && rect.top == 81 && rect.right == 234 && rect.bottom == 131,
            "Analytic perspective box/offset viewport bounds");
    require(GlassFg::ProjectGeometryBounds(camera, { 99, -21, 2, 101, -19, 4 }, viewport, 2, rect) ==
                    GlassFg::BoundProjection::Visible &&
                rect.left == 32 && rect.top == 16 && rect.right == 352 && rect.bottom == 196,
            "Eye-plane bounds truncated");
    require(GlassFg::ProjectGeometryBounds(camera, { 99, -21, 0, 101, -19, 1 }, viewport, 2, rect) ==
                GlassFg::BoundProjection::Outside,
            "Behind-camera bounds admitted");
    camera.fixedOrigin[0] += 20 * 131072;
    require(GlassFg::ProjectGeometryBounds(camera, bounds, viewport, 2, rect) == GlassFg::BoundProjection::Outside,
            "Offscreen bounds admitted");
    require(!GlassFg::DecodeCyberpunkCamera(std::span(bytes).first(847), camera), "Incomplete camera input admitted");
    const float invalid = std::numeric_limits<float>::quiet_NaN();
    memcpy(bytes.data() + 28 * 16, &invalid, 4);
    require(!GlassFg::DecodeCyberpunkCamera(bytes, camera), "Nonfinite camera admitted");
    puts("PASS camera_fixed_origin=1 jitter_uv=1 perspective_bounds=1 shifted_viewport=1 eye_plane_fallback=1 "
         "invalid_rejected=1");
}
int wmain(int argc, wchar_t** argv)
{
    try
    {
        require(argc == 3, "gpu directory dxcompiler.dll");
        const std::filesystem::path dir(argv[1]);
        checkCameraBounds();
        auto vs = read(dir / L"fixture.dxil"), modified = read(dir / L"fixture-history.dxil"),
             ps = read(dir / L"fixture-pixel.dxil"), motion = read(dir / L"fixture-motion.dxil"),
             captureShader = read(dir / L"fixture-capture.dxil");
        Device g;
        checkRoots(g.d.Get());
        D3D12_FEATURE_DATA_D3D12_OPTIONS options {};
        check(g.d->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS, &options, sizeof(options)));
        require(options.ROVsSupported, "Rasterizer-ordered views unavailable");
        D3D12_ROOT_PARAMETER params[1] {};
        params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        params[0].Constants = { 0, 0, 4 };
        params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;
        D3D12_ROOT_SIGNATURE_DESC rd { 1, params, 0, nullptr,
                                       D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT |
                                           D3D12_ROOT_SIGNATURE_FLAG_ALLOW_STREAM_OUTPUT };
        ComPtr<ID3DBlob> rb, error;
        check(D3D12SerializeRootSignature(&rd, D3D_ROOT_SIGNATURE_VERSION_1, &rb, &error));
        ComPtr<ID3D12RootSignature> originalRoot;
        check(g.d->CreateRootSignature(0, rb->GetBufferPointer(), rb->GetBufferSize(), IID_PPV_ARGS(&originalRoot)));
        GlassFg::GeometryRoot geometryRoot;
        std::string pipelineError;
        check(GlassFg::CreateGeometryRoot(g.d.Get(), originalRoot.Get(), 0, rb->GetBufferPointer(), rb->GetBufferSize(),
                                          geometryRoot, pipelineError));
        require(geometryRoot.dwords == 20 && geometryRoot.materialSlot == 4, "Extended root layout");
        auto root = geometryRoot.extended;
        GlassFg::GeometryCompiler compiler { std::filesystem::path(argv[2]) };
        constexpr UINT W = 192, H = 128;
        D3D12_RESOURCE_DESC tex {};
        tex.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        tex.Width = W;
        tex.Height = H;
        tex.DepthOrArraySize = 1;
        tex.MipLevels = 1;
        tex.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
        tex.SampleDesc.Count = 1;
        tex.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
        D3D12_HEAP_PROPERTIES hp {};
        hp.Type = D3D12_HEAP_TYPE_DEFAULT;
        ComPtr<ID3D12Resource> color[4];
        D3D12_CLEAR_VALUE clear {};
        clear.Format = tex.Format;
        for (auto& r : color)
            check(g.d->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &tex, D3D12_RESOURCE_STATE_RENDER_TARGET,
                                               &clear, IID_PPV_ARGS(&r)));
        D3D12_DESCRIPTOR_HEAP_DESC hd {};
        hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
        hd.NumDescriptors = 4;
        ComPtr<ID3D12DescriptorHeap> rtvs;
        check(g.d->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&rtvs)));
        D3D12_CPU_DESCRIPTOR_HANDLE handles[4];
        for (UINT i = 0; i < 4; ++i)
        {
            handles[i] = rtvs->GetCPUDescriptorHandleForHeapStart();
            handles[i].ptr += i * g.d->GetDescriptorHandleIncrementSize(hd.Type);
            g.d->CreateRenderTargetView(color[i].Get(), nullptr, handles[i]);
        }
        const D3D12_INPUT_ELEMENT_DESC layout[] = {
            { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
            { "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 }
        };
        const D3D12_SO_DECLARATION_ENTRY so[] = { { 0, "SV_Position", 0, 0, 4, 0 },
                                                  { 0, "GLASS_PREVIOUS", 0, 0, 4, 0 },
                                                  { 0, "GLASS_HISTORY_MISSING", 0, 0, 1, 0 } };
        ComPtr<ID3D12PipelineState> pipeline[4];
        ComPtr<ID3D12PipelineState> originalColor;
        for (UINT i = 0; i < 4; ++i)
        {
            D3D12_GRAPHICS_PIPELINE_STATE_DESC pd {};
            pd.pRootSignature = root.Get();
            pd.VS = { i ? modified.data() : vs.data(), i ? modified.size() : vs.size() };
            pd.PS = { i == 3   ? captureShader.data()
                      : i == 2 ? motion.data()
                               : ps.data(),
                      i == 3   ? captureShader.size()
                      : i == 2 ? motion.size()
                               : ps.size() };
            pd.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
            if (i != 2)
            {
                auto& blend = pd.BlendState.RenderTarget[0];
                blend.BlendEnable = TRUE;
                blend.SrcBlend = D3D12_BLEND_ONE;
                blend.DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
                blend.BlendOp = D3D12_BLEND_OP_ADD;
                blend.SrcBlendAlpha = D3D12_BLEND_ONE;
                blend.DestBlendAlpha = D3D12_BLEND_ZERO;
                blend.BlendOpAlpha = D3D12_BLEND_OP_ADD;
            }
            pd.SampleMask = UINT_MAX;
            pd.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
            pd.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
            pd.RasterizerState.DepthClipEnable = TRUE;
            pd.InputLayout = { layout, 2 };
            pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
            pd.NumRenderTargets = 1;
            pd.RTVFormats[0] = tex.Format;
            pd.SampleDesc.Count = 1;
            UINT stride = i ? sizeof(Record) : sizeof(Clip);
            if (i < 2)
                pd.StreamOutput = { so, i ? 3u : 1u, &stride, 1, 0 };
            if (i == 3)
            {
                pd.pRootSignature = originalRoot.Get();
                pd.VS = { vs.data(), vs.size() };
                pd.PS = { ps.data(), ps.size() };
                check(g.d->CreateGraphicsPipelineState(&pd, IID_PPV_ARGS(&originalColor)));
                auto hr = compiler.create(g.d.Get(), geometryRoot, pd, pipeline[i], pipelineError);
                if (FAILED(hr))
                    throw std::runtime_error(pipelineError);
                auto rejected = pd;
                rejected.DepthStencilState.DepthEnable = TRUE;
                rejected.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
                ComPtr<ID3D12PipelineState> absent;
                require(FAILED(compiler.create(g.d.Get(), geometryRoot, rejected, absent, pipelineError)) && !absent,
                        "Depth-writing pipeline admitted");
            }
            else
                check(g.d->CreateGraphicsPipelineState(&pd, IID_PPV_ARGS(&pipeline[i])));
        }
        const Vertex verts[] = { { 0, 0, 0, 0, 0 },       { 0, 0, 0, 0, 0 },      { -.35f, -.5f, 0, 0, 1 },
                                 { -.35f, .5f, 0, 0, 0 }, { .35f, .5f, 0, 1, 0 }, { .35f, -.5f, 0, 1, 1 } };
        const uint16_t indices[] = { 0, 1, 2, 0, 2, 3 };
        auto vb = g.buffer(sizeof(verts), D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
        upload(vb.Get(), verts, sizeof(verts));
        auto ib = g.buffer(sizeof(indices), D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
        upload(ib.Get(), indices, sizeof(indices));
        D3D12_VERTEX_BUFFER_VIEW vv { vb->GetGPUVirtualAddress(), sizeof(verts), sizeof(Vertex) };
        D3D12_INDEX_BUFFER_VIEW iv { ib->GetGPUVirtualAddress(), sizeof(indices), DXGI_FORMAT_R16_UINT };
        constexpr UINT Slots = 64, HistoryBytes = Slots * sizeof(History), SOBytes = 4096;
        std::array<char, SOBytes> zero {};
        constexpr UINT RoiLeft = 16, RoiTop = 12, RoiWidth = W - 32, RoiHeight = H - 24, RoiStride = RoiWidth + 8,
                       RoiBase = 17;
        constexpr UINT CapturePixels = RoiBase + RoiStride * RoiHeight + 17, CaptureBytes = CapturePixels * 32;
        auto capture = g.buffer(CaptureBytes, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_COPY_DEST,
                                D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
        auto materialConstants = g.buffer(256, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
        auto zeros =
            g.buffer(HistoryBytes + SOBytes + CaptureBytes, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
        std::vector<char> allzero(HistoryBytes + SOBytes + CaptureBytes);
        upload(zeros.Get(), allzero.data(), allzero.size());
        ComPtr<ID3D12Resource> history[2], stream[2];
        for (auto& r : history)
            r = g.buffer(HistoryBytes, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_COPY_DEST,
                         D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
        for (auto& r : stream)
            r = g.buffer(SOBytes, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_COPY_DEST);
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint {};
        UINT64 imageBytes;
        g.d->GetCopyableFootprints(&tex, 0, 1, 0, &footprint, nullptr, nullptr, &imageBytes);
        auto readback = g.buffer(imageBytes * 4 + SOBytes * 2 + HistoryBytes + CaptureBytes, D3D12_HEAP_TYPE_READBACK,
                                 D3D12_RESOURCE_STATE_COPY_DEST);
        g.begin();
        for (auto& r : history)
        {
            g.c->CopyBufferRegion(r.Get(), 0, zeros.Get(), 0, HistoryBytes);
            g.barrier(r.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        }
        g.c->CopyBufferRegion(capture.Get(), 0, zeros.Get(), 0, CaptureBytes);
        g.barrier(capture.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        g.finish();
        std::vector<Clip> last(18);
        uint64_t exactColors = 0, exactHistory = 0, validSamples = 0, motionPixels = 0, capturePixels = 0;
        double maxMotionError = 0;
        for (UINT frame = 1; frame <= 5; ++frame)
        {
            UINT current = frame & 1, previous = current ^ 1;
            UINT generation = frame >= 3 ? 99 : 42;
            UINT expected = frame == 5 ? 2 : frame - 1;
            const bool shouldValid = frame == 2 || frame == 4;
            float fc[] = { frame * .31f, frame * .023f, frame * -.017f, 0 };
            GlassFg::VertexHistoryConstants hc { 8, 4, 0, 0, 3, generation, frame, expected };
            require(hc.valid(Slots), "History allocation contract");
            auto invalid = hc;
            invalid.base = UINT32_MAX;
            require(!invalid.valid(Slots), "History address overflow admitted");
            invalid = hc;
            invalid.instances = UINT32_MAX;
            require(!invalid.valid(Slots), "History instance overflow admitted");
            invalid = hc;
            invalid.generation = 0;
            require(!invalid.valid(Slots), "Uninitialized history generation admitted");
            require(!hc.valid(0), "Empty history buffer admitted");
            GlassFg::MaterialCaptureConstants pc { 0,       0,         1.f / W,       1.f / H, 0,        0,
                                                   frame,   0,         RoiLeft,       RoiTop,  RoiWidth, RoiHeight,
                                                   RoiBase, RoiStride, CapturePixels, 0 };
            require(pc.valid(CapturePixels), "Capture allocation contract");
            require(!pc.valid(CapturePixels - 1), "Capture buffer smaller than advertised capacity admitted");
            upload(materialConstants.Get(), &pc, sizeof(pc));
            g.begin();
            g.barrier(history[current].Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                      D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            for (UINT i = 0; i < 4; ++i)
            {
                if (i < 2)
                {
                    g.c->CopyBufferRegion(stream[i].Get(), 0, zeros.Get(), 0, SOBytes);
                    g.barrier(stream[i].Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_STREAM_OUT);
                }
                GlassFg::GraphicsRootBindings saved;
                if (i == 3)
                {
                    saved.reset(originalColor.Get());
                    saved.setRoot(originalRoot.Get());
                    g.c->SetGraphicsRootSignature(originalRoot.Get());
                    g.c->SetPipelineState(originalColor.Get());
                    saved.constants(0, 2, fc, 0);
                    saved.constants(0, 2, fc + 2, 2);
                    g.c->SetGraphicsRoot32BitConstants(0, 2, fc, 0);
                    g.c->SetGraphicsRoot32BitConstants(0, 2, fc + 2, 2);
                    require(saved.canReplay(geometryRoot, originalColor.Get()), "Tracked root not restorable");
                    auto invalidState = saved;
                    invalidState.invalidate();
                    require(!invalidState.canReplay(geometryRoot, originalColor.Get()),
                            "Unknown command state admitted");
                    saved.replay(g.c.Get(), root.Get());
                }
                else
                    g.c->SetGraphicsRootSignature(root.Get());
                g.c->SetPipelineState(pipeline[i].Get());
                g.c->SetGraphicsRoot32BitConstants(0, 4, fc, 0);
                g.c->SetGraphicsRoot32BitConstants(1, 8, &hc, 0);
                g.c->SetGraphicsRootShaderResourceView(2, history[previous]->GetGPUVirtualAddress());
                g.c->SetGraphicsRootUnorderedAccessView(3, history[current]->GetGPUVirtualAddress());
                g.c->SetGraphicsRootConstantBufferView(4, materialConstants->GetGPUVirtualAddress());
                g.c->SetGraphicsRootUnorderedAccessView(5, capture->GetGPUVirtualAddress());
                D3D12_VIEWPORT viewport { 0, 0, (float) W, (float) H, 0, 1 };
                D3D12_RECT scissor { 0, 0, W, H };
                g.c->RSSetViewports(1, &viewport);
                g.c->RSSetScissorRects(1, &scissor);
                g.c->OMSetRenderTargets(1, &handles[i], FALSE, nullptr);
                const float cv[4] = { 0, 0, i == 2 ? -1.f : 0.f, 0 };
                g.c->ClearRenderTargetView(handles[i], cv, 0, nullptr);
                g.c->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
                g.c->IASetVertexBuffers(0, 1, &vv);
                g.c->IASetIndexBuffer(&iv);
                if (i < 2)
                {
                    D3D12_STREAM_OUTPUT_BUFFER_VIEW view { stream[i]->GetGPUVirtualAddress() + 256, SOBytes - 256,
                                                           stream[i]->GetGPUVirtualAddress() };
                    g.c->SOSetTargets(0, 1, &view);
                }
                g.c->DrawIndexedInstanced(6, 3, 0, 2, 7);
                D3D12_STREAM_OUTPUT_BUFFER_VIEW empty {};
                g.c->SOSetTargets(0, 1, &empty);
                if (i < 2)
                {
                    g.barrier(stream[i].Get(), D3D12_RESOURCE_STATE_STREAM_OUT, D3D12_RESOURCE_STATE_COPY_SOURCE);
                    g.c->CopyBufferRegion(readback.Get(), imageBytes * 4 + i * SOBytes, stream[i].Get(), 0, SOBytes);
                    g.barrier(stream[i].Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
                }
                g.barrier(color[i].Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
                D3D12_TEXTURE_COPY_LOCATION src {};
                src.pResource = color[i].Get();
                src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
                D3D12_TEXTURE_COPY_LOCATION dst {};
                dst.pResource = readback.Get();
                dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
                dst.PlacedFootprint = footprint;
                dst.PlacedFootprint.Offset = i * imageBytes;
                g.c->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
                g.barrier(color[i].Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
                if (i == 3)
                {
                    saved.replay(g.c.Get(), originalRoot.Get());
                    g.c->SetPipelineState(saved.pipeline);
                    g.c->OMSetRenderTargets(1, &handles[0], FALSE, nullptr);
                    const float emptyColor[4] {};
                    g.c->ClearRenderTargetView(handles[0], emptyColor, 0, nullptr);
                    // No root setter here: use the restored original constants.
                    g.c->DrawIndexedInstanced(6, 3, 0, 2, 7);
                    g.barrier(color[0].Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
                    src.pResource = color[0].Get();
                    dst.PlacedFootprint.Offset = 0;
                    g.c->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
                    g.barrier(color[0].Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
                }
            }
            g.barrier(history[current].Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
            g.c->CopyBufferRegion(readback.Get(), imageBytes * 4 + SOBytes * 2, history[current].Get(), 0,
                                  HistoryBytes);
            g.barrier(history[current].Get(), D3D12_RESOURCE_STATE_COPY_SOURCE,
                      D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            g.barrier(capture.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
            g.c->CopyBufferRegion(readback.Get(), imageBytes * 4 + SOBytes * 2 + HistoryBytes, capture.Get(), 0,
                                  CaptureBytes);
            g.barrier(capture.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            g.finish();
            void* m;
            D3D12_RANGE range { 0, (SIZE_T) (imageBytes * 4 + SOBytes * 2 + HistoryBytes + CaptureBytes) };
            check(readback->Map(0, &range, &m));
            auto* b = (unsigned char*) m;
            require(memcmp(b, b + imageBytes, (size_t) imageBytes) == 0, "Original pixel output changed");
            require(memcmp(b, b + imageBytes * 3, (size_t) imageBytes) == 0,
                    "Original color/discard changed with simultaneous capture");
            exactColors += W * H;
            auto* base = b + imageBytes * 4;
            require(*(uint64_t*) base == 18 * sizeof(Clip), "Reference SO length");
            require(*(uint64_t*) (base + SOBytes) == 18 * sizeof(Record), "History SO length");
            const auto* ref = (const Clip*) (base + 256);
            const auto* out = (const Record*) (base + SOBytes + 256);
            const auto* hist = (const History*) (base + SOBytes * 2);
            for (UINT j = 0; j < 18; ++j)
            {
                require(memcmp(ref[j].xyzw, out[j].current, 16) == 0, "Original vertex position changed");
                require(out[j].valid == (shouldValid ? 0.f : 1.f), "Stale/generation history admission mismatch");
                if (shouldValid)
                {
                    require(memcmp(last[j].xyzw, out[j].previous, 16) == 0, "Previous actual vertex mismatch");
                    ++validSamples;
                }
                UINT object = j / 6, vertex = indices[j % 6];
                const auto& h = hist[8 + object * 4 + vertex];
                require(h.frame == frame && h.generation == generation, "History tag mismatch");
                require(memcmp(h.clip, ref[j].xyzw, 16) == 0, "Captured current position mismatch");
                ++exactHistory;
            }
            for (UINT j = 0; j < Slots; ++j)
                if (j < 8 || j >= 20)
                {
                    const History empty {};
                    require(memcmp(&hist[j], &empty, sizeof(History)) == 0, "History wrote outside admitted range");
                }
            const auto* captured = (const CaptureRecord*) (b + imageBytes * 4 + SOBytes * 2 + HistoryBytes);
            for (UINT j = 0; j < CapturePixels; ++j)
            {
                const bool inside =
                    j >= RoiBase && j < RoiBase + RoiStride * RoiHeight && (j - RoiBase) % RoiStride < RoiWidth;
                if (!inside)
                {
                    const CaptureRecord empty {};
                    require(memcmp(&captured[j], &empty, sizeof(empty)) == 0, "Capture wrote outside owned region");
                }
            }
            for (UINT y = 0; y < H; ++y)
                for (UINT x = 0; x < W; ++x)
                {
                    const auto* original = (const float*) (b + y * footprint.Footprint.RowPitch + x * 16);
                    const auto* mv = (const float*) (b + imageBytes * 2 + y * footprint.Footprint.RowPitch + x * 16);
                    const bool covered = original[3] > 0;
                    if (x >= RoiLeft && y >= RoiTop && x < RoiLeft + RoiWidth && y < RoiTop + RoiHeight)
                    {
                        const auto& record = captured[RoiBase + (y - RoiTop) * RoiStride + x - RoiLeft];
                        require((record.frame == frame) == (covered && shouldValid),
                                "Simultaneous capture coverage mismatch");
                        if (record.frame == frame)
                        {
                            require(record.motion[0] == mv[0] && record.motion[1] == mv[1] && record.depth == mv[3],
                                    "Simultaneous and separate geometry motion differ");
                            for (float transmission : record.transmission)
                                require(std::abs(transmission - (1 - original[3])) < 3e-7f,
                                        "Simultaneous transmission mismatch");
                            ++capturePixels;
                        }
                    }
                    if ((mv[2] >= 0) != (covered && shouldValid))
                        printf("OUTLINE frame=%u x=%u y=%u expected=%u original=%g,%g,%g,%g motion=%g,%g,%g,%g\n",
                               frame, x, y, covered && shouldValid, original[0], original[1], original[2], original[3],
                               mv[0], mv[1], mv[2], mv[3]);
                    require((mv[2] >= 0) == (covered && shouldValid),
                            "Material alpha/outline or history rejection changed");
                    if (!covered || !shouldValid)
                        continue;
                    require(std::isfinite(mv[0]) && std::isfinite(mv[1]), "Nonfinite motion");
                    require(std::abs(mv[2] - original[3]) < 3e-7f, "Material attenuation mismatch");
                    require(std::abs(mv[3] - .4f) < 1e-6f, "Surface depth mismatch");
                    const int object = (int) std::lround(original[0] / original[1] * 2.5 - 1);
                    require(object >= 0 && object < 3, "Reference object identity");
                    double best = -1e30, weights[3] {}, selectedW[3] {};
                    unsigned selected = 0;
                    for (unsigned t = 0; t < 2; ++t)
                    {
                        unsigned j = object * 6 + t * 3;
                        double sx[3], sy[3], cw[3];
                        for (unsigned k = 0; k < 3; ++k)
                        {
                            const auto& v = ref[j + k].xyzw;
                            cw[k] = v[3];
                            sx[k] = (v[0] / cw[k] * .5 + .5) * W;
                            sy[k] = (-v[1] / cw[k] * .5 + .5) * H;
                        }
                        double dx1 = sx[1] - sx[0], dy1 = sy[1] - sy[0], dx2 = sx[2] - sx[0], dy2 = sy[2] - sy[0],
                               bx = x + .5 - sx[0], by = y + .5 - sy[0], den = dx1 * dy2 - dy1 * dx2;
                        double w[3];
                        w[1] = (bx * dy2 - by * dx2) / den;
                        w[2] = (dx1 * by - dy1 * bx) / den;
                        w[0] = 1 - w[1] - w[2];
                        double score = std::min(w[0], std::min(w[1], w[2]));
                        if (score > best)
                        {
                            best = score;
                            selected = j;
                            memcpy(weights, w, sizeof(w));
                            memcpy(selectedW, cw, sizeof(cw));
                        }
                    }
                    require(best > -.001, "Reference pixel outside geometry");
                    double previousClip[4] {}, den = 0;
                    for (unsigned k = 0; k < 3; ++k)
                    {
                        double w = weights[k] / selectedW[k];
                        den += w;
                        for (unsigned c = 0; c < 4; ++c)
                            previousClip[c] += w * last[selected + k].xyzw[c];
                    }
                    for (double& v : previousClip)
                        v /= den;
                    const double expectedMv[] = { previousClip[0] / previousClip[3] * .5 + .5 - (x + .5) / W,
                                                  -previousClip[1] / previousClip[3] * .5 + .5 - (y + .5) / H };
                    for (unsigned c = 0; c < 2; ++c)
                    {
                        double motionError = std::abs(mv[c] - expectedMv[c]) * (c ? H : W);
                        maxMotionError = std::max(maxMotionError, motionError);
                        require(motionError < .004, "Perspective vertex motion mismatch");
                    }
                    ++motionPixels;
                }
            memcpy(last.data(), ref, 18 * sizeof(Clip));
            D3D12_RANGE noWrite { 0, 0 };
            readback->Unmap(0, &noWrite);
        }
        require(capturePixels > 0, "No simultaneous capture pixels");
        printf("PASS original_pixels=%llu exact_current_vertices=%llu exact_previous_vertices=%llu motion_pixels=%llu "
               "capture_pixels=%llu "
               "max_motion_error_px=%.9f frames=5 generation_and_stale_rejection=1 outside_range_unchanged=1 "
               "original_material_outline=1 simultaneous_color_capture=1\n",
               exactColors, exactHistory, validSamples, motionPixels, capturePixels, maxMotionError);
        return 0;
    }
    catch (const std::exception& e)
    {
        fprintf(stderr, "FAIL %s\n", e.what());
        return 1;
    }
}
