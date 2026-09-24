#pragma once
#include <d3d12.h>
#include <wrl/client.h>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>
#include <cstddef>
#include "GeometryInstance.h"

namespace GlassFg
{
enum class MaterialMotionTarget;
struct VertexConstantPair;
struct VertexInputPair;
struct VertexClipPair;
struct NativeClipInputs;
struct NativeGraft;
// Captured serialized bytes must belong to originalIdentity after any upstream
// sampler overrides. Creation does not bind or replace the application's root.
struct GeometryRoot
{
    GeometryRoot() = default;
    GeometryRoot(GeometryRoot&&) = default;
    GeometryRoot& operator=(GeometryRoot&&) = default;
    GeometryRoot(const GeometryRoot&) = delete;
    GeometryRoot& operator=(const GeometryRoot&) = delete;
    Microsoft::WRL::ComPtr<ID3D12RootSignature> original, extended;
    std::vector<std::byte> originalSerialized;
    UINT originalNodeMask = 0;
    std::vector<D3D12_ROOT_PARAMETER1> originalParameters;
    std::vector<std::vector<D3D12_DESCRIPTOR_RANGE1>> ranges;
    UINT constantsSlot = 0, previousSlot = 0, currentSlot = 0, materialSlot = 0, captureSlot = 0;
    UINT dwords = 0;
    UINT instanceSlot = UINT32_MAX;
    GeometryLayout layout = GeometryLayout::Contiguous;
};
HRESULT CreateGeometryRoot(ID3D12Device* device, ID3D12RootSignature* originalIdentity, UINT nodeMask,
                           const void* serialized, SIZE_T bytes, GeometryRoot& output, std::string& error,
                           GeometryLayout layout = GeometryLayout::Contiguous);
// Packed variants that had to fall back to coverage-only capture because the
// material exports could not be read safely. Diagnostics only.
std::uint64_t ReadPackedCoverageFallbackCount() noexcept;
// Packed variants that had to compile without the capture constant pair
// because the audited b1/space0 block was absent, ambiguous or oversized.
// Those keep the pre-pair tag payload and carry no capture jitter delta.
std::uint64_t ReadPackedCaptureFallbackCount() noexcept;

// Instantiate on a worker, reuse for PSOs, and retain the compiler through all
// calls. No compilation, file I/O or PSO creation belongs in a draw callback.
class GeometryCompiler
{
  public:
    explicit GeometryCompiler(const std::filesystem::path& compilerPath);
    ~GeometryCompiler();
    GeometryCompiler(const GeometryCompiler&) = delete;
    GeometryCompiler& operator=(const GeometryCompiler&) = delete;
    HRESULT create(ID3D12Device* device, const GeometryRoot& root, const D3D12_GRAPHICS_PIPELINE_STATE_DESC& original,
                   Microsoft::WRL::ComPtr<ID3D12PipelineState>& output, std::string& error);
    HRESULT createCoverage(ID3D12Device* device, const GeometryRoot& root,
                           const D3D12_GRAPHICS_PIPELINE_STATE_DESC& original,
                           Microsoft::WRL::ComPtr<ID3D12PipelineState>& output, std::string& error);
    // Retains the original material draw and atomically records one nearest
    // transparent-layer payload per screen pixel. Requires SM 6.6/int64 ops.
    // With a capture pair the record tag also carries the two raw words of the
    // named constant row and the paired pixel shader adds their frame-to-frame
    // UV difference to the packed motion. A pipeline whose shader does not
    // expose that binding is recompiled without the pair instead of rejected,
    // and `pairMissing` then reports true: the pair-less variant adds a zero
    // delta, so its recorded motion is the raw jittered difference and not the
    // engine's motion convention. The caller has to withhold that variant from
    // delivery (F-01).
    //
    // With a native graft (NativeGraftCatalog) the vertex stage is the grafted
    // VS: its own outputs carry the engine's de-jittered current clip and the
    // previous clip from the engine's MotionMatrix supply, no vertex history is
    // read or written, and the pixel stage adds neither a jitter term nor a
    // capture delta. capture must be null then; pairMissing stays false and a
    // failed graft rewrite is returned as the failure (no pair-less retry). The
    // cache passes the camera-only variant (NativeGraft::cameraBytes and its
    // outputs) the same way to build the array/multi-instance pipeline.
    //
    // lightTarget: the original PS draws in an engine pass that adds light to
    // the displayed scene colour (NativeGraftCatalog.h IsLightPixelShader). Only
    // then does the record opacity include the displayed brightness of the
    // colour the draw adds; otherwise it is the material opacity alone
    // (RewriteMaterialMotion lightTarget).
    //
    // backgroundTarget: the original PS shows background content rather than
    // the surface it is drawn on (NativeGraftCatalog.h IsBackgroundPixelShader).
    // The pixel stage is then the coverage-only rewrite whatever the blend
    // equation: record opacity 0 and no brightness term, so the interior keeps
    // the engine's motion and only the boundary takes the object's.
    HRESULT createPackedMotion(ID3D12Device* device, const GeometryRoot& root,
                               const D3D12_GRAPHICS_PIPELINE_STATE_DESC& original,
                               Microsoft::WRL::ComPtr<ID3D12PipelineState>& output, std::string& error,
                               const VertexConstantPair* capture = nullptr, bool* pairMissing = nullptr,
                               const NativeGraft* graft = nullptr, bool lightTarget = false,
                               bool backgroundTarget = false);
    HRESULT createCoverageAudit(ID3D12Device* device, const GeometryRoot& root,
                                const D3D12_GRAPHICS_PIPELINE_STATE_DESC& original,
                                Microsoft::WRL::ComPtr<ID3D12PipelineState>& output, std::string& error,
                                const VertexConstantPair* capture = nullptr);
    // Diagnostic actual vertex output only; keeps original PS/state unchanged,
    // including depth writes. Replace the original draw once, never duplicate it.
    HRESULT createVertexCapture(ID3D12Device* device, const GeometryRoot& root,
                                const D3D12_GRAPHICS_PIPELINE_STATE_DESC& original,
                                Microsoft::WRL::ComPtr<ID3D12PipelineState>& output, std::string& error,
                                const VertexConstantPair* capture = nullptr,
                                const VertexClipPair* clipPair = nullptr,
                                const VertexInputPair* inputPair = nullptr);
    // Explicit native clip inputs and added same-draw MV UAV, no second draw.
    // Default: unblended PS without discard/depth exports. inputs.material opts
    // into supported material blending/discard with read-only depth/stencil.
    HRESULT createNativeMotionCapture(ID3D12Device* device, const GeometryRoot& root,
                                const D3D12_GRAPHICS_PIPELINE_STATE_DESC& original,
                                Microsoft::WRL::ComPtr<ID3D12PipelineState>& output, std::string& error,
                                const NativeClipInputs& inputs);

  private:
    struct Impl;
    std::unique_ptr<Impl> implementation;
    HRESULT createTarget(ID3D12Device* device, const GeometryRoot& root,
                         const D3D12_GRAPHICS_PIPELINE_STATE_DESC& original,
                         Microsoft::WRL::ComPtr<ID3D12PipelineState>& output, std::string& error,
                         MaterialMotionTarget target, bool vertexOnly = false,
                         const VertexConstantPair* capture = nullptr,
                         const VertexClipPair* clipPair = nullptr,
                         const NativeClipInputs* nativeInputs = nullptr,
                         const VertexInputPair* inputPair = nullptr,
                         const NativeGraft* graft = nullptr, bool lightTarget = false,
                         bool backgroundTarget = false);
};
} // namespace GlassFg
