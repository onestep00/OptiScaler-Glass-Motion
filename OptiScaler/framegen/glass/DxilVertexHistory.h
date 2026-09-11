#pragma once
#include <string>
#include <string_view>
#include <cstdint>
#include <cmath>
#include "GeometryInstance.h"

namespace GlassFg
{
struct VertexHistoryConstants
{
    std::uint32_t base, vertices, vertexOrigin, instanceOrigin;
    std::uint32_t instances, generation, frame, previousFrame;

    bool valid(std::uint64_t capacityVertices) const
    {
        return capacityVertices && capacityVertices <= UINT32_MAX / 32 && vertices && instances && generation &&
               frame && previousFrame != frame &&
               std::uint64_t(base) + std::uint64_t(vertices) * instances <= capacityVertices &&
               std::uint64_t(vertexOrigin) + vertices <= (std::uint64_t(1) << 32) &&
               std::uint64_t(instanceOrigin) + instances <= (std::uint64_t(1) << 32);
    }
};
static_assert(sizeof(VertexHistoryConstants) == 32);
struct VertexHistoryShader
{
    std::string assembly;
    std::string error;
    unsigned previousRegister = 0;
    unsigned missingRegister = 0;
    explicit operator bool() const { return !assembly.empty(); }
};

// Rewrites DXC disassembly, not executable/driver instructions. The caller must
// assemble AND validate the result, extend the root signature, supply bounded
// history allocations, and prove stable object/topology/frame identity.
// Original vertex outputs remain unchanged. New varyings contain the previous
// actual clip position and missing-history flag (zero means valid). Interpolated
// zero stays exact; interpolating one introduced rounding-induced coverage holes.
// Neither varying is an object-identity estimator.
// Validate VertexHistoryConstants against BOTH bound history buffers first.
// Unsupported shaders return an error and must keep their original pipeline.
VertexHistoryShader RewriteVertexHistory(std::string_view disassembly,
                                         GeometryLayout layout = GeometryLayout::Contiguous);

enum class MaterialSource
{
    Zero,
    One,
    Alpha
};
enum class MaterialDestination
{
    Zero,
    One,
    Alpha,
    OneMinusAlpha,
    SecondSourceRgb
};
enum class MaterialMotionTarget
{
    SeparateTarget,
    OriginalColorAndCapture,
    // Diagnostic object coverage only. No motion is produced or implied.
    OriginalColorAndCoverage,
    // Diagnostic: also record same-draw surviving/contributing pixels before
    // object mapping. The two reference bit regions use capture CB base/reserved.
    OriginalColorAndCoverageAudit
};

struct MaterialCaptureConstants
{
    float viewportX, viewportY, inverseWidth, inverseHeight;
    float jitterDeltaX, jitterDeltaY;
    std::uint32_t frame, reverseDepth;
    std::uint32_t left, top, width, height;
    std::uint32_t base, stride, capacity, reserved;
    bool valid(std::uint64_t allocatedCapacity) const
    {
        return frame && width && height && stride >= width && capacity && capacity <= UINT32_MAX / 32 &&
               capacity <= allocatedCapacity && std::isfinite(viewportX) && std::isfinite(viewportY) &&
               std::isfinite(inverseWidth) && std::isfinite(inverseHeight) && inverseWidth > 0 && inverseHeight > 0 &&
               std::isfinite(jitterDeltaX) && std::isfinite(jitterDeltaY) &&
               std::uint64_t(base) + std::uint64_t(height - 1) * stride + width <= capacity &&
               std::uint64_t(left) + width <= 32768 && std::uint64_t(top) + height <= 32768;
    }
};
static_assert(sizeof(MaterialCaptureConstants) == 64);

// Preserves the original material computation/discard, replacing only its
// color exports with (normalized MV.xy, mean RGB attenuation, device depth).
// Rejected history or a pixel contributing neither F nor attenuation discards.
// Must render into an owned target; never substitute this for the color draw.
// b1/space31: viewport origin XY, inverse extent XY, jitter delta UV, unused XY.
VertexHistoryShader RewriteMaterialMotion(std::string_view disassembly, MaterialSource source,
                                          MaterialDestination destination,
                                          MaterialMotionTarget target = MaterialMotionTarget::SeparateTarget,
                                          unsigned firstHistoryRegister = UINT32_MAX,
                                          GeometryLayout layout = GeometryLayout::Contiguous);
// A paired pipeline must pass the rewritten VS's previousRegister. The original
// PS can omit VS-only outputs (for example SV_ClipDistance), so independently
// appending to each stage's first free register does not produce a linked pair.
// OriginalColorAndCapture preserves every original color export/discard and
// writes u1/space31 as a rasterizer-ordered raw buffer. It requires read-only
// depth/stencil, ROV hardware support, one nonoverlapping owned region per object,
// MaterialCaptureConstants validated against the bound buffer, and ordering
// between overlapping draws. ROV ordering alone covers a single Draw call;
// overlapping writes from different Draw calls require a UAV barrier/dependency.
// Records: float MV.xy/depth, uint frame; float transmission.rgb, uint zero.
} // namespace GlassFg
