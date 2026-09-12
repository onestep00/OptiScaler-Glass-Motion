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
    unsigned recordBytes = 32;
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
// Optional diagnostic capture of two raw words from an existing draw-bound CB.
// Binding/size validation does not establish the semantic meaning of the words.
// Stored at bytes 24/28 of each existing 32-byte record; no extra allocation.
struct VertexConstantPair
{
    unsigned space, binding, bytes, row;
};
// Explicit original uint input signature ID/components. Diagnostic raw words
// at bytes 24/28, mutually exclusive with the other diagnostic payloads.
// Values have no inferred bone/history meaning.
struct VertexInputPair
{
    unsigned input, first, second;
};
// Explicitly audited native VS output IDs, not automatically identified motion.
// Diagnostic records are 64 bytes: original clip float4 and frame/gen/padding,
// followed by selected current and previous clip float4 at bytes 32 and 48.
// Both buffers must use this stride, with capacity <= UINT32_MAX / 64.
// Preserve W for perspective-correct rasterization. Original outputs are unchanged.
// Mutually exclusive with VertexConstantPair. Reject nonfinite saved values
// before interpreting motion; native previous inputs still require validation.
struct VertexClipPair
{
    unsigned currentOutput, previousOutput;
};
struct NativeClipInputs
{
    unsigned currentInput, previousInput;
    // Diagnostic only: reserve capture bytes 0..31 and start pixels at >=1.
    // Adds an invocation counter at byte 16 before object/motion rejection.
    bool countInvocations = false;
    // Explicit material capture: preserve supported blending/discard and derive
    // transmission from the original blend equation. Requires read-only depth.
    // The caller still proves the supplied clip inputs are actual current/previous positions.
    bool material = false;
};
// Keep an explicitly identified native float4 render target as SV_Target0.
// Preserves native inputs/calculation/discard; does not identify motion semantics,
// change depth state, provide previous transforms, or select an object boundary.
VertexHistoryShader ExtractNativeMotionTarget(std::string_view disassembly, unsigned targetIndex);
VertexHistoryShader RewriteVertexHistory(std::string_view disassembly,
                                         GeometryLayout layout = GeometryLayout::Contiguous,
                                         const VertexConstantPair* capture = nullptr,
                                         const VertexClipPair* clipPair = nullptr,
                                         const VertexInputPair* inputPair = nullptr);

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
    // Production candidate: retain original material/color and atomically keep
    // the nearest layer's object MV, weight and frame-local ID in 8 bytes.
    OriginalColorAndPackedMotion,
    // Diagnostic object coverage only. No motion is produced or implied.
    OriginalColorAndCoverage,
    // Diagnostic: also record same-draw surviving/contributing pixels before
    // object mapping. The two reference bit regions use capture CB base/reserved.
    OriginalColorAndCoverageAudit,
    // Unblended native attachments; no discard or shader side effects permitted.
    // Coverage only, with original depth/stencil/color writes preserved.
    OriginalColorAndDepthCoverageAudit
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
                                          GeometryLayout layout = GeometryLayout::Contiguous,
                                          const NativeClipInputs* nativeInputs = nullptr);
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
// OriginalColorAndPackedMotion requires per-instance mapping, SM 6.6 plus
// Int64ShaderOps and reserved[0] as a nonzero frame-local object ID.
} // namespace GlassFg
