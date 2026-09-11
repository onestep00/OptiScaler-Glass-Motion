#pragma once
#include <d3d12.h>
#include <array>
#include <cmath>

namespace GlassFg
{
// Actual API state for this recording. Handles identify bindings only; the
// capture owner must resolve and retain resources through descriptor provenance.
struct GeometryRasterState
{
    D3D12_VIEWPORT viewport {};
    D3D12_RECT scissor {};
    std::array<D3D12_CPU_DESCRIPTOR_HANDLE, 8> targets {};
    D3D12_CPU_DESCRIPTOR_HANDLE depth {};
    UINT targetCount = 0;
    bool viewportKnown = false, scissorKnown = false, targetsKnown = false;
    bool predicate = false, renderPass = false, unknown = false;

    void viewports(UINT count, const D3D12_VIEWPORT* value)
    {
        viewportKnown = count == 1 && value;
        if (viewportKnown)
            viewport = *value;
    }
    void scissors(UINT count, const D3D12_RECT* value)
    {
        scissorKnown = count == 1 && value;
        if (scissorKnown)
            scissor = *value;
    }
    void renderTargets(UINT count, const D3D12_CPU_DESCRIPTOR_HANDLE* values, BOOL contiguous,
                       const D3D12_CPU_DESCRIPTOR_HANDLE* dsv, UINT increment)
    {
        targetsKnown = count <= targets.size() && (!count || values) && (!contiguous || increment);
        targetCount = 0;
        depth = {};
        if (!targetsKnown)
            return;
        targetCount = count;
        if (dsv)
            depth = *dsv;
        for (UINT i = 0; i < count; ++i)
            targets[i] = contiguous ? D3D12_CPU_DESCRIPTOR_HANDLE { values[0].ptr + SIZE_T(i) * increment } : values[i];
    }
    bool usable() const
    {
        return !unknown && !predicate && !renderPass && viewportKnown && scissorKnown && targetsKnown && targetCount &&
               std::isfinite(viewport.TopLeftX) && std::isfinite(viewport.TopLeftY) &&
               std::isfinite(viewport.Width) && std::isfinite(viewport.Height) && viewport.Width > 0 &&
               viewport.Height > 0 && std::isfinite(viewport.MinDepth) && std::isfinite(viewport.MaxDepth) &&
               viewport.MinDepth >= 0 && viewport.MaxDepth <= 1 && viewport.MinDepth <= viewport.MaxDepth &&
               scissor.left < scissor.right && scissor.top < scissor.bottom;
    }
};
} // namespace GlassFg
