#pragma once
#include <d3d12.h>
#include <cstdint>
#include <memory>

namespace GlassFg
{
// A CPU observation copied when OM binds the descriptor. No resource pointer
// in this metadata licenses a later COM call or GPU access.
struct GeometryView
{
    uint64_t handle = 0, heap = 0, revision = 0, resource = 0, address = 0;
    uint32_t kind = 0; // 1 RTV, 2 DSV
    bool defaultDescriptor = false, nullResource = false;
    D3D12_RESOURCE_DESC allocation {};
    D3D12_RENDER_TARGET_VIEW_DESC rtv {};
    D3D12_DEPTH_STENCIL_VIEW_DESC dsv {};
};
bool StartGeometryViews(ID3D12Device*) noexcept;
struct GeometryViewStats
{
    bool active = false, healthy = false;
    uint64_t heaps = 0, writes = 0, copies = 0, lookups = 0, misses = 0;
};
GeometryViewStats GetGeometryViewStats() noexcept;
std::shared_ptr<const GeometryView> FindGeometryView(D3D12_CPU_DESCRIPTOR_HANDLE, uint32_t kind) noexcept;
} // namespace GlassFg
