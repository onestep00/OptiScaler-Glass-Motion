#pragma once
#include <d3d12.h>
#include <array>
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>

namespace GlassFg
{
struct GeometryCamera
{
    std::array<std::int32_t, 3> fixedOrigin {};
    std::array<float, 16> relativeWorldToClip {};
    std::array<float, 2> jitterUv {}, renderExtent {};
};

// This is a layout decoder, not resource/pass identification. The game adapter
// must prove the executable layout, original VS b1 binding, current recording,
// upload source stability, and final FG frame association before admission.
// The matrix matches the original VS's rows 28..31 after its fixed origin (38).
inline bool DecodeCyberpunkCamera(std::span<const std::byte> bytes, GeometryCamera& result)
{
    if (bytes.size() != 848)
        return false;
    GeometryCamera camera;
    std::memcpy(camera.fixedOrigin.data(), bytes.data() + 38 * 16, 12);
    std::memcpy(camera.relativeWorldToClip.data(), bytes.data() + 28 * 16, 64);
    std::memcpy(camera.renderExtent.data(), bytes.data() + 47 * 16, 8);
    std::array<float, 2> jitter {};
    std::memcpy(jitter.data(), bytes.data() + 51 * 16, 8);
    for (auto value : camera.relativeWorldToClip)
        if (!std::isfinite(value))
            return false;
    for (UINT i = 0; i < 2; ++i)
        if (!std::isfinite(camera.renderExtent[i]) || camera.renderExtent[i] < 1 || camera.renderExtent[i] > 32768 ||
            std::floor(camera.renderExtent[i]) != camera.renderExtent[i] || !std::isfinite(jitter[i]) ||
            std::abs(jitter[i]) > 1)
            return false;
    camera.jitterUv = { jitter[0] * .5f, jitter[1] * -.5f };
    result = camera;
    return true;
}

enum class BoundProjection
{
    Invalid,
    Outside,
    Visible
};
// Bounds only size an owned storage rectangle; they never define the actual
// contour or overwrite a pixel. Original material coverage defines the contour.
// A producer must flag any coverage outside this rectangle and reject that
// object's capture, since deformation/late updates can outgrow engine bounds.
inline BoundProjection ProjectGeometryBounds(const GeometryCamera& camera, const std::array<float, 6>& bounds,
                                             const D3D12_VIEWPORT& viewport, UINT padding, D3D12_RECT& result)
{
    if (!std::isfinite(viewport.TopLeftX) || !std::isfinite(viewport.TopLeftY) || !std::isfinite(viewport.Width) ||
        !std::isfinite(viewport.Height) || viewport.TopLeftX < 0 || viewport.TopLeftY < 0 || viewport.Width < 1 ||
        viewport.Height < 1 || viewport.TopLeftX + viewport.Width > 32768 ||
        viewport.TopLeftY + viewport.Height > 32768 || padding > 64 || viewport.MinDepth != 0 || viewport.MaxDepth != 1)
        return BoundProjection::Invalid;
    for (UINT i = 0; i < 3; ++i)
        if (!std::isfinite(bounds[i]) || !std::isfinite(bounds[i + 3]) || bounds[i] > bounds[i + 3])
            return BoundProjection::Invalid;
    std::array<double, 2> low { 1.e300, 1.e300 }, high { -1.e300, -1.e300 };
    UINT behind = 0;
    for (UINT corner = 0; corner < 8; ++corner)
    {
        double position[4] { 0, 0, 0, 1 }, clip[4] {};
        for (UINT axis = 0; axis < 3; ++axis)
            position[axis] =
                double(bounds[axis + ((corner >> axis) & 1) * 3]) - double(camera.fixedOrigin[axis]) / 131072;
        for (UINT row = 0; row < 4; ++row)
            for (UINT col = 0; col < 4; ++col)
                clip[row] += double(camera.relativeWorldToClip[row * 4 + col]) * position[col];
        for (auto v : clip)
            if (!std::isfinite(v))
                return BoundProjection::Invalid;
        if (clip[3] <= 1.e-6)
        {
            ++behind;
            continue;
        }
        const double pixel[] = { viewport.TopLeftX + (clip[0] / clip[3] * .5 + .5) * viewport.Width,
                                 viewport.TopLeftY + (clip[1] / clip[3] * -.5 + .5) * viewport.Height };
        for (UINT axis = 0; axis < 2; ++axis)
        {
            low[axis] = std::min(low[axis], pixel[axis]);
            high[axis] = std::max(high[axis], pixel[axis]);
        }
    }
    if (behind == 8)
        return BoundProjection::Outside;
    const double origin[] = { viewport.TopLeftX, viewport.TopLeftY };
    const double end[] = { double(viewport.TopLeftX) + viewport.Width, double(viewport.TopLeftY) + viewport.Height };
    LONG minimum[2], maximum[2];
    for (UINT axis = 0; axis < 2; ++axis)
    {
        // A box crossing the eye plane needs clipping; full viewport is the
        // conservative bounded fallback, subject to the caller's memory budget.
        minimum[axis] = LONG(std::clamp(std::floor(behind ? origin[axis] : low[axis] - padding), 0., 32768.));
        maximum[axis] = LONG(std::clamp(std::ceil(behind ? end[axis] : high[axis] + padding), 0., 32768.));
        minimum[axis] = std::max(minimum[axis], LONG(std::floor(origin[axis])));
        maximum[axis] = std::min(maximum[axis], LONG(std::ceil(end[axis])));
        if (maximum[axis] <= minimum[axis])
            return BoundProjection::Outside;
    }
    result = { minimum[0], minimum[1], maximum[0], maximum[1] };
    return BoundProjection::Visible;
}
} // namespace GlassFg
