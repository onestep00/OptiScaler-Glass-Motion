#pragma once
#include <d3d12.h>
#include <wrl/client.h>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>
#include "GeometryInstance.h"

namespace GlassFg
{
enum class MaterialMotionTarget;
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
    HRESULT createCoverageAudit(ID3D12Device* device, const GeometryRoot& root,
                                const D3D12_GRAPHICS_PIPELINE_STATE_DESC& original,
                                Microsoft::WRL::ComPtr<ID3D12PipelineState>& output, std::string& error);
    // Diagnostic actual vertex output only; keeps original PS bytes unchanged.
    HRESULT createVertexCapture(ID3D12Device* device, const GeometryRoot& root,
                                const D3D12_GRAPHICS_PIPELINE_STATE_DESC& original,
                                Microsoft::WRL::ComPtr<ID3D12PipelineState>& output, std::string& error);

  private:
    struct Impl;
    std::unique_ptr<Impl> implementation;
    HRESULT createTarget(ID3D12Device* device, const GeometryRoot& root,
                         const D3D12_GRAPHICS_PIPELINE_STATE_DESC& original,
                         Microsoft::WRL::ComPtr<ID3D12PipelineState>& output, std::string& error,
                         MaterialMotionTarget target, bool vertexOnly = false);
};
} // namespace GlassFg
