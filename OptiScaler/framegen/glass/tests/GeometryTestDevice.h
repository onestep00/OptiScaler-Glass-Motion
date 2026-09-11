#pragma once
// Independent GPU experiment. Never obtains or attaches to a game device.
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <wrl/client.h>
#include <filesystem>
#include <fstream>
#include <vector>
#include <array>
#include <cstring>
#include <cstdio>
#include <stdexcept>
#include <cstdint>
#include <cmath>
#include <limits>
#include "../DxilVertexHistory.h"
#include "../GeometryPipeline.h"
#include "../GraphicsRootBindings.h"
#include "../CyberpunkCamera.h"
using Microsoft::WRL::ComPtr;
static void check(HRESULT h)
{
    if (FAILED(h))
        throw std::runtime_error("HRESULT " + std::to_string((unsigned) h));
}
static void require(bool v, const char* s)
{
    if (!v)
        throw std::runtime_error(s);
}
static std::vector<char> read(const std::filesystem::path& p)
{
    std::ifstream f(p, std::ios::binary);
    std::vector<char> b((size_t) std::filesystem::file_size(p));
    require(bool(f.read(b.data(), b.size())), "read");
    return b;
}
struct Device
{
    ComPtr<ID3D12Device> d;
    ComPtr<ID3D12CommandQueue> q;
    ComPtr<ID3D12CommandAllocator> a;
    ComPtr<ID3D12GraphicsCommandList> c;
    ComPtr<ID3D12Fence> f;
    HANDLE event {};
    UINT64 value {};
    Device()
    {
        ComPtr<IDXGIFactory4> factory;
        check(CreateDXGIFactory1(IID_PPV_ARGS(&factory)));
        for (UINT i = 0; !d; ++i)
        {
            ComPtr<IDXGIAdapter1> adapter;
            if (factory->EnumAdapters1(i, &adapter) == DXGI_ERROR_NOT_FOUND)
                break;
            DXGI_ADAPTER_DESC1 desc {};
            check(adapter->GetDesc1(&desc));
            if (desc.VendorId == 0x10de && !(desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE))
                check(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&d)));
        }
        require(bool(d), "NVIDIA adapter missing");
        D3D12_COMMAND_QUEUE_DESC qd {};
        qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        check(d->CreateCommandQueue(&qd, IID_PPV_ARGS(&q)));
        check(d->CreateCommandAllocator(qd.Type, IID_PPV_ARGS(&a)));
        check(d->CreateCommandList(0, qd.Type, a.Get(), nullptr, IID_PPV_ARGS(&c)));
        check(c->Close());
        check(d->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&f)));
        event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        require(event != nullptr, "event");
    }
    ~Device()
    {
        if (event)
            CloseHandle(event);
    }
    void begin()
    {
        check(a->Reset());
        check(c->Reset(a.Get(), nullptr));
    }
    void finish()
    {
        check(c->Close());
        ID3D12CommandList* lists[] = { c.Get() };
        q->ExecuteCommandLists(1, lists);
        check(q->Signal(f.Get(), ++value));
        check(f->SetEventOnCompletion(value, event));
        require(WaitForSingleObject(event, 30000) == WAIT_OBJECT_0, "GPU timeout");
        check(d->GetDeviceRemovedReason());
    }
    ComPtr<ID3D12Resource> buffer(UINT64 n, D3D12_HEAP_TYPE heap, D3D12_RESOURCE_STATES state,
                                  D3D12_RESOURCE_FLAGS flags = D3D12_RESOURCE_FLAG_NONE)
    {
        D3D12_HEAP_PROPERTIES hp {};
        hp.Type = heap;
        D3D12_RESOURCE_DESC rd {};
        rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        rd.Width = n;
        rd.Height = 1;
        rd.DepthOrArraySize = 1;
        rd.MipLevels = 1;
        rd.SampleDesc.Count = 1;
        rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        rd.Flags = flags;
        ComPtr<ID3D12Resource> r;
        check(d->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, state, nullptr, IID_PPV_ARGS(&r)));
        return r;
    }
    void barrier(ID3D12Resource* r, D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after)
    {
        D3D12_RESOURCE_BARRIER b {};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition = { r, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, before, after };
        c->ResourceBarrier(1, &b);
    }
};
struct Vertex
{
    float x, y, z, u, v;
};
struct Clip
{
    float xyzw[4];
};
struct Record
{
    float current[4], previous[4], valid;
};
struct History
{
    float clip[4];
    uint32_t frame, generation, unused[2];
};
struct CaptureRecord
{
    float motion[2], depth;
    uint32_t frame;
    float transmission[3];
    uint32_t reserved;
};
static_assert(sizeof(CaptureRecord) == 32);
static void upload(ID3D12Resource* r, const void* p, size_t n)
{
    void* m;
    D3D12_RANGE noRead { 0, 0 };
    check(r->Map(0, &noRead, &m));
    memcpy(m, p, n);
    D3D12_RANGE written { 0, n };
    r->Unmap(0, &written);
}
