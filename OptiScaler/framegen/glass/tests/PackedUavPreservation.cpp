#include "GeometryTestDevice.h"

int wmain(int argc, wchar_t** argv)
{
    try
    {
        require(argc == 3, "Expected fixture binary directory and dxcompiler.dll");
        const std::filesystem::path dir(argv[1]);
        constexpr UINT W = 64, H = 32, Pixels = W * H, OriginalBytes = (Pixels + 1) * 4;
        Device g;
        D3D12_ROOT_PARAMETER originalParameter {};
        originalParameter.ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
        originalParameter.Descriptor = {3, 0};
        originalParameter.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
        D3D12_ROOT_SIGNATURE_DESC rd {1, &originalParameter, 0, nullptr,
                                      D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT};
        ComPtr<ID3DBlob> bytes, errors;
        check(D3D12SerializeRootSignature(&rd, D3D_ROOT_SIGNATURE_VERSION_1, &bytes, &errors));
        ComPtr<ID3D12RootSignature> originalRoot;
        check(g.d->CreateRootSignature(0, bytes->GetBufferPointer(), bytes->GetBufferSize(), IID_PPV_ARGS(&originalRoot)));
        GlassFg::GeometryRoot root;
        std::string error;
        check(GlassFg::CreateGeometryRoot(g.d.Get(), originalRoot.Get(), 0, bytes->GetBufferPointer(),
                                         bytes->GetBufferSize(), root, error, GlassFg::GeometryLayout::PerInstance));
        auto vs = read(dir / "original.vs.dxil"), ps = read(dir / "original.ps.dxil");
        auto patchedVs = read(dir / "patched.vs.dxil"), patchedPs = read(dir / "patched.ps.dxil");
        D3D12_GRAPHICS_PIPELINE_STATE_DESC pd {};
        pd.pRootSignature = originalRoot.Get();
        pd.VS = {vs.data(), vs.size()}; pd.PS = {ps.data(), ps.size()};
        pd.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
        auto& blend = pd.BlendState.RenderTarget[0];
        blend.BlendEnable = TRUE;
        blend.SrcBlend = D3D12_BLEND_ONE; blend.DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
        blend.SrcBlendAlpha = D3D12_BLEND_ONE; blend.DestBlendAlpha = D3D12_BLEND_ZERO;
        blend.BlendOp = blend.BlendOpAlpha = D3D12_BLEND_OP_ADD;
        pd.SampleMask = UINT_MAX;
        pd.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
        pd.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
        pd.RasterizerState.DepthClipEnable = TRUE;
        pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        pd.NumRenderTargets = 1; pd.RTVFormats[0] = DXGI_FORMAT_R32G32B32A32_FLOAT;
        pd.SampleDesc.Count = 1;
        ComPtr<ID3D12PipelineState> original, patched;
        GlassFg::GeometryCompiler guardedCompiler(std::filesystem::absolute(argv[2]));
        require(FAILED(guardedCompiler.createPackedMotion(g.d.Get(), root, pd, patched, error)),
                "Default production path admitted original UAVs");
        if (error != "Pixel UAV side effects unsupported") throw std::runtime_error("Unexpected admission rejection: " + error);
        check(g.d->CreateGraphicsPipelineState(&pd, IID_PPV_ARGS(&original)));
        pd.pRootSignature = root.extended.Get();
        pd.VS = {patchedVs.data(), patchedVs.size()}; pd.PS = {patchedPs.data(), patchedPs.size()};
        check(g.d->CreateGraphicsPipelineState(&pd, IID_PPV_ARGS(&patched)));
        D3D12_HEAP_PROPERTIES hp {}; hp.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC td {};
        td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D; td.Width = W; td.Height = H;
        td.DepthOrArraySize = td.MipLevels = 1; td.SampleDesc.Count = 1;
        td.Format = pd.RTVFormats[0]; td.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
        ComPtr<ID3D12Resource> color;
        check(g.d->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &td, D3D12_RESOURCE_STATE_RENDER_TARGET,
                                           nullptr, IID_PPV_ARGS(&color)));
        D3D12_DESCRIPTOR_HEAP_DESC hd {}; hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV; hd.NumDescriptors = 1;
        ComPtr<ID3D12DescriptorHeap> heap; check(g.d->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&heap)));
        auto rtv = heap->GetCPUDescriptorHandleForHeapStart(); g.d->CreateRenderTargetView(color.Get(), nullptr, rtv);
        const auto uav = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        auto writes = g.buffer(OriginalBytes, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_COPY_DEST, uav);
        auto capture = g.buffer(Pixels * 8, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_COPY_DEST, uav);
        auto current = g.buffer(sizeof(History) * 3, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, uav);
        auto previous = g.buffer(sizeof(History) * 3, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
        const History baseHistory[3] {{{-1,-1,.5f,1},1,7,{}}, {{-1,3,.5f,1},1,7,{}}, {{3,-1,.5f,1},1,7,{}}};
        auto mapping = g.buffer(64, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
        auto constants = g.buffer(256, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
        GlassFg::MaterialCaptureConstants pc {0,0,1.f/W,1.f/H,0,0,2,0,0,0,W,H,0,W,Pixels,0};
        upload(constants.Get(), &pc, sizeof(pc));
        std::vector<char> zero(Pixels * 8);
        auto zeros = g.buffer(zero.size(), D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
        upload(zeros.Get(), zero.data(), zero.size());
        auto rb = g.buffer(OriginalBytes + Pixels * 8, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST);
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint {}; UINT rows; UINT64 rowBytes, colorBytes;
        g.d->GetCopyableFootprints(&td, 0, 1, 0, &footprint, &rows, &rowBytes, &colorBytes);
        auto crb = g.buffer(colorBytes, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST);
        std::vector<char> baselineColor;
        UINT64 verified = 0;
        struct Case { float dx, dy; bool captured; };
        constexpr Case cases[] {{0,0,false}, {0,0,true}, {0,0,false}, {2,-1,true},
                                {256,0,false}, {-256,0,false}, {0,256,false}, {0,-256,false},
                                {127.75f,-128,true}, {0,0,false}, {0,0,false}};
        for (UINT pass = 0; pass < std::size(cases); ++pass)
        {
            History history[3]; memcpy(history, baseHistory, sizeof(history));
            for (auto& vertex : history)
            {
                vertex.clip[0] += 2.f * cases[pass].dx / W;
                vertex.clip[1] -= 2.f * cases[pass].dy / H;
            }
            upload(previous.Get(), history, sizeof(history));
            GlassFg::GeometryInstance map {0,3,0,7,0,0,W,H,1,W,Pixels+1,0,{1,0,0,0}};
            if (pass == 2) map = {}; // Missing identity must not suppress original writes/color.
            if (pass == 9) map.reserved[0] = 32768; // Must not wrap to another object ID.
            if (pass == 10) map.generation = 8; // Arena reuse must not read old generation 7.
            upload(mapping.Get(), &map, sizeof(map));
            g.begin();
            g.c->CopyBufferRegion(writes.Get(), 0, zeros.Get(), 0, OriginalBytes);
            g.c->CopyBufferRegion(capture.Get(), 0, zeros.Get(), 0, Pixels * 8);
            g.barrier(writes.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            g.barrier(capture.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            g.c->SetGraphicsRootSignature(pass ? root.extended.Get() : originalRoot.Get());
            g.c->SetPipelineState(pass ? patched.Get() : original.Get());
            g.c->SetGraphicsRootUnorderedAccessView(0, writes->GetGPUVirtualAddress());
            if (pass)
            {
                const UINT hc[] {0,1,3,0,1,0,2,1};
                g.c->SetGraphicsRoot32BitConstants(root.constantsSlot, 8, hc, 0);
                g.c->SetGraphicsRootShaderResourceView(root.previousSlot, previous->GetGPUVirtualAddress());
                g.c->SetGraphicsRootUnorderedAccessView(root.currentSlot, current->GetGPUVirtualAddress());
                g.c->SetGraphicsRootConstantBufferView(root.materialSlot, constants->GetGPUVirtualAddress());
                g.c->SetGraphicsRootUnorderedAccessView(root.captureSlot, capture->GetGPUVirtualAddress());
                g.c->SetGraphicsRootShaderResourceView(root.instanceSlot, mapping->GetGPUVirtualAddress());
            }
            const D3D12_VIEWPORT viewport {0,0,float(W),float(H),0,1}; const D3D12_RECT scissor {0,0,W,H};
            g.c->RSSetViewports(1, &viewport); g.c->RSSetScissorRects(1, &scissor);
            g.c->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            g.c->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
            const float clear[4] {}; g.c->ClearRenderTargetView(rtv, clear, 0, nullptr);
            g.c->DrawInstanced(3, 1, 0, 0);
            g.barrier(writes.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
            g.barrier(capture.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
            g.c->CopyBufferRegion(rb.Get(), 0, writes.Get(), 0, OriginalBytes);
            g.c->CopyBufferRegion(rb.Get(), OriginalBytes, capture.Get(), 0, Pixels * 8);
            g.barrier(color.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
            D3D12_TEXTURE_COPY_LOCATION source {}; source.pResource = color.Get(); source.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            D3D12_TEXTURE_COPY_LOCATION dest {}; dest.pResource = crb.Get(); dest.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT; dest.PlacedFootprint = footprint;
            g.c->CopyTextureRegion(&dest, 0, 0, 0, &source, nullptr);
            g.barrier(color.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
            g.barrier(writes.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
            g.barrier(capture.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
            g.finish();
            void* data; check(rb->Map(0, nullptr, &data));
            auto words = static_cast<const UINT*>(data);
            require(words[0] == (W-9)*H, "Original atomic count changed");
            for (UINT i = 0; i < Pixels; ++i)
            {
                bool covered = i % W >= 8 && i % W != 20;
                require(words[1+i] == (covered ? i+1 : 0), "Original raw store/discard changed");
                UINT64 packed; memcpy(&packed, static_cast<const char*>(data)+OriginalBytes+i*8, 8);
                require(bool(packed) == (cases[pass].captured && covered), "Packed coverage or rejected input mismatch");
                if (packed)
                {
                    const auto decode = [](UINT bits) { return float(bits & 1024 ? int(bits)-2048 : int(bits)) / 8.f; };
                    require(decode(UINT((packed >> 35) & 2047)) == cases[pass].dx &&
                            decode(UINT((packed >> 24) & 2047)) == cases[pass].dy, "Packed motion changed direction/magnitude");
                }
                ++verified;
            }
            rb->Unmap(0, nullptr);
            check(crb->Map(0, nullptr, &data));
            if (!pass) baselineColor.assign(static_cast<char*>(data), static_cast<char*>(data)+colorBytes);
            else require(memcmp(baselineColor.data(), data, size_t(colorBytes)) == 0, "Original color changed");
            crb->Unmap(0, nullptr);
        }
        printf("PACKED_UAV_PRESERVATION_OK pixels=%llu passes=%zu original_atomic_exact=1 raw_store_exact=1 color_exact=1 discard_exact=1 motion_range_checked=1 reused_generation_rejected=1\n", verified, std::size(cases));
        return 0;
    }
    catch (const std::exception& error) { fprintf(stderr, "%s\n", error.what()); return 1; }
}
