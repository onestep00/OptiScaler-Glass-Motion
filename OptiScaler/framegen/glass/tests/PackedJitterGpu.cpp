// A1 capture-pair discriminating check. Runs the production compiler
// (GeometryCompiler::createPackedMotion with the audited b1/space0 constant
// pair) on an independent device and asserts that the packed motion of the
// second frame is the frame-to-frame row 51 delta alone.
//
// Both endpoints keep the same clip position, so the reconstructed raw motion
// is zero and every packed value belongs to the pair term. The two runs differ
// only in the captured words (or in the pair being absent), which is what makes
// this a discriminator rather than a smoke test.
#include "GeometryTestDevice.h"
#include <limits>

int wmain(int argc, wchar_t** argv)
{
    try
    {
        require(argc == 3, "Expected fixture binary directory and dxcompiler.dll");
        const std::filesystem::path dir(argv[1]);
        constexpr UINT W = 64, H = 32, Pixels = W * H, OriginalBytes = (Pixels + 1) * 4;
        Device g;
        // Original root: the fixture material's own UAV at u3 plus the audited
        // camera block at b1/space0 that the pair capture resolves.
        D3D12_ROOT_PARAMETER parameters[2] {};
        parameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
        parameters[0].Descriptor = {3, 0};
        parameters[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
        parameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
        parameters[1].Descriptor = {1, 0};
        parameters[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        D3D12_ROOT_SIGNATURE_DESC rd {2, parameters, 0, nullptr,
                                      D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT};
        ComPtr<ID3DBlob> bytes, errors;
        check(D3D12SerializeRootSignature(&rd, D3D_ROOT_SIGNATURE_VERSION_1, &bytes, &errors));
        ComPtr<ID3D12RootSignature> originalRoot;
        check(g.d->CreateRootSignature(0, bytes->GetBufferPointer(), bytes->GetBufferSize(),
                                       IID_PPV_ARGS(&originalRoot)));
        GlassFg::GeometryRoot root;
        std::string error;
        check(GlassFg::CreateGeometryRoot(g.d.Get(), originalRoot.Get(), 0, bytes->GetBufferPointer(),
                                          bytes->GetBufferSize(), root, error, GlassFg::GeometryLayout::PerInstance));
        auto vs = read(dir / "original.jitter.vs.dxil"), ps = read(dir / "original.ps.dxil");
        D3D12_GRAPHICS_PIPELINE_STATE_DESC pd {};
        pd.pRootSignature = originalRoot.Get();
        pd.VS = {vs.data(), vs.size()};
        pd.PS = {ps.data(), ps.size()};
        pd.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
        auto& blend = pd.BlendState.RenderTarget[0];
        blend.BlendEnable = TRUE;
        blend.SrcBlend = D3D12_BLEND_ONE;
        blend.DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
        blend.SrcBlendAlpha = D3D12_BLEND_ONE;
        blend.DestBlendAlpha = D3D12_BLEND_ZERO;
        blend.BlendOp = blend.BlendOpAlpha = D3D12_BLEND_OP_ADD;
        pd.SampleMask = UINT_MAX;
        pd.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
        pd.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
        pd.RasterizerState.DepthClipEnable = TRUE;
        pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        pd.NumRenderTargets = 1;
        pd.RTVFormats[0] = DXGI_FORMAT_R32G32B32A32_FLOAT;
        pd.SampleDesc.Count = 1;
        // Audited Cyberpunk transparent-VS camera block: b1/space0, 848 bytes,
        // row 51 XY. The second call omits the pair and must fall back to the
        // pre-pair behaviour instead of failing.
        const GlassFg::VertexConstantPair camera { 0, 1, 848, 51 };
        GlassFg::GeometryCompiler compiler(std::filesystem::absolute(argv[2]));
        ComPtr<ID3D12PipelineState> paired, unpaired;
        bool pairedPairMissing = true;
        check(compiler.createPackedMotion(g.d.Get(), root, pd, paired, error, &camera, &pairedPairMissing));
        require(!pairedPairMissing, "Paired pipeline reported a missing capture pair");
        error.clear();
        check(compiler.createPackedMotion(g.d.Get(), root, pd, unpaired, error));
        fprintf(stderr, "compiled paired=%p unpaired=%p fallbacks=%llu\n",
                static_cast<void*>(paired.Get()), static_cast<void*>(unpaired.Get()),
                static_cast<unsigned long long>(GlassFg::ReadPackedCaptureFallbackCount()));
        // R6: a vertex shader without the audited b1/space0 block cannot carry
        // the pair, so the same request must recompile without it and count the
        // fallback once.
        auto bare = read(dir / "r6.vs.dxil");
        pd.VS = { bare.data(), bare.size() };
        const auto beforeFallback = GlassFg::ReadPackedCaptureFallbackCount();
        ComPtr<ID3D12PipelineState> fallback;
        error.clear();
        bool fallbackPairMissing = false;
        const HRESULT fallbackStatus =
            compiler.createPackedMotion(g.d.Get(), root, pd, fallback, error, &camera, &fallbackPairMissing);
        const auto afterFallback = GlassFg::ReadPackedCaptureFallbackCount();
        fprintf(stderr, "r6 status=0x%08X object=%p fallbacks=%llu->%llu error=%s\n", (unsigned) fallbackStatus,
                static_cast<void*>(fallback.Get()), static_cast<unsigned long long>(beforeFallback),
                static_cast<unsigned long long>(afterFallback), error.c_str());
        require(fallbackStatus == S_OK && fallback.Get() != nullptr, "Pair-less vertex fallback did not compile");
        require(afterFallback == beforeFallback + 1, "Fallback counter did not increment exactly once");
        // F-01: the pair-less variant is reported so the delivery path can
        // withhold it. A false value here means the caller would ship a record
        // whose motion is the raw jittered difference.
        require(fallbackPairMissing, "Pair-less fallback did not report the missing capture pair");
        pd.VS = { vs.data(), vs.size() };
        // F-02 control: extra MRT slots above one are skipped by the rewrite
        // rather than rejected, so this material takes the normal packed path
        // and must leave the coverage fallback counter untouched. The material
        // reads TEXCOORD0/TEXCOORD1, so the pair uses the vertex shader that
        // declares them. The SV_Position-only fixture shader leaves the history
        // block at register row 1, which is a vertex/pixel signature mismatch
        // the game does not produce (2026-09-19 07:2x).
        auto mrt = read(dir / "mrt.ps.dxil");
        auto materialVs = read(dir / "material.vs.dxil");
        auto mrtDesc = pd;
        mrtDesc.VS = { materialVs.data(), materialVs.size() };
        mrtDesc.PS = { mrt.data(), mrt.size() };
        mrtDesc.NumRenderTargets = 3;
        mrtDesc.RTVFormats[1] = mrtDesc.RTVFormats[2] = DXGI_FORMAT_R32G32B32A32_FLOAT;
        const auto mrtBefore = GlassFg::ReadPackedCoverageFallbackCount();
        ComPtr<ID3D12PipelineState> mrtPacked;
        error.clear();
        const HRESULT mrtStatus = compiler.createPackedMotion(g.d.Get(), root, mrtDesc, mrtPacked, error, &camera);
        const auto mrtAfter = GlassFg::ReadPackedCoverageFallbackCount();
        fprintf(stderr, "mrt-material status=0x%08X object=%p fallbacks=%llu->%llu error=%s\n",
                (unsigned) mrtStatus, static_cast<void*>(mrtPacked.Get()),
                static_cast<unsigned long long>(mrtBefore), static_cast<unsigned long long>(mrtAfter),
                error.c_str());
        require(mrtStatus == S_OK && mrtPacked.Get() != nullptr, "MRT material packed variant did not compile");
        require(mrtAfter == mrtBefore, "MRT material took the coverage fallback");
        // F-02 fallback: a material that exports colour plus SV_Depth cannot
        // keep its material equation on the packed path, so the packed path
        // recovers as a coverage-only pixel stage. That stage carries the
        // capture delta as well, must compile, and must increment the counter
        // exactly once. The work continues below with the recovered pipeline.
        auto materialDepth = read(dir / "materialdepth.ps.dxil");
        auto depthDesc = mrtDesc;
        depthDesc.NumRenderTargets = 1;
        depthDesc.RTVFormats[1] = depthDesc.RTVFormats[2] = DXGI_FORMAT_UNKNOWN;
        depthDesc.PS = { materialDepth.data(), materialDepth.size() };
        const auto coverageBefore = GlassFg::ReadPackedCoverageFallbackCount();
        ComPtr<ID3D12PipelineState> coverage;
        error.clear();
        const HRESULT coverageStatus = compiler.createPackedMotion(g.d.Get(), root, depthDesc, coverage, error, &camera);
        const auto coverageAfter = GlassFg::ReadPackedCoverageFallbackCount();
        fprintf(stderr, "coverage-fallback status=0x%08X object=%p fallbacks=%llu->%llu error=%s\n",
                (unsigned) coverageStatus, static_cast<void*>(coverage.Get()),
                static_cast<unsigned long long>(coverageBefore), static_cast<unsigned long long>(coverageAfter),
                error.c_str());
        require(coverageStatus == S_OK && coverage.Get() != nullptr, "Packed coverage fallback did not compile");
        require(coverageAfter == coverageBefore + 1, "Packed coverage fallback counter did not increment");
        D3D12_HEAP_PROPERTIES hp {};
        hp.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC td {};
        td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        td.Width = W;
        td.Height = H;
        td.DepthOrArraySize = td.MipLevels = 1;
        td.SampleDesc.Count = 1;
        td.Format = pd.RTVFormats[0];
        td.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
        ComPtr<ID3D12Resource> color;
        check(g.d->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &td, D3D12_RESOURCE_STATE_RENDER_TARGET,
                                           nullptr, IID_PPV_ARGS(&color)));
        D3D12_DESCRIPTOR_HEAP_DESC hd {};
        hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
        hd.NumDescriptors = 1;
        ComPtr<ID3D12DescriptorHeap> heap;
        check(g.d->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&heap)));
        auto rtv = heap->GetCPUDescriptorHandleForHeapStart();
        g.d->CreateRenderTargetView(color.Get(), nullptr, rtv);
        const auto uav = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        auto writes = g.buffer(OriginalBytes, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_COPY_DEST, uav);
        auto capture = g.buffer(Pixels * 8, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_COPY_DEST, uav);
        auto current = g.buffer(sizeof(History) * 3, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, uav);
        auto previous = g.buffer(sizeof(History) * 3, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
        auto mapping = g.buffer(sizeof(GlassFg::GeometryInstance), D3D12_HEAP_TYPE_UPLOAD,
                                D3D12_RESOURCE_STATE_GENERIC_READ);
        auto constants = g.buffer(256, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
        GlassFg::MaterialCaptureConstants pc { 0, 0, 1.f / W, 1.f / H, 0, 0, 2, 0, 0, 0, W, H, 0, W,
                                               Pixels, 0, 0.f, 0.f, 0.f, 0.f };
        upload(constants.Get(), &pc, sizeof(pc));
        std::vector<char> cameraBytes(848);
        auto cameraBuffer = g.buffer(cameraBytes.size(), D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
        std::vector<char> zero(Pixels * 8);
        auto zeros = g.buffer(zero.size(), D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
        upload(zeros.Get(), zero.data(), zero.size());
        auto rb = g.buffer(OriginalBytes + Pixels * 8 + sizeof(History) * 3, D3D12_HEAP_TYPE_READBACK,
                           D3D12_RESOURCE_STATE_COPY_DEST);
        struct Phase
        {
            const char* name;
            bool pair;
            float current[2];
            float previous[2];
            float expected[2];
            uint32_t historyFrame { 1 };
            uint32_t frame { 2 };
            uint32_t previousFrame { 1 };
            bool expectPackedDiscard { false };
        };
        // The fixture clip corners are (+-1, -1/3) so the reconstructed raw term
        // is exactly zero and only the pair term can move the packed value.
        constexpr Phase phases[] {
            // Expected values are the packed payload decoded at 1/8 px, so 4 is
            // the four-pixel clamp bound while 2 stays below it and pins the
            // half-UV scale of the pair delta.
            { "delta", true, { 0.25f, 0.5f }, { -0.25f, -0.5f }, { 4, -4 } },
            { "scale", true, { 0.03125f, 0.0625f }, { -0.03125f, -0.0625f }, { 2, -2 } },
            { "equal", true, { 0.25f, 0.5f }, { 0.25f, 0.5f }, { 0, 0 } },
            { "unpaired", false, { 0.25f, 0.5f }, { -0.25f, -0.5f }, { 0, 0 } },
            // A non-finite captured word must not reach the delta term.
            { "nonfinite", true, { 0.25f, 0.5f }, { std::numeric_limits<float>::quiet_NaN(), 0.5f }, { 0, 0 } },
            // A tag that is not the expected predecessor is rejected: the
            // rewrite suppresses its packed layer, while the material's own
            // writes remain (asserted for every phase below).
            { "invalid", true, { 0.25f, 0.5f }, { -0.25f, -0.5f }, { 0, 0 }, 0, 2, 1, true },
            // F-03 counterexample: a frame recorded by the pair-less pipeline
            // advances the tag header and leaves words 24/28 untouched, so the
            // next paired frame adds a stale difference. The observed value is
            // below the four-pixel clamp; a tagged validity marker (R2) would
            // make this zero instead.
            { "stale", true, { 0.03125f, 0.0625f }, { 0, 0 }, { 1, -1 }, 2, 3, 2 },
            // The same window when the words still hold the last paired frame:
            // the delta then spans two frames and saturates the clamp.
            { "staleswap", true, { 0.25f, 0.5f }, { -0.25f, -0.5f }, { 4, -4 }, 2, 3, 2 },
        };
        UINT64 verified = 0;
        bool allPhasesOk = true;
        float measured[std::size(phases)][2] {};
        for (size_t p = 0; p < std::size(phases); ++p)
        {
            const auto& phase = phases[p];
            History history[3] {
                { { -1, -1, .5f, 1 }, phase.historyFrame, 7, {} },
                { { -1, 3, .5f, 1 }, phase.historyFrame, 7, {} },
                { { 3, -1, .5f, 1 }, phase.historyFrame, 7, {} },
            };
            for (auto& record : history)
            {
                memcpy(&record.unused[0], &phase.previous[0], 4);
                memcpy(&record.unused[1], &phase.previous[1], 4);
            }
            upload(previous.Get(), history, sizeof(history));
            std::fill(cameraBytes.begin(), cameraBytes.end(), char(0));
            memcpy(cameraBytes.data() + 51 * 16, &phase.current[0], 4);
            memcpy(cameraBytes.data() + 51 * 16 + 4, &phase.current[1], 4);
            upload(cameraBuffer.Get(), cameraBytes.data(), cameraBytes.size());
            const GlassFg::GeometryInstance map { 0, 3, 0, 7, 0, 0, W, H, 1, W, Pixels + 1, 0, { 1, 0, 0, 0 } };
            upload(mapping.Get(), &map, sizeof(map));
            g.begin();
            g.c->CopyBufferRegion(writes.Get(), 0, zeros.Get(), 0, OriginalBytes);
            g.c->CopyBufferRegion(capture.Get(), 0, zeros.Get(), 0, Pixels * 8);
            g.barrier(writes.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            g.barrier(capture.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            g.c->SetGraphicsRootSignature(root.extended.Get());
            g.c->SetPipelineState(phase.pair ? paired.Get() : unpaired.Get());
            g.c->SetGraphicsRootUnorderedAccessView(0, writes->GetGPUVirtualAddress());
            g.c->SetGraphicsRootConstantBufferView(1, cameraBuffer->GetGPUVirtualAddress());
            const UINT hc[] { 0, 1, 3, 0, 1, 0, phase.frame, phase.previousFrame };
            g.c->SetGraphicsRoot32BitConstants(root.constantsSlot, 8, hc, 0);
            g.c->SetGraphicsRootShaderResourceView(root.previousSlot, previous->GetGPUVirtualAddress());
            g.c->SetGraphicsRootUnorderedAccessView(root.currentSlot, current->GetGPUVirtualAddress());
            g.c->SetGraphicsRootConstantBufferView(root.materialSlot, constants->GetGPUVirtualAddress());
            g.c->SetGraphicsRootUnorderedAccessView(root.captureSlot, capture->GetGPUVirtualAddress());
            g.c->SetGraphicsRootShaderResourceView(root.instanceSlot, mapping->GetGPUVirtualAddress());
            const D3D12_VIEWPORT viewport { 0, 0, float(W), float(H), 0, 1 };
            const D3D12_RECT scissor { 0, 0, W, H };
            g.c->RSSetViewports(1, &viewport);
            g.c->RSSetScissorRects(1, &scissor);
            g.c->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            g.c->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
            const float clear[4] {};
            g.c->ClearRenderTargetView(rtv, clear, 0, nullptr);
            g.c->DrawInstanced(3, 1, 0, 0);
            g.barrier(writes.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
            g.barrier(capture.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
            g.c->CopyBufferRegion(rb.Get(), 0, writes.Get(), 0, OriginalBytes);
            g.c->CopyBufferRegion(rb.Get(), OriginalBytes, capture.Get(), 0, Pixels * 8);
            g.barrier(writes.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
            g.barrier(capture.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
            g.barrier(current.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
            g.c->CopyBufferRegion(rb.Get(), OriginalBytes + Pixels * 8, current.Get(), 0, sizeof(History) * 3);
            g.barrier(current.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            g.finish();
            void* data;
            check(rb->Map(0, nullptr, &data));
            const auto written = static_cast<const History*>(static_cast<const void*>(
                static_cast<const char*>(data) + OriginalBytes + Pixels * 8));
            float extra0 = 0, extra1 = 0;
            memcpy(&extra0, &written[0].unused[0], 4);
            memcpy(&extra1, &written[0].unused[1], 4);
            fprintf(stderr, "phase=%s tag0 frame=%u gen=%u word2=%u word3=%u extra=(%.6f,%.6f)\n", phase.name,
                    written[0].frame, written[0].generation, written[0].unused[0], written[0].unused[1],
                    extra0, extra1);
            const auto words = static_cast<const UINT*>(data);
            fprintf(stderr, "phase=%s words0=%u words1=%u words33=%u\n", phase.name, words[0], words[1],
                    words[1 + W]);
            // The rewritten pixel preamble runs after the original material
            // body, so a rejected history suppresses only the packed layer the
            // rewrite appends; the material's own writes stay untouched.
            require(words[0] == (W - 9) * H, "Original material writes changed");
            bool phaseOk = true;
            bool firstSample = false;
            for (UINT i = 0; i < Pixels; ++i)
            {
                bool covered = i % W >= 8 && i % W != 20;
                require(words[1 + i] == (covered ? i + 1 : 0), "Original raw store/discard changed");
                UINT64 packed;
                memcpy(&packed, static_cast<const char*>(data) + OriginalBytes + i * 8, 8);
                require(bool(packed) == (covered && !phase.expectPackedDiscard), "Packed coverage mismatch");
                if (!packed)
                    continue;
                const auto decode = [](UINT bits) {
                    return float(bits & 1024 ? int(bits) - 2048 : int(bits)) / 8.f;
                };
                const float actualX = decode(UINT((packed >> 35) & 2047));
                const float actualY = decode(UINT((packed >> 24) & 2047));
                if (!firstSample)
                {
                    measured[p][0] = actualX;
                    measured[p][1] = actualY;
                    fprintf(stderr, "phase=%s sample pixel=%u actual=(%.4f,%.4f) expected=(%.4f,%.4f)\n",
                            phase.name, i, actualX, actualY, phase.expected[0], phase.expected[1]);
                    firstSample = true;
                }
                if (actualX != phase.expected[0] || actualY != phase.expected[1])
                {
                    phaseOk = false;
                }
                ++verified;
            }
            rb->Unmap(0, nullptr);
            fprintf(stderr, "phase=%s ok=%d\n", phase.name, phaseOk ? 1 : 0);
            allPhasesOk = allPhasesOk && phaseOk;
        }
        require(allPhasesOk, "Packed motion does not match the capture pair delta");
        printf("PACKED_JITTER_GPU_OK phases=%zu covered_pixels=%llu", std::size(phases), verified);
        for (size_t p = 0; p < std::size(phases); ++p)
            printf(" %s_px=%.0f,%.0f", phases[p].name, measured[p][0], measured[p][1]);
        printf("\n");
        return 0;
    }
    catch (const std::exception& error)
    {
        fprintf(stderr, "%s\n", error.what());
        return 1;
    }
}
