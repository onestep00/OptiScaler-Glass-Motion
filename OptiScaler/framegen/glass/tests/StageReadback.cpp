// Independent capture-coordinate and state-restoration checks. No game hooks.
#include "StageReadback.h"
#include <dxgi1_4.h>
#include <d3d12sdklayers.h>
#include <cstdio>
#include <stdexcept>
#include <array>
using namespace stage_capture;
static void require(bool ok, const char* message) { if (!ok) throw std::runtime_error(message); }
static void check(HRESULT hr) { require(SUCCEEDED(hr), "D3D12 call failed"); }
static ComPtr<ID3D12Resource> buffer(ID3D12Device* d, UINT64 size, D3D12_HEAP_TYPE type)
{
    D3D12_RESOURCE_DESC r{};
    r.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER; r.Width=size;
    r.Height=r.SampleDesc.Count=1;r.DepthOrArraySize=r.MipLevels=1;
    r.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    D3D12_HEAP_PROPERTIES h{};h.Type=type;h.CreationNodeMask=h.VisibleNodeMask=1;
    ComPtr<ID3D12Resource> result;
    check(d->CreateCommittedResource(&h,D3D12_HEAP_FLAG_NONE,&r,
          type==D3D12_HEAP_TYPE_UPLOAD?D3D12_RESOURCE_STATE_GENERIC_READ:D3D12_RESOURCE_STATE_COPY_DEST,
          nullptr,IID_PPV_ARGS(&result)));
    return result;
}
struct Case { UINT width,height; Region region; DXGI_FORMAT format; };
int main()
{
    try
    {
        ComPtr<ID3D12Debug> debug;
        const bool debugAvailable=SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug)));
        if(debugAvailable)debug->EnableDebugLayer();
        ComPtr<ID3D12Device>d; check(D3D12CreateDevice(nullptr,D3D_FEATURE_LEVEL_12_0,IID_PPV_ARGS(&d)));
        ComPtr<ID3D12InfoQueue> info;if(debugAvailable)check(d.As(&info));
        const Case cases[]={
            {257,145,{3,5,193,109},DXGI_FORMAT_R16G16B16A16_FLOAT},
            {1920,1080,{0,0,1920,1080},DXGI_FORMAT_R16G16B16A16_FLOAT},
            {3847,2169,{7,9,3840,2160},DXGI_FORMAT_R8G8B8A8_UNORM},
            {811,457,{9,7,800,450},DXGI_FORMAT_R32G32B32A32_FLOAT},
            {13,11,{12,10,1,1},DXGI_FORMAT_R10G10B10A2_UNORM}};
        UINT64 totalPixels=0,totalBytes=0;
        unsigned rejections=0;
        for (const auto& c:cases)
        {
            D3D12_COMMAND_QUEUE_DESC qd{};qd.Type=D3D12_COMMAND_LIST_TYPE_DIRECT;
            ComPtr<ID3D12CommandQueue>q;check(d->CreateCommandQueue(&qd,IID_PPV_ARGS(&q)));
            ComPtr<ID3D12CommandAllocator>a;check(d->CreateCommandAllocator(qd.Type,IID_PPV_ARGS(&a)));
            ComPtr<ID3D12GraphicsCommandList>cmd;check(d->CreateCommandList(0,qd.Type,a.Get(),nullptr,IID_PPV_ARGS(&cmd)));
            D3D12_RESOURCE_DESC td{};td.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D;
            td.Width=c.width;td.Height=c.height;td.DepthOrArraySize=td.MipLevels=1;td.SampleDesc.Count=1;
            td.Format=c.format;td.Flags=D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
            D3D12_HEAP_PROPERTIES hp{};hp.Type=D3D12_HEAP_TYPE_DEFAULT;hp.CreationNodeMask=hp.VisibleNodeMask=1;
            ComPtr<ID3D12Resource>source;check(d->CreateCommittedResource(&hp,D3D12_HEAP_FLAG_NONE,&td,
                D3D12_RESOURCE_STATE_COPY_DEST,nullptr,IID_PPV_ARGS(&source)));
            Layout full{};require(Describe(d.Get(),td,{0,0,c.width,c.height},128ull<<20,full),"full layout");
            auto upload=buffer(d.Get(),full.totalBytes,D3D12_HEAP_TYPE_UPLOAD);
            const UINT pixelBytes=PixelBytes(c.format);
            void* mapped=nullptr;D3D12_RANGE empty{};check(upload->Map(0,&empty,&mapped));
            auto bytes=static_cast<std::uint8_t*>(mapped);
            std::memset(bytes,0xcc,SIZE_T(full.totalBytes));
            std::vector<std::uint8_t> expected;
            for(UINT y=0;y<c.height;y++)for(UINT x=0;x<c.width;x++)for(UINT k=0;k<pixelBytes;k++)
            {
                auto value=std::uint8_t((UINT64(x)*73+UINT64(y)*37+k*11+(x^y)*3)&255);
                bytes[full.footprint.Offset+SIZE_T(y)*full.footprint.Footprint.RowPitch+SIZE_T(x)*pixelBytes+k]=value;
                if(x>=c.region.x&&x<c.region.x+c.region.width&&y>=c.region.y&&y<c.region.y+c.region.height)
                    expected.push_back(value);
            }
            upload->Unmap(0,nullptr);
            D3D12_TEXTURE_COPY_LOCATION from{},to{};from.pResource=upload.Get();from.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;from.PlacedFootprint=full.footprint;
            to.pResource=source.Get();to.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            cmd->CopyTextureRegion(&to,0,0,0,&from,nullptr);
            D3D12_RESOURCE_BARRIER b{};b.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            b.Transition={source.Get(),D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_RENDER_TARGET};cmd->ResourceBarrier(1,&b);
            Readback capture,second;
            require(capture.initialize(source.Get(),c.region,128ull<<20),"initialize");
            require(!capture.record(cmd.Get(),D3D12_RESOURCE_STATE_COMMON),"unknown promotion admitted");++rejections;
            require(capture.record(cmd.Get(),D3D12_RESOURCE_STATE_RENDER_TARGET),"record");
            require(!capture.record(cmd.Get(),D3D12_RESOURCE_STATE_RENDER_TARGET),"duplicate record");++rejections;
            require(second.initialize(source.Get(),c.region,128ull<<20)&&second.record(cmd.Get(),D3D12_RESOURCE_STATE_RENDER_TARGET),"restored-state readback");
            std::vector<std::uint8_t> actual;
            require(!capture.read(actual),"unsubmitted read");
            check(cmd->Close());
            ComPtr<ID3D12Fence>gate,done;check(d->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&gate)));check(d->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&done)));
            check(q->Wait(gate.Get(),1));ID3D12CommandList* list[]={cmd.Get()};q->ExecuteCommandLists(1,list);check(q->Signal(done.Get(),1));
            require(capture.submitted(done.Get(),1)&&second.submitted(done.Get(),1),"completion binding");
            require(!capture.discardedWithoutSubmission(),"submitted treated as unsubmitted");++rejections;
            require(!capture.read(actual),"unfinished read");
            // Reset with a fresh allocator must not make pending reads available.
            ComPtr<ID3D12CommandAllocator>fresh;check(d->CreateCommandAllocator(qd.Type,IID_PPV_ARGS(&fresh)));
            check(cmd->Reset(fresh.Get(),nullptr));capture.discarded();
            require(!capture.read(actual),"discard incorrectly implied completion");
            check(cmd->Close());check(gate->Signal(1));
            HANDLE event=CreateEventW(nullptr,FALSE,FALSE,nullptr);require(event!=nullptr,"event");check(done->SetEventOnCompletion(1,event));
            const auto waited=WaitForSingleObject(event,10000);CloseHandle(event);require(waited==WAIT_OBJECT_0,"completion timeout");
            require(capture.read(actual)&&actual==expected,"first copy mismatch");
            require(!second.read(actual),"completion without discard proof admitted");
            second.discarded();
            require(second.read(actual)&&actual==expected,"restoration or second copy mismatch");
            auto bad=td;bad.MipLevels=2;Layout rejected{};
            require(!Describe(d.Get(),bad,c.region,128ull<<20,rejected),"mip admission");++rejections;
            bad=td;bad.DepthOrArraySize=2;require(!Describe(d.Get(),bad,c.region,128ull<<20,rejected),"array admission");++rejections;
            bad=td;bad.SampleDesc.Count=4;require(!Describe(d.Get(),bad,c.region,128ull<<20,rejected),"MSAA admission");++rejections;
            require(!Describe(d.Get(),td,{c.width,0,1,1},128ull<<20,rejected),"out of bounds");++rejections;
            require(!Describe(d.Get(),td,{UINT_MAX,0,UINT_MAX,1},128ull<<20,rejected),"overflow bounds");++rejections;
            require(!Describe(d.Get(),td,c.region,1,rejected),"budget admission");++rejections;
            totalPixels+=UINT64(c.region.width)*c.region.height*2;totalBytes+=expected.size()*2;
            printf("CASE allocation=%ux%u origin=%u,%u extent=%ux%u format=%u exact=1 restored_state=1 gated_completion=1\n",c.width,c.height,c.region.x,c.region.y,c.region.width,c.region.height,c.format);
        }
        unsigned errors=0;
        if(info)for(UINT64 i=0;i<info->GetNumStoredMessages();i++)
        {
            SIZE_T bytes=0;check(info->GetMessage(i,nullptr,&bytes));std::vector<std::uint8_t> storage(bytes);
            auto message=reinterpret_cast<D3D12_MESSAGE*>(storage.data());check(info->GetMessage(i,message,&bytes));
            if(message->Severity<=D3D12_MESSAGE_SEVERITY_ERROR){++errors;printf("DEBUG %s\n",message->pDescription);}
        }
        require(errors==0,"debug layer errors");
        printf("{\"cases\":5,\"exactPixels\":%llu,\"exactBytes\":%llu,\"rejections\":%u,\"failures\":0,\"gameAttachment\":false,\"debugLayer\":%s}\n",totalPixels,totalBytes,rejections,debugAvailable?"true":"false");
        return 0;
    }
    catch(const std::exception&e){printf("FAILED %s\n",e.what());return 1;}
}
