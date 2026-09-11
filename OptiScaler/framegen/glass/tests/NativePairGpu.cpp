#include "GeometryTestDevice.h"
int wmain(int argc, wchar_t** argv)
{
    try
    {
        require(argc == 3, "Expected fixture directory and dxcompiler.dll");
        Device g;
        const auto vs = read(std::filesystem::path(argv[1]) / "native-pair-vs.dxil");
        const auto ps = read(std::filesystem::path(argv[1]) / "native-pair-ps.dxil");
        D3D12_ROOT_SIGNATURE_DESC desc {};
        desc.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
        ComPtr<ID3DBlob> bytes, errors;
        check(D3D12SerializeRootSignature(&desc, D3D_ROOT_SIGNATURE_VERSION_1, &bytes, &errors));
        ComPtr<ID3D12RootSignature> original;
        check(g.d->CreateRootSignature(0, bytes->GetBufferPointer(), bytes->GetBufferSize(), IID_PPV_ARGS(&original)));
        GlassFg::GeometryRoot root;
        std::string error;
        check(GlassFg::CreateGeometryRoot(g.d.Get(), original.Get(), 0, bytes->GetBufferPointer(), bytes->GetBufferSize(), root, error));
        D3D12_GRAPHICS_PIPELINE_STATE_DESC pd {};
        pd.pRootSignature = original.Get(); pd.VS = {vs.data(), vs.size()}; pd.PS = {ps.data(), ps.size()};
        pd.SampleMask = UINT_MAX; pd.SampleDesc.Count = 1;
        pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        pd.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID; pd.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
        pd.RasterizerState.DepthClipEnable = TRUE;
        pd.BlendState.RenderTarget[0].RenderTargetWriteMask = 15;
        pd.NumRenderTargets = 1; pd.RTVFormats[0] = DXGI_FORMAT_R32G32B32A32_FLOAT;
        GlassFg::GeometryCompiler compiler {std::filesystem::path(argv[2])};
        const GlassFg::VertexClipPair pair {1,2};
        ComPtr<ID3D12PipelineState> pipeline;
        check(compiler.createVertexCapture(g.d.Get(), root, pd, pipeline, error, nullptr, &pair));
        constexpr size_t size = 5 * 64;
        std::array<unsigned char, size> zero {};
        auto prior = g.buffer(size, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
        auto next = g.buffer(size, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
        auto back = g.buffer(size, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST);
        upload(prior.Get(), zero.data(), size);
        D3D12_RESOURCE_DESC td {}; td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        td.Width = td.Height = 16; td.DepthOrArraySize = td.MipLevels = 1;
        td.Format = pd.RTVFormats[0]; td.SampleDesc.Count = 1; td.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
        D3D12_HEAP_PROPERTIES hp {}; hp.Type = D3D12_HEAP_TYPE_DEFAULT;
        ComPtr<ID3D12Resource> color;
        check(g.d->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &td, D3D12_RESOURCE_STATE_RENDER_TARGET, nullptr, IID_PPV_ARGS(&color)));
        D3D12_DESCRIPTOR_HEAP_DESC hd {}; hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV; hd.NumDescriptors = 1;
        ComPtr<ID3D12DescriptorHeap> heap;
        check(g.d->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&heap)));
        const auto rtv = heap->GetCPUDescriptorHandleForHeapStart();
        g.d->CreateRenderTargetView(color.Get(), nullptr, rtv);
        g.begin(); g.c->CopyBufferRegion(next.Get(), 0, prior.Get(), 0, size);
        g.barrier(next.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        g.c->SetPipelineState(pipeline.Get()); g.c->SetGraphicsRootSignature(root.extended.Get());
        const GlassFg::VertexHistoryConstants constants {1,3,0,0,1,7,11,10};
        g.c->SetGraphicsRoot32BitConstants(root.constantsSlot, 8, &constants, 0);
        g.c->SetGraphicsRootShaderResourceView(root.previousSlot, prior->GetGPUVirtualAddress());
        g.c->SetGraphicsRootUnorderedAccessView(root.currentSlot, next->GetGPUVirtualAddress());
        const D3D12_VIEWPORT viewport {0,0,16,16,0,1}; const D3D12_RECT scissor {0,0,16,16};
        g.c->RSSetViewports(1, &viewport); g.c->RSSetScissorRects(1, &scissor);
        g.c->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
        g.c->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        g.c->DrawInstanced(3,1,0,0);
        g.barrier(next.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
        g.c->CopyBufferRegion(back.Get(),0,next.Get(),0,size); g.finish();
        void* mapped = nullptr; D3D12_RANGE range {0,size}; check(back->Map(0,&range,&mapped));
        const auto* data = static_cast<const unsigned char*>(mapped);
        require(!memcmp(data,zero.data(),64) && !memcmp(data+256,zero.data(),64), "Guard record changed");
        for (unsigned i = 0; i < 3; ++i)
        {
            const float x = i == 0 ? -0.5f : i == 1 ? 0.f : 0.5f;
            const float y = i == 1 ? 0.5f : -0.5f, w = float(i+2);
            const float expected[] = {x*w,y*w,0.5f*w,w,(x+0.125f)*(w+1),(y-0.25f)*(w+1),0.25f*(w+1),w+1};
            const auto* record = data + (i+1)*64;
            require(!memcmp(record+32,expected,sizeof(expected)), "Native clip pair mismatch");
            uint32_t tags[2]; memcpy(tags,record+16,8);
            require(tags[0] == 11 && tags[1] == 7, "Native pair tags mismatch");
            float rasterX; memcpy(&rasterX,record,4);
            require(rasterX == (x+0.0625f)*w, "Raster clip replaced by unjittered clip");
        }
        D3D12_RANGE noWrite {0,0}; back->Unmap(0,&noWrite);
        puts("NATIVE_PAIR_GPU_OK three_vertices full_clip_w tags guards raster_jitter_distinct");
        return 0;
    }
    catch (const std::exception& e) { fprintf(stderr,"%s\n",e.what()); return 1; }
}
