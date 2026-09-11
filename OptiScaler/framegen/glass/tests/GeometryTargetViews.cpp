#include "GeometryTestDevice.h"
#include "../GeometryViews.h"
#include "../GeometryCommands.h"
#include "../CommandLifetime.h"
#include "../ExperimentDrawBridge.h"
int main()
{
    try
    {
        Device g;
        D3D12_DESCRIPTOR_HEAP_DESC hd { D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 8, D3D12_DESCRIPTOR_HEAP_FLAG_NONE, 0 };
        ComPtr<ID3D12DescriptorHeap> beforeObservation;
        check(g.d->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&beforeObservation)));
        D3D12_RENDER_TARGET_VIEW_DESC nullView {};
        nullView.Format = DXGI_FORMAT_R8G8B8A8_UNORM; nullView.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
        const auto unknownSource = beforeObservation->GetCPUDescriptorHandleForHeapStart();
        g.d->CreateRenderTargetView(nullptr, &nullView, unknownSource);
        require(GlassFg::StartGeometryViews(g.d.Get()), "View observer installation");
        require(GlassFg::StartGeometryCommands(g.d.Get()), "Command observer installation");
        ComPtr<ID3D12DescriptorHeap> heap, depthHeap;
        check(g.d->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&heap)));
        hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
        check(g.d->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&depthHeap)));
        const auto increment = g.d->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
        std::array<D3D12_CPU_DESCRIPTOR_HANDLE, 8> handle;
        for (unsigned i = 0; i < handle.size(); ++i) handle[i] = { heap->GetCPUDescriptorHandleForHeapStart().ptr + i * increment };
        D3D12_RESOURCE_DESC desc {};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D; desc.Width = 64; desc.Height = 32;
        desc.DepthOrArraySize = 3; desc.MipLevels = 3; desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        desc.SampleDesc.Count = 1; desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
        D3D12_HEAP_PROPERTIES properties {}; properties.Type = D3D12_HEAP_TYPE_DEFAULT;
        ComPtr<ID3D12Resource> first, second, depth;
        check(g.d->CreateCommittedResource(&properties, D3D12_HEAP_FLAG_NONE, &desc,
                                           D3D12_RESOURCE_STATE_RENDER_TARGET, nullptr, IID_PPV_ARGS(&first)));
        check(g.d->CreateCommittedResource(&properties, D3D12_HEAP_FLAG_NONE, &desc,
                                           D3D12_RESOURCE_STATE_RENDER_TARGET, nullptr, IID_PPV_ARGS(&second)));
        desc.Format = DXGI_FORMAT_D32_FLOAT; desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
        check(g.d->CreateCommittedResource(&properties, D3D12_HEAP_FLAG_NONE, &desc,
                                           D3D12_RESOURCE_STATE_DEPTH_WRITE, nullptr, IID_PPV_ARGS(&depth)));
        D3D12_RENDER_TARGET_VIEW_DESC slice {};
        slice.Format = DXGI_FORMAT_R8G8B8A8_UNORM; slice.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2DARRAY;
        slice.Texture2DArray = { 2, 1, 1, 0 };
        g.d->CreateRenderTargetView(first.Get(), nullptr, handle[0]);
        g.d->CreateRenderTargetView(first.Get(), &slice, handle[1]);
        g.d->CreateRenderTargetView(second.Get(), nullptr, handle[2]);
        g.d->CreateRenderTargetView(nullptr, &nullView, handle[3]);
        const auto original = GlassFg::FindGeometryView(handle[1], 1);
        const auto other = GlassFg::FindGeometryView(handle[2], 1);
        const auto defaultView = GlassFg::FindGeometryView(handle[0], 1);
        const auto nullObserved = GlassFg::FindGeometryView(handle[3], 1);
        require(original && other && defaultView && nullObserved && original->resource == defaultView->resource &&
                    original->resource != other->resource && original->rtv.Texture2DArray.MipSlice == 2 &&
                    original->rtv.Texture2DArray.FirstArraySlice == 1 && original->allocation.MipLevels == 3 &&
                    defaultView->defaultDescriptor && nullObserved->nullResource && !nullObserved->resource,
                "Resource/explicit slice/default/null metadata");
        g.d->CopyDescriptorsSimple(1, handle[4], handle[1], D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
        const auto copied = GlassFg::FindGeometryView(handle[4], 1);
        require(copied && copied->resource == original->resource && copied->revision != original->revision &&
                    copied->handle == handle[4].ptr && copied->rtv.Texture2DArray.MipSlice == 2,
                "Simple descriptor copy provenance");
        const D3D12_CPU_DESCRIPTOR_HANDLE sources[] { handle[1], handle[2] };
        const UINT two = 2;
        g.d->CopyDescriptors(1, &handle[5], &two, 2, sources, nullptr, D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
        require(GlassFg::FindGeometryView(handle[5], 1)->resource == original->resource &&
                    GlassFg::FindGeometryView(handle[6], 1)->resource == other->resource,
                "Ranged descriptor copy provenance");
        const D3D12_CPU_DESCRIPTOR_HANDLE destinations[] { handle[4], handle[7] };
        g.d->CopyDescriptors(2, destinations, nullptr, 2, sources, nullptr, D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
        require(GlassFg::FindGeometryView(handle[7], 1)->resource == other->resource, "Implicit one-descriptor ranges");
        g.d->CopyDescriptorsSimple(1, handle[7], unknownSource, D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
        require(!GlassFg::FindGeometryView(handle[7], 1), "Unknown copy source retained stale destination");
        D3D12_DEPTH_STENCIL_VIEW_DESC ds {};
        ds.Format = DXGI_FORMAT_D32_FLOAT; ds.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2DARRAY;
        ds.Texture2DArray = { 2, 1, 1 }; ds.Flags = D3D12_DSV_FLAG_READ_ONLY_DEPTH;
        const auto depthHandle = depthHeap->GetCPUDescriptorHandleForHeapStart();
        g.d->CreateDepthStencilView(depth.Get(), &ds, depthHandle);
        g.begin();
        g.c->OMSetRenderTargets(1, &handle[4], FALSE, &depthHandle);
        const auto* bound = GlassFg::ReadGeometryRasterState(g.c.Get());
        require(bound && bound->targetViews[0] && bound->depthView &&
                    bound->targetViews[0]->resource == original->resource &&
                    bound->depthView->dsv.Flags == D3D12_DSV_FLAG_READ_ONLY_DEPTH &&
                    bound->depthView->dsv.Texture2DArray.FirstArraySlice == 1,
                "Original OM binding snapshot");
        const auto boundCopy = *bound;
        GlassExperimentTarget abi {}; abi.size = sizeof(abi);
        require(GlassFg::ExperimentTargetAt(bound, 0, &abi) == 1 && abi.resource == original->resource &&
                    abi.descriptorBytes == sizeof(slice) && !memcmp(abi.descriptor, &slice, sizeof(slice)),
                "Explicit view descriptor changed across the borrowed ABI");
        require(GlassFg::ExperimentTargetAt(bound, 8, &abi) == 1 && abi.descriptorBytes == sizeof(ds) &&
                    !memcmp(abi.descriptor, &ds, sizeof(ds)) && GlassFg::ExperimentTargetAt(bound, 9, &abi) == 0,
                "Depth descriptor or ABI bounds");
        abi.size = 0;
        require(GlassFg::ExperimentTargetAt(bound, 0, &abi) == 0, "Wrong-sized target ABI admitted");
        g.d->CreateRenderTargetView(second.Get(), nullptr, handle[4]);
        require(GlassFg::FindGeometryView(handle[4], 1)->resource == other->resource &&
                    GlassFg::ReadGeometryRasterState(g.c.Get())->targetViews[0]->resource == original->resource,
                "Descriptor overwrite changed an earlier OM binding");
        g.finish(); g.begin(); g.finish(); // GPU completion and discard for fixture resources.
        heap.Reset(); depthHeap.Reset();
        require(!GlassFg::FindGeometryView(handle[1], 1) && !GlassFg::FindGeometryView(depthHandle, 2) &&
                    boundCopy.targetViews[0]->resource == original->resource,
                "Destroyed heap/stable numeric snapshot");
        hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
        check(g.d->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&heap)));
        const auto fresh = heap->GetCPUDescriptorHandleForHeapStart();
        require(!GlassFg::FindGeometryView(fresh, 1), "New heap inherited old descriptors");
        GlassFg::CommandLifetime life;
        require(life.attach(first.Get()), "Resource lifetime notification");
        first.Reset();
        require(life.wasDestroyed(), "Metadata registry retained a game resource");
        puts("PASS target_view_hooks=1 resource_identity=1 explicit_subresources=1 default_and_null=1 copies=1 "
             "om_snapshot_survives_overwrite=1 heap_destruction=1 resource_not_retained=1 game_hooks=0");
        return 0;
    }
    catch (const std::exception& error) { fprintf(stderr, "%s\n", error.what()); return 1; }
}
