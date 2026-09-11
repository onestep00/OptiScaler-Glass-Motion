#pragma once
#include "GeometryPipeline.h"
#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

namespace GlassFg
{
struct GeometryPipelineEntry
{
    std::shared_ptr<const GeometryRoot> root;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> original, instrumented;
    D3D12_GRAPHICS_PIPELINE_STATE_DESC description {};
    std::uint64_t identity = 0;
    std::vector<std::byte> vertexBytes, pixelBytes;
    std::vector<std::string> semantics;
    std::vector<D3D12_INPUT_ELEMENT_DESC> inputs;
};

struct GeometryCacheLimits
{
    std::size_t roots = 128, pipelines = 2048, bytes = 128 * 1024 * 1024;
};
struct GeometryCacheStats
{
    std::uint64_t roots = 0, pipelines = 0, ready = 0, rejected = 0, pending = 0, retainedBytes = 0;
    std::string lastError;
};

// Captures successful public creation calls, then builds the paired shaders on
// one worker. find() never compiles, waits for a result, or calls a D3D12 method.
// A caller recording a returned entry must retain its shared_ptr until both GPU
// completion and recording discard. Cache membership alone is not GPU ownership.
class GeometryPipelineCache
{
  public:
    GeometryPipelineCache(ID3D12Device* device, std::filesystem::path compiler, GeometryCacheLimits limits = {});
    ~GeometryPipelineCache();
    GeometryPipelineCache(const GeometryPipelineCache&) = delete;
    GeometryPipelineCache& operator=(const GeometryPipelineCache&) = delete;

    bool rootCreated(ID3D12RootSignature* root, UINT nodeMask, const void* bytes, SIZE_T size) noexcept;
    bool pipelineCreated(ID3D12PipelineState* pipeline, const D3D12_GRAPHICS_PIPELINE_STATE_DESC& desc) noexcept;
    std::shared_ptr<const GeometryPipelineEntry> find(ID3D12PipelineState* original) const;
    GeometryCacheStats stats() const;
    // May join the compiler thread. Do not call from a render/API callback.
    void stop();
    static bool compilerThread();

  private:
    struct Impl;
    std::unique_ptr<Impl> implementation;
};
} // namespace GlassFg
