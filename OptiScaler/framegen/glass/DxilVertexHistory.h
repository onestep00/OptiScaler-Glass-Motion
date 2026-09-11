#pragma once
#include <string>
#include <string_view>
#include <cstdint>

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
VertexHistoryShader RewriteVertexHistory(std::string_view disassembly);

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

// Preserves the original material computation/discard, replacing only its
// color exports with (normalized MV.xy, mean RGB attenuation, device depth).
// Rejected history or a pixel contributing neither F nor attenuation discards.
// Must render into an owned target; never substitute this for the color draw.
// b1/space31: viewport origin XY, inverse extent XY, jitter delta UV, unused XY.
VertexHistoryShader RewriteMaterialMotion(std::string_view disassembly, MaterialSource source,
                                          MaterialDestination destination);
} // namespace GlassFg
