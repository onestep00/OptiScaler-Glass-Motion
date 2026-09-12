#pragma once
#include "GeometryPipelineCache.h"
#include <mutex>
#include <unordered_map>
#include <cstring>
#include <atomic>

namespace GlassFg
{
struct GeometryObservationStats
{
    std::uint64_t attempted = 0, retained = 0, filtered = 0, invalid = 0, capacityRejected = 0;
    std::size_t retainedBytes = 0;
};

// Immutable diagnostic descriptors only. No compiler, GPU allocation, replay
// permission or dependency on driver-private PSO storage.
class GeometryObservationCache
{
    mutable std::mutex mutex;
    std::unordered_map<ID3D12PipelineState*, std::shared_ptr<const GeometryPipelineEntry>> entries;
    std::unordered_map<ID3D12RootSignature*, std::shared_ptr<const GeometryRoot>> roots;
    std::size_t used = 0;
    const std::size_t maxEntries, maxBytes;
    const bool blendedOnly;
    std::atomic<std::uint64_t> attempted = 0, filtered = 0, invalid = 0, capacityRejected = 0;

    static bool blended(const D3D12_GRAPHICS_PIPELINE_STATE_DESC& d) noexcept
    {
        if (d.BlendState.AlphaToCoverageEnable) return true;
        for (UINT target = 0; target < d.NumRenderTargets && target < 8; ++target)
        {
            const auto& value = d.BlendState.RenderTarget[d.BlendState.IndependentBlendEnable ? target : 0];
            const bool opaqueReplace = value.SrcBlend == D3D12_BLEND_ONE && value.DestBlend == D3D12_BLEND_ZERO &&
                value.BlendOp == D3D12_BLEND_OP_ADD && value.SrcBlendAlpha == D3D12_BLEND_ONE &&
                value.DestBlendAlpha == D3D12_BLEND_ZERO && value.BlendOpAlpha == D3D12_BLEND_OP_ADD;
            if (value.RenderTargetWriteMask && value.BlendEnable && !value.LogicOpEnable && !opaqueReplace)
                return true;
        }
        return false;
    }

  public:
    explicit GeometryObservationCache(std::size_t count = 1024, std::size_t bytes = 32 * 1024 * 1024,
                                      bool onlyBlended = false)
        : maxEntries(count), maxBytes(bytes), blendedOnly(onlyBlended) {}

    bool rootCreated(ID3D12RootSignature* identity, const void* bytes, SIZE_T size, UINT node = 0) noexcept
    {
        if (!identity || !bytes || !size || size > 256 * 1024) return false;
        try
        {
            std::lock_guard lock(mutex);
            if (roots.contains(identity)) return true;
            if (roots.size() >= 128) return false;
            Microsoft::WRL::ComPtr<ID3D12VersionedRootSignatureDeserializer> reader;
            if (FAILED(D3D12CreateVersionedRootSignatureDeserializer(bytes, size, IID_PPV_ARGS(&reader)))) return false;
            const D3D12_VERSIONED_ROOT_SIGNATURE_DESC* description = nullptr;
            if (FAILED(reader->GetRootSignatureDescAtVersion(D3D_ROOT_SIGNATURE_VERSION_1_1, &description)) ||
                !description || description->Desc_1_1.NumParameters > 64) return false;
            const auto& d = description->Desc_1_1;
            std::size_t ranges = 0;
            for (UINT i = 0; i < d.NumParameters; ++i)
                if (d.pParameters[i].ParameterType == D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE)
                {
                    const auto n = d.pParameters[i].DescriptorTable.NumDescriptorRanges;
                    if (n > 2048 - ranges) return false;
                    ranges += n;
                }
            const auto cost = sizeof(GeometryRoot) + size + d.NumParameters *
                (sizeof(D3D12_ROOT_PARAMETER1) + sizeof(std::vector<D3D12_DESCRIPTOR_RANGE1>)) +
                ranges * sizeof(D3D12_DESCRIPTOR_RANGE1);
            if (used > maxBytes || cost > maxBytes - used) return false;
            auto root = std::make_shared<GeometryRoot>();
            root->original = identity;
            root->originalSerialized.assign(static_cast<const std::byte*>(bytes), static_cast<const std::byte*>(bytes) + size);
            root->originalNodeMask = node;
            if (d.NumParameters) root->originalParameters.assign(d.pParameters, d.pParameters + d.NumParameters);
            root->ranges.resize(d.NumParameters);
            for (UINT i = 0; i < d.NumParameters; ++i)
            {
                auto& parameter = root->originalParameters[i];
                if (parameter.ParameterType != D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE) continue;
                auto& table = parameter.DescriptorTable;
                auto& owned = root->ranges[i];
                if (table.NumDescriptorRanges)
                    owned.assign(table.pDescriptorRanges, table.pDescriptorRanges + table.NumDescriptorRanges);
                table.pDescriptorRanges = owned.data();
            }
            roots.emplace(identity, std::move(root));
            used += cost;
            return true;
        }
        catch (...) { return false; }
    }

    bool observe(ID3D12PipelineState* identity, const D3D12_GRAPHICS_PIPELINE_STATE_DESC& d) noexcept
    {
        ++attempted;
        // Observation must also retain passes rejected by material compilation.
        // Depth/blend state identifies neither transparency nor replay permission.
        if (!identity || !d.pRootSignature ||
            !d.VS.pShaderBytecode || !d.PS.pShaderBytecode || !d.VS.BytecodeLength || !d.PS.BytecodeLength ||
            d.VS.BytecodeLength > 2 * 1024 * 1024 || d.PS.BytecodeLength > 2 * 1024 * 1024 ||
            !d.NumRenderTargets || d.NumRenderTargets > 8 || d.GS.BytecodeLength || d.HS.BytecodeLength ||
            d.DS.BytecodeLength || d.StreamOutput.NumEntries || d.StreamOutput.NumStrides ||
            d.PrimitiveTopologyType != D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE ||
            d.InputLayout.NumElements > 32 || (d.InputLayout.NumElements && !d.InputLayout.pInputElementDescs))
        {
            ++invalid;
            return false;
        }
        if (blendedOnly && !blended(d))
        {
            ++filtered;
            return false;
        }
        try
        {
            std::lock_guard lock(mutex);
            if (entries.contains(identity)) return true;
            const auto cost = sizeof(GeometryPipelineEntry) + sizeof(GeometryRoot) +
                d.VS.BytecodeLength + d.PS.BytecodeLength +
                d.InputLayout.NumElements * (sizeof(D3D12_INPUT_ELEMENT_DESC) + sizeof(std::string) + 128);
            if (entries.size() >= maxEntries || used > maxBytes || cost > maxBytes - used)
            {
                ++capacityRejected;
                return false;
            }
            auto entry = std::make_shared<GeometryPipelineEntry>();
            const auto knownRoot = roots.find(d.pRootSignature);
            if (knownRoot != roots.end()) entry->root = knownRoot->second;
            else
            {
                auto root = std::make_shared<GeometryRoot>();
                root->original = d.pRootSignature;
                entry->root = std::move(root);
            }
            // extended stays null: observation is never replay admission.
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

    GeometryObservationStats stats() const
    {
        std::lock_guard lock(mutex);
        return { attempted.load(), entries.size(), filtered.load(), invalid.load(), capacityRejected.load(), used };
    }
};
} // namespace GlassFg
