#pragma once
#include "GeometryPipelineCache.h"
#include <mutex>
#include <unordered_map>
#include <cstring>

namespace GlassFg
{
// Immutable diagnostic descriptors only. No compiler, GPU allocation, replay
// permission or dependency on driver-private PSO storage.
class GeometryObservationCache
{
    mutable std::mutex mutex;
    std::unordered_map<ID3D12PipelineState*, std::shared_ptr<const GeometryPipelineEntry>> entries;
    std::size_t used = 0;
    const std::size_t maxEntries, maxBytes;

  public:
    explicit GeometryObservationCache(std::size_t count = 1024, std::size_t bytes = 32 * 1024 * 1024)
        : maxEntries(count), maxBytes(bytes) {}

    bool observe(ID3D12PipelineState* identity, const D3D12_GRAPHICS_PIPELINE_STATE_DESC& d) noexcept
    {
        // Depth-writing geometry was excluded by the material capture cache.
        // Eligibility here identifies no material or velocity semantic.
        if (!identity || !d.pRootSignature || !d.DepthStencilState.DepthEnable ||
            d.DepthStencilState.DepthWriteMask != D3D12_DEPTH_WRITE_MASK_ALL ||
            !d.VS.pShaderBytecode || !d.PS.pShaderBytecode || !d.VS.BytecodeLength || !d.PS.BytecodeLength ||
            d.VS.BytecodeLength > 2 * 1024 * 1024 || d.PS.BytecodeLength > 2 * 1024 * 1024 ||
            !d.NumRenderTargets || d.NumRenderTargets > 8 || d.GS.BytecodeLength || d.HS.BytecodeLength ||
            d.DS.BytecodeLength || d.StreamOutput.NumEntries || d.StreamOutput.NumStrides ||
            d.PrimitiveTopologyType != D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE ||
            d.InputLayout.NumElements > 32 || (d.InputLayout.NumElements && !d.InputLayout.pInputElementDescs))
            return false;
        try
        {
            std::lock_guard lock(mutex);
            if (entries.contains(identity)) return true;
            const auto cost = sizeof(GeometryPipelineEntry) + sizeof(GeometryRoot) +
                d.VS.BytecodeLength + d.PS.BytecodeLength +
                d.InputLayout.NumElements * (sizeof(D3D12_INPUT_ELEMENT_DESC) + sizeof(std::string) + 128);
            if (entries.size() >= maxEntries || used > maxBytes || cost > maxBytes - used) return false;
            auto entry = std::make_shared<GeometryPipelineEntry>();
            auto root = std::make_shared<GeometryRoot>();
            root->original = d.pRootSignature;
            entry->root = std::move(root); // extended stays null: observation is never replay admission.
            entry->original = identity;
            entry->description = d;
            entry->description.CachedPSO = {};
            entry->description.StreamOutput = {};
            entry->description.GS = {}; entry->description.HS = {}; entry->description.DS = {};
            entry->vertexBytes.resize(d.VS.BytecodeLength);
            entry->pixelBytes.resize(d.PS.BytecodeLength);
            std::memcpy(entry->vertexBytes.data(), d.VS.pShaderBytecode, d.VS.BytecodeLength);
            std::memcpy(entry->pixelBytes.data(), d.PS.pShaderBytecode, d.PS.BytecodeLength);
            entry->description.VS = { entry->vertexBytes.data(), entry->vertexBytes.size() };
            entry->description.PS = { entry->pixelBytes.data(), entry->pixelBytes.size() };
            entry->semantics.reserve(d.InputLayout.NumElements);
            if (d.InputLayout.NumElements)
                entry->inputs.assign(d.InputLayout.pInputElementDescs,
                                     d.InputLayout.pInputElementDescs + d.InputLayout.NumElements);
            for (auto& input : entry->inputs)
            {
                if (!input.SemanticName || !input.SemanticName[0] || strnlen_s(input.SemanticName, 128) == 128)
                    return false;
                entry->semantics.emplace_back(input.SemanticName);
                input.SemanticName = entry->semantics.back().c_str();
            }
            entry->description.InputLayout = { entry->inputs.data(), static_cast<UINT>(entry->inputs.size()) };
            entry->identity = (uint64_t { 1 } << 63) | (entries.size() + 1);
            entries.emplace(identity, std::move(entry));
            used += cost;
            return true;
        }
        catch (...) { return false; }
    }

    std::shared_ptr<const GeometryPipelineEntry> find(ID3D12PipelineState* identity) const
    {
        std::lock_guard lock(mutex);
        const auto it = entries.find(identity);
        return it == entries.end() ? nullptr : it->second;
    }
};
} // namespace GlassFg
