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
        pd.BlendState.RenderTarget[0].SrcBlend = pd.BlendState.RenderTarget[0].SrcBlendAlpha = D3D12_BLEND_ONE;
        pd.BlendState.RenderTarget[0].DestBlend = pd.BlendState.RenderTarget[0].DestBlendAlpha = D3D12_BLEND_ZERO;
        pd.BlendState.RenderTarget[0].BlendOp = pd.BlendState.RenderTarget[0].BlendOpAlpha = D3D12_BLEND_OP_ADD;
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
        const GlassFg::NativeClipInputs inputs {1,2};
        ComPtr<ID3D12PipelineState> motionPipeline;
        if (FAILED(compiler.createNativeMotionCapture(g.d.Get(), root, pd, motionPipeline, error, inputs)))
            throw std::runtime_error("Native pixel compiler: "+error);
        constexpr size_t pixelBytes = (16*16+1)*sizeof(CaptureRecord);
        std::vector<unsigned char> clearPixels(pixelBytes);
        auto pixelZero=g.buffer(pixelBytes,D3D12_HEAP_TYPE_UPLOAD,D3D12_RESOURCE_STATE_GENERIC_READ);
        auto pixels=g.buffer(pixelBytes,D3D12_HEAP_TYPE_DEFAULT,D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
        auto pixelBack=g.buffer(pixelBytes,D3D12_HEAP_TYPE_READBACK,D3D12_RESOURCE_STATE_COPY_DEST);
        auto pixelConstants=g.buffer(256,D3D12_HEAP_TYPE_UPLOAD,D3D12_RESOURCE_STATE_GENERIC_READ);
        upload(pixelZero.Get(),clearPixels.data(),pixelBytes);
        const GlassFg::MaterialCaptureConstants pc {0,0,1.f/16,1.f/16,0,0,11,0,0,0,16,16,0,16,256,0};
        upload(pixelConstants.Get(),&pc,sizeof(pc));
        g.begin();
        g.c->CopyBufferRegion(pixels.Get(),0,pixelZero.Get(),0,pixelBytes);
        g.barrier(pixels.Get(),D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        g.barrier(next.Get(),D3D12_RESOURCE_STATE_COPY_SOURCE,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        g.c->SetGraphicsRootSignature(root.extended.Get()); g.c->SetPipelineState(motionPipeline.Get());
        g.c->SetGraphicsRoot32BitConstants(root.constantsSlot,8,&constants,0);
        g.c->SetGraphicsRootShaderResourceView(root.previousSlot,prior->GetGPUVirtualAddress());
        g.c->SetGraphicsRootUnorderedAccessView(root.currentSlot,next->GetGPUVirtualAddress());
        g.c->SetGraphicsRootConstantBufferView(root.materialSlot,pixelConstants->GetGPUVirtualAddress());
        g.c->SetGraphicsRootUnorderedAccessView(root.captureSlot,pixels->GetGPUVirtualAddress());
        g.c->RSSetViewports(1,&viewport); g.c->RSSetScissorRects(1,&scissor);
        g.c->OMSetRenderTargets(1,&rtv,FALSE,nullptr);
        g.c->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        g.c->DrawInstanced(3,1,0,0);
        g.barrier(pixels.Get(),D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_COPY_SOURCE);
        g.c->CopyBufferRegion(pixelBack.Get(),0,pixels.Get(),0,pixelBytes); g.finish();
        D3D12_RANGE pixelRange {0,pixelBytes}; mapped=nullptr; check(pixelBack->Map(0,&pixelRange,&mapped));
        unsigned count=0;
        const auto* samples=static_cast<const CaptureRecord*>(mapped);
        double maxError=0;
        for(unsigned i=0;i<256;++i) if(samples[i].frame) {
            require(samples[i].frame==11 && std::isfinite(samples[i].motion[0]) && std::isfinite(samples[i].motion[1]), "Native pixel record invalid");
            const double x=double(i%16)+0.5,y=double(i/16)+0.5;
            const double b1=(12-y)/8,b2=(x-4.5-4*b1)/8,b0=1-b1-b2;
            const double b[]={b0,b1,b2}, px[]={-.5,0,.5},py[]={-.5,.5,-.5};
            double oldX=0,oldY=0,oldW=0;
            for(unsigned j=0;j<3;++j) {
                const double weight=b[j]*(double(j)+3)/(double(j)+2);
                oldX+=weight*(px[j]+.125); oldY+=weight*(py[j]-.25); oldW+=weight;
            }
            const double expectedX=(oldX/oldW-(x/8-1-.0625))*.5;
            const double expectedY=(oldY/oldW-(1-y/8))*(-.5);
            const double difference=std::max(std::abs(samples[i].motion[0]-expectedX),std::abs(samples[i].motion[1]-expectedY));
            maxError=std::max(maxError,difference);
            require(difference<1.e-6,"Native pixel perspective/jitter mismatch");
            ++count;
        }
        require(count>0 && count<256,"Native pixel coverage empty or unbounded");
        pixelBack->Unmap(0,&noWrite);
        printf("NATIVE_PIXEL_GPU_OK pixels=%u max_normalized_error=%.9g native_previous_without_history=1\n",count,maxError);
        pd.DepthStencilState.DepthEnable=TRUE;
        pd.DepthStencilState.DepthWriteMask=D3D12_DEPTH_WRITE_MASK_ALL;
        pd.DepthStencilState.DepthFunc=D3D12_COMPARISON_FUNC_LESS;
        pd.DSVFormat=DXGI_FORMAT_D32_FLOAT;
        ComPtr<ID3D12PipelineState> depthOriginal,depthCaptured;
        check(g.d->CreateGraphicsPipelineState(&pd,IID_PPV_ARGS(&depthOriginal)));
        GlassFg::GeometryRoot mappedRoot;
        check(GlassFg::CreateGeometryRoot(g.d.Get(),original.Get(),0,bytes->GetBufferPointer(),bytes->GetBufferSize(),mappedRoot,error,GlassFg::GeometryLayout::PerInstance));
        auto instanceMap=g.buffer(64,D3D12_HEAP_TYPE_UPLOAD,D3D12_RESOURCE_STATE_GENERIC_READ);
        const GlassFg::GeometryInstance instance {0,0,0,1,0,0,16,16,1,16,257,0};
        upload(instanceMap.Get(),&instance,sizeof(instance));
        const GlassFg::InstanceHistoryConstants mappedConstants {0,1,0,0,1,0,11,0};
        const GlassFg::NativeClipInputs auditedInputs {1,2,true};
        if(FAILED(compiler.createNativeMotionCapture(g.d.Get(),mappedRoot,pd,depthCaptured,error,auditedInputs)))
            throw std::runtime_error(error);
#ifdef GLASS_TEST_DEPTH_COVERAGE
        if(FAILED(compiler.createCoverageAudit(g.d.Get(),mappedRoot,pd,depthCaptured,error)))
            throw std::runtime_error(error);
        const GlassFg::GeometryInstance coverageInstance {0,0,0,1,0,0,16,16,32,16,1024,0};
        upload(instanceMap.Get(),&coverageInstance,sizeof(coverageInstance));
        const GlassFg::MaterialCaptureConstants coverageConstants {0,0,1.f/16,1.f/16,0,0,11,0,
            0,0,16,16,288,16,1024,544};
        upload(pixelConstants.Get(),&coverageConstants,sizeof(coverageConstants));
#endif
        auto depthDesc=td; depthDesc.Format=DXGI_FORMAT_D32_FLOAT;
        depthDesc.Flags=D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
        ComPtr<ID3D12Resource> depth;
        check(g.d->CreateCommittedResource(&hp,D3D12_HEAP_FLAG_NONE,&depthDesc,D3D12_RESOURCE_STATE_DEPTH_WRITE,nullptr,IID_PPV_ARGS(&depth)));
        hd.Type=D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
        ComPtr<ID3D12DescriptorHeap> depthHeap;
        check(g.d->CreateDescriptorHeap(&hd,IID_PPV_ARGS(&depthHeap)));
        const auto dsv=depthHeap->GetCPUDescriptorHandleForHeapStart();
        g.d->CreateDepthStencilView(depth.Get(),nullptr,dsv);
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprints[2]; UINT64 lengths[2];
        g.d->GetCopyableFootprints(&td,0,1,0,&footprints[0],nullptr,nullptr,&lengths[0]);
        g.d->GetCopyableFootprints(&depthDesc,0,1,0,&footprints[1],nullptr,nullptr,&lengths[1]);
        const UINT64 stride=(lengths[0]+511)&~UINT64(511);
        const UINT64 variantBytes=(stride+lengths[1]+511)&~UINT64(511);
        auto comparison=g.buffer(variantBytes*2,D3D12_HEAP_TYPE_READBACK,D3D12_RESOURCE_STATE_COPY_DEST);
        for(unsigned variant=0;variant<2;++variant) {
            g.begin();
            const float clearColor[4]{}; const D3D12_RECT occluder{0,0,8,16};
            g.c->ClearRenderTargetView(rtv,clearColor,0,nullptr);
            g.c->ClearDepthStencilView(dsv,D3D12_CLEAR_FLAG_DEPTH,1,0,0,nullptr);
            g.c->ClearDepthStencilView(dsv,D3D12_CLEAR_FLAG_DEPTH,.25f,0,1,&occluder);
            g.c->SetGraphicsRootSignature(variant?mappedRoot.extended.Get():original.Get());
            g.c->SetPipelineState(variant?depthCaptured.Get():depthOriginal.Get());
            if(variant) {
                g.barrier(pixels.Get(),D3D12_RESOURCE_STATE_COPY_SOURCE,D3D12_RESOURCE_STATE_COPY_DEST);
                g.c->CopyBufferRegion(pixels.Get(),0,pixelZero.Get(),0,pixelBytes);
                g.barrier(pixels.Get(),D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
                g.c->SetGraphicsRoot32BitConstants(mappedRoot.constantsSlot,8,&mappedConstants,0);
                g.c->SetGraphicsRootShaderResourceView(mappedRoot.previousSlot,prior->GetGPUVirtualAddress());
                g.c->SetGraphicsRootUnorderedAccessView(mappedRoot.currentSlot,next->GetGPUVirtualAddress());
                g.c->SetGraphicsRootConstantBufferView(mappedRoot.materialSlot,pixelConstants->GetGPUVirtualAddress());
                g.c->SetGraphicsRootUnorderedAccessView(mappedRoot.captureSlot,pixels->GetGPUVirtualAddress());
                g.c->SetGraphicsRootShaderResourceView(mappedRoot.instanceSlot,instanceMap->GetGPUVirtualAddress());
            }
            g.c->RSSetViewports(1,&viewport); g.c->RSSetScissorRects(1,&scissor);
            g.c->OMSetRenderTargets(1,&rtv,FALSE,&dsv);
            g.c->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            g.c->DrawInstanced(3,1,0,0);
            for(unsigned resource=0;resource<2;++resource) {
                auto* source=resource?depth.Get():color.Get();
                const auto state=resource?D3D12_RESOURCE_STATE_DEPTH_WRITE:D3D12_RESOURCE_STATE_RENDER_TARGET;
                g.barrier(source,state,D3D12_RESOURCE_STATE_COPY_SOURCE);
                D3D12_TEXTURE_COPY_LOCATION from{}; from.pResource=source; from.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
                D3D12_TEXTURE_COPY_LOCATION to{}; to.pResource=comparison.Get(); to.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
                to.PlacedFootprint=footprints[resource]; to.PlacedFootprint.Offset=variant*variantBytes+(resource?stride:0);
                g.c->CopyTextureRegion(&to,0,0,0,&from,nullptr);
                g.barrier(source,D3D12_RESOURCE_STATE_COPY_SOURCE,state);
            }
            if(variant) {
                g.barrier(pixels.Get(),D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_COPY_SOURCE);
                g.c->CopyBufferRegion(pixelBack.Get(),0,pixels.Get(),0,pixelBytes);
            }
            g.finish();
        }
        D3D12_RANGE comparisonRange{0,SIZE_T(variantBytes*2)}; mapped=nullptr;
        check(comparison->Map(0,&comparisonRange,&mapped));
        const auto* compared=static_cast<const unsigned char*>(mapped);
        void* pixelData=nullptr; check(pixelBack->Map(0,&pixelRange,&pixelData));
#ifndef GLASS_TEST_DEPTH_COVERAGE
        const auto* depthSamples=static_cast<const CaptureRecord*>(pixelData);
#endif
        unsigned visible=0;
        for(unsigned y=0;y<16;++y) {
            for(unsigned r=0;r<2;++r) {
                const size_t offset=(r?size_t(stride):0)+y*footprints[r].Footprint.RowPitch;
                require(!memcmp(compared+offset,compared+variantBytes+offset,16*(r?4:16)),"Native capture changed color/depth");
            }
            const auto* rgba=reinterpret_cast<const float*>(compared+y*footprints[0].Footprint.RowPitch);
            for(unsigned x=0;x<16;++x) {
                const bool drawn=rgba[x*4+3]!=0;
#ifdef GLASS_TEST_DEPTH_COVERAGE
                const auto* bits=static_cast<const uint32_t*>(pixelData);
                for (unsigned base : {32u,288u,544u}) {
                    const unsigned bit=base+y*16+x;
                    require(bool((bits[bit/32]>>(bit%32))&1)==drawn,"Depth coverage differs from original visible color");
                }
#else
                require((depthSamples[1+y*16+x].frame==11)==drawn,"Native UAV coverage differs from depth-tested color");
#endif
                if(drawn) ++visible;
            }
        }
        require(visible>0 && visible<count,"Occlusion test did not remove pixels");
        const auto* auditWords=static_cast<const uint32_t*>(pixelData);
#ifdef GLASS_TEST_DEPTH_COVERAGE
        require(auditWords[0]==0,"Depth coverage status mismatch");
        puts("DEPTH_COVERAGE_GPU_OK original_color_depth_and_three_masks");
#else
        require(auditWords[0]==0 && auditWords[4]==visible,"Native invocation counter/status mismatch");
#endif
        pixelBack->Unmap(0,&noWrite); comparison->Unmap(0,&noWrite);
        printf("NATIVE_DEPTH_GPU_OK exact_color_depth=256 occluded_pixels=%u visible_pixels=%u\n",count-visible,visible);
        puts("NATIVE_PAIR_GPU_OK three_vertices full_clip_w tags guards raster_jitter_distinct");
        return 0;
    }
    catch (const std::exception& e) { fprintf(stderr,"%s\n",e.what()); return 1; }
}
