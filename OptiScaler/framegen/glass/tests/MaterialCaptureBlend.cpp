// Independent GPU check of extracting the second dual-source output via blend state.
// Synthetic draws only. No game attachment, private API, timing or quality assertion.
#include "../MaterialCaptureBlend.h"
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <d3dcompiler.h>
#include <wrl/client.h>
#include <cstdio>
#include <cmath>
#include <cstring>
#include <stdexcept>
using Microsoft::WRL::ComPtr;
static void check(HRESULT hr)
{
    if (FAILED(hr))
    {
        printf("HRESULT %08lx\n", (unsigned long) hr);
        throw std::runtime_error("D3D operation");
    }
}
static const char* code = R"(
cbuffer Data:register(b0){float4 F;float4 T;}
float4 VS(uint id:SV_VertexID):SV_Position{float2 p=float2((id<<1)&2,id&2);return float4(p*float2(2,-2)+float2(-1,1),0,1);}
struct Output{float4 color:SV_Target0;float4 transmission:SV_Target1;};
Output PS(float4 p:SV_Position){if(p.x<8)discard;float3 t=p.x>=16?float3(1,1,1):T.rgb;Output o;o.color=float4(F.rgb,F.a);o.transmission=float4(t,1);return o;}
)";
static ComPtr<ID3DBlob> compile(const char* entry, const char* target)
{
    ComPtr<ID3DBlob> b, e;
    auto hr = D3DCompile(code, strlen(code), "synthetic-dual-source", nullptr, nullptr, entry, target,
                         D3DCOMPILE_ENABLE_STRICTNESS | D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &b, &e);
    if (e)
        printf("%s\n", (char*) e->GetBufferPointer());
    check(hr);
    return b;
}
int main()
{
    try
    {
        ComPtr<IDXGIFactory4> factory;
        check(CreateDXGIFactory1(IID_PPV_ARGS(&factory)));
        ComPtr<ID3D12Device> dev;
        for (UINT i = 0;; i++)
        {
            ComPtr<IDXGIAdapter1> a;
            if (factory->EnumAdapters1(i, &a) == DXGI_ERROR_NOT_FOUND)
                break;
            DXGI_ADAPTER_DESC1 desc {};
            check(a->GetDesc1(&desc));
            if (desc.VendorId == 0x10de && !(desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE))
            {
                check(D3D12CreateDevice(a.Get(), D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&dev)));
                break;
            }
        }
        if (!dev)
            return 2;
        D3D12_FEATURE_DATA_FORMAT_SUPPORT support { DXGI_FORMAT_R16G16B16A16_FLOAT };
        check(dev->CheckFeatureSupport(D3D12_FEATURE_FORMAT_SUPPORT, &support, sizeof(support)));
        if (!(support.Support1 & D3D12_FORMAT_SUPPORT1_BLENDABLE))
            return 3;
        D3D12_COMMAND_QUEUE_DESC qd {};
        qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        ComPtr<ID3D12CommandQueue> queue;
        check(dev->CreateCommandQueue(&qd, IID_PPV_ARGS(&queue)));
        ComPtr<ID3D12CommandAllocator> alloc;
        check(dev->CreateCommandAllocator(qd.Type, IID_PPV_ARGS(&alloc)));
        ComPtr<ID3D12GraphicsCommandList> cmd;
        check(dev->CreateCommandList(0, qd.Type, alloc.Get(), nullptr, IID_PPV_ARGS(&cmd)));
        D3D12_ROOT_PARAMETER param {};
        param.ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        param.Constants = { 0, 0, 8 };
        param.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
        D3D12_ROOT_SIGNATURE_DESC rd {};
        rd.NumParameters = 1;
        rd.pParameters = &param;
        ComPtr<ID3DBlob> rb, re;
        check(D3D12SerializeRootSignature(&rd, D3D_ROOT_SIGNATURE_VERSION_1, &rb, &re));
        ComPtr<ID3D12RootSignature> root;
        check(dev->CreateRootSignature(0, rb->GetBufferPointer(), rb->GetBufferSize(), IID_PPV_ARGS(&root)));
        auto vs = compile("VS", "vs_5_0"), ps = compile("PS", "ps_5_0");
        D3D12_GRAPHICS_PIPELINE_STATE_DESC pd {};
        pd.pRootSignature = root.Get();
        pd.VS = { vs->GetBufferPointer(), vs->GetBufferSize() };
        pd.PS = { ps->GetBufferPointer(), ps->GetBufferSize() };
        pd.SampleMask = UINT_MAX;
        pd.SampleDesc.Count = 1;
        pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        pd.NumRenderTargets = 1;
        pd.RTVFormats[0] = DXGI_FORMAT_R16G16B16A16_FLOAT;
        pd.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
        pd.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
        pd.RasterizerState.DepthClipEnable = TRUE;
        pd.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_ALWAYS;
        auto& blend = pd.BlendState.RenderTarget[0];
        blend.BlendEnable = TRUE;
        blend.SrcBlend = D3D12_BLEND_ONE;
        blend.DestBlend = D3D12_BLEND_SRC1_COLOR;
        blend.BlendOp = D3D12_BLEND_OP_ADD;
        blend.SrcBlendAlpha = D3D12_BLEND_ONE;
        blend.DestBlendAlpha = D3D12_BLEND_INV_SRC_ALPHA;
        blend.BlendOpAlpha = D3D12_BLEND_OP_ADD;
        blend.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
        constexpr unsigned TargetCount = 20;
        ComPtr<ID3D12PipelineState> states[TargetCount];
        const D3D12_BLEND factors[5][2] = { { D3D12_BLEND_ONE, D3D12_BLEND_SRC1_COLOR },
                                            { D3D12_BLEND_ONE, D3D12_BLEND_INV_SRC_ALPHA },
                                            { D3D12_BLEND_ONE, D3D12_BLEND_SRC_ALPHA },
                                            { D3D12_BLEND_ONE, D3D12_BLEND_ONE },
                                            { D3D12_BLEND_SRC_ALPHA, D3D12_BLEND_INV_SRC_ALPHA } };
        const GlassFg::MaterialCapture modes[] = { GlassFg::MaterialCapture::Transmittance,
                                                   GlassFg::MaterialCapture::SourceColor,
                                                   GlassFg::MaterialCapture::Uncovered };
        for (unsigned family = 0; family < 5; family++)
        {
            auto original = pd;
            original.BlendState.RenderTarget[0].SrcBlend = factors[family][0];
            original.BlendState.RenderTarget[0].DestBlend = factors[family][1];
            check(dev->CreateGraphicsPipelineState(&original, IID_PPV_ARGS(&states[family * 4])));
            for (unsigned i = 0; i < 3; i++)
            {
                auto clone = original;
                if (!GlassFg::tryMaterialCaptureBlend(original.BlendState, modes[i], clone.BlendState))
                    return 7;
                check(dev->CreateGraphicsPipelineState(&clone, IID_PPV_ARGS(&states[family * 4 + i + 1])));
            }
        }
        auto unsupported = pd.BlendState;
        unsupported.RenderTarget[0].SrcBlend = D3D12_BLEND_DEST_COLOR;
        auto unchanged = unsupported;
        if (GlassFg::tryMaterialCaptureBlend(unsupported, GlassFg::MaterialCapture::SourceColor, unchanged) ||
            memcmp(&unsupported, &unchanged, sizeof(unchanged)))
            return 8;
        unsupported = pd.BlendState;
        unsupported.RenderTarget[0].BlendOp = D3D12_BLEND_OP_MAX;
        unchanged = unsupported;
        if (GlassFg::tryMaterialCaptureBlend(unsupported, GlassFg::MaterialCapture::SourceColor, unchanged) ||
            memcmp(&unsupported, &unchanged, sizeof(unchanged)))
            return 9;
        unsupported = pd.BlendState;
        unsupported.IndependentBlendEnable = TRUE;
        unchanged = unsupported;
        if (GlassFg::tryMaterialCaptureBlend(unsupported, GlassFg::MaterialCapture::SourceColor, unchanged) ||
            !GlassFg::tryMaterialCaptureBlend(unsupported, GlassFg::MaterialCapture::SourceColor, unchanged, true))
            return 10;
        D3D12_DESCRIPTOR_HEAP_DESC hd {};
        hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
        hd.NumDescriptors = TargetCount;
        ComPtr<ID3D12DescriptorHeap> heap;
        check(dev->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&heap)));
        UINT inc = dev->GetDescriptorHandleIncrementSize(hd.Type);
        auto handle = heap->GetCPUDescriptorHandleForHeapStart();
        D3D12_RESOURCE_DESC td {};
        td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        td.Width = td.Height = 32;
        td.DepthOrArraySize = td.MipLevels = td.SampleDesc.Count = 1;
        td.Format = pd.RTVFormats[0];
        td.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
        D3D12_HEAP_PROPERTIES hp {};
        hp.Type = D3D12_HEAP_TYPE_DEFAULT;
        hp.CreationNodeMask = hp.VisibleNodeMask = 1;
        ComPtr<ID3D12Resource> textures[TargetCount], readbacks[TargetCount];
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp {};
        UINT64 bytes {};
        dev->GetCopyableFootprints(&td, 0, 1, 0, &fp, nullptr, nullptr, &bytes);
        D3D12_VIEWPORT viewport { 0, 0, 32, 32, 0, 1 };
        D3D12_RECT scissor { 0, 0, 32, 32 };
        cmd->SetGraphicsRootSignature(root.Get());
        cmd->RSSetViewports(1, &viewport);
        cmd->RSSetScissorRects(1, &scissor);
        cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        const float constants[2][8] = { { .125f, .25f, .5f, .5f, .5f, .25f, .125f, 1 },
                                        { .25f, .125f, .0625f, .5f, .5f, .5f, .5f, 1 } };
        for (UINT i = 0; i < TargetCount; i++)
        {
            check(dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &td, D3D12_RESOURCE_STATE_RENDER_TARGET,
                                               nullptr, IID_PPV_ARGS(&textures[i])));
            auto rt = handle;
            rt.ptr += i * inc;
            dev->CreateRenderTargetView(textures[i].Get(), nullptr, rt);
            const float scene[] = { .5f, .5f, .5f, 1 }, white[] = { 1, 1, 1, 1 }, black[] = { 0, 0, 0, 1 };
            cmd->ClearRenderTargetView(rt, i % 4 == 0 ? scene : i % 4 == 2 ? black : white, 0, nullptr);
            cmd->OMSetRenderTargets(1, &rt, FALSE, nullptr);
            cmd->SetPipelineState(states[i].Get());
            for (const auto& c : constants)
            {
                cmd->SetGraphicsRoot32BitConstants(0, 8, c, 0);
                cmd->DrawInstanced(3, 1, 0, 0);
            }
            D3D12_RESOURCE_BARRIER barrier {};
            barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            barrier.Transition = { textures[i].Get(), D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
                                   D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE };
            cmd->ResourceBarrier(1, &barrier);
            D3D12_RESOURCE_DESC bd {};
            bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
            bd.Width = bytes;
            bd.Height = bd.DepthOrArraySize = bd.MipLevels = bd.SampleDesc.Count = 1;
            bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
            D3D12_HEAP_PROPERTIES rh = hp;
            rh.Type = D3D12_HEAP_TYPE_READBACK;
            check(dev->CreateCommittedResource(&rh, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                               IID_PPV_ARGS(&readbacks[i])));
            D3D12_TEXTURE_COPY_LOCATION dst {}, source {};
            dst.pResource = readbacks[i].Get();
            dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
            dst.PlacedFootprint = fp;
            source.pResource = textures[i].Get();
            source.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            cmd->CopyTextureRegion(&dst, 0, 0, 0, &source, nullptr);
        }
        check(cmd->Close());
        ID3D12CommandList* lists[] = { cmd.Get() };
        queue->ExecuteCommandLists(1, lists);
        ComPtr<ID3D12Fence> fence;
        check(dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)));
        check(queue->Signal(fence.Get(), 1));
        HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (!event)
            return 4;
        check(fence->SetEventOnCompletion(1, event));
        auto wait = WaitForSingleObject(event, 30000);
        CloseHandle(event);
        if (wait != WAIT_OBJECT_0)
            return 5;
        check(dev->GetDeviceRemovedReason());
        unsigned failures = 0;
        for (UINT i = 0; i < TargetCount; i++)
        {
            void* data = nullptr;
            check(readbacks[i]->Map(0, nullptr, &data));
            for (UINT y = 0; y < 32; y++)
            {
                auto row = (const unsigned short*) ((const char*) data + fp.Offset + y * fp.Footprint.RowPitch);
                for (UINT x = 0; x < 32; x++)
                    for (UINT c = 0; c < 4; c++)
                    {
                        unsigned h = row[x * 4 + c];
                        float actual = std::ldexp(float(h & 1023u) + ((h & 0x7c00u) ? 1024.f : 0.f),
                                                  int((h >> 10) & 31u) - 25 + ((h & 0x7c00u) ? 0 : 1));
                        if (h & 0x8000u)
                            actual = -actual;
                        unsigned mode = i % 4, family = i / 4;
                        float wanted = mode == 0 ? .5f : mode == 2 ? 0.f : 1.f;
                        if (c == 3)
                            wanted = 1;
                        else if (x >= 8)
                        {
                            if (mode == 3)
                                wanted = 0;
                            else
                                for (unsigned layer = 0; layer < 2; layer++)
                                {
                                    float factor = family == 0   ? (x >= 16 ? 1.f : constants[layer][4 + c])
                                                   : family == 3 ? 1.f
                                                                 : .5f;
                                    float source = family == 4 ? .5f : 1.f;
                                    wanted = (mode == 1 ? 0.f : constants[layer][c] * source) + factor * wanted;
                                }
                        }
                        if (!std::isfinite(actual) || std::abs(actual - wanted) > 1e-6f)
                            ++failures;
                    }
            }
            readbacks[i]->Unmap(0, nullptr);
        }
        printf("MATERIAL_CAPTURE pixels=20480 failures=%u format=RGBA16F blend_families=5 "
               "rejected_destination_source_and_max=1 discarded_strip=1 unit_transmission_nonzero_source=1 "
               "game_attachment=0\n",
               failures);
        return failures ? 6 : 0;
    }
    catch (const std::exception& e)
    {
        printf("FAILED %s\n", e.what());
        return 1;
    }
}
