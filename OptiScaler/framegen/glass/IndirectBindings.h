#pragma once
#include "GraphicsRootBindings.h"
#include <atomic>
#include <mutex>

namespace GlassFg
{
// Derived only from successful public CreateCommandSignature calls. Never read
// driver objects or GPU argument buffers. Unobserved/unknown signatures reject.
struct IndirectBindingEffect
{
    struct Reset
    {
        D3D12_ROOT_PARAMETER_TYPE type = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        UINT slot = 0, offset = 0, count = 0;
    };
    std::array<Reset, 64> resets {};
    UINT count = 0;
    bool valid = false, graphics = false;

    static IndirectBindingEffect describe(const D3D12_COMMAND_SIGNATURE_DESC& desc)
    {
        IndirectBindingEffect result;
        if (!desc.pArgumentDescs || !desc.NumArgumentDescs || desc.NumArgumentDescs > 128)
            return result;
        bool operation = false;
        for (UINT i = 0; i < desc.NumArgumentDescs; ++i)
        {
            if (operation)
                return result; // A draw/dispatch must be the final argument.
            const auto& arg = desc.pArgumentDescs[i];
            Reset reset;
            switch (arg.Type)
            {
            case D3D12_INDIRECT_ARGUMENT_TYPE_DRAW:
            case D3D12_INDIRECT_ARGUMENT_TYPE_DRAW_INDEXED:
            case D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH_MESH:
                result.graphics = true;
                operation = true;
                continue;
            case D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH:
            case D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH_RAYS:
                operation = true;
                continue;
            case D3D12_INDIRECT_ARGUMENT_TYPE_VERTEX_BUFFER_VIEW:
            case D3D12_INDIRECT_ARGUMENT_TYPE_INDEX_BUFFER_VIEW:
                continue; // IA reset belongs to the separate IA observer.
            case D3D12_INDIRECT_ARGUMENT_TYPE_CONSTANT:
                reset.slot = arg.Constant.RootParameterIndex;
                reset.offset = arg.Constant.DestOffsetIn32BitValues;
                reset.count = arg.Constant.Num32BitValuesToSet;
                if (!reset.count || reset.offset >= 64 || reset.count > 64 - reset.offset)
                    return result;
                break;
            case D3D12_INDIRECT_ARGUMENT_TYPE_CONSTANT_BUFFER_VIEW:
                reset.type = D3D12_ROOT_PARAMETER_TYPE_CBV;
                reset.slot = arg.ConstantBufferView.RootParameterIndex;
                break;
            case D3D12_INDIRECT_ARGUMENT_TYPE_SHADER_RESOURCE_VIEW:
                reset.type = D3D12_ROOT_PARAMETER_TYPE_SRV;
                reset.slot = arg.ShaderResourceView.RootParameterIndex;
                break;
            case D3D12_INDIRECT_ARGUMENT_TYPE_UNORDERED_ACCESS_VIEW:
                reset.type = D3D12_ROOT_PARAMETER_TYPE_UAV;
                reset.slot = arg.UnorderedAccessView.RootParameterIndex;
                break;
            default:
                return result;
            }
            if (reset.slot >= 64 || result.count == result.resets.size())
                return result;
            result.resets[result.count++] = reset;
        }
        result.valid = operation;
        return result;
    }

    bool apply(GraphicsRootBindings& bindings, ID3D12RootSignature* root) const
    {
        if (!valid || (graphics && count && (!root || root != bindings.root)))
        {
            bindings.invalidate();
            return false;
        }
        if (!graphics)
            return true; // Compute root arguments are separate from graphics.
        constexpr std::array<UINT, 64> zeros {};
        for (UINT i = 0; i < count; ++i)
        {
            const auto& reset = resets[i];
            if (reset.type == D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS)
                bindings.constants(reset.slot, reset.count, zeros.data(), reset.offset);
            else
                bindings.address(reset.slot, reset.type, 0);
        }
        return true;
    }
};

// Immutable, bounded entries: retaining the COM signature/root prevents pointer
// reuse. Creation alone locks/decodes; ExecuteIndirect makes at most 32 probes.
// Exhaustion stays unknown rather than evicting an entry used by another thread.
class IndirectBindingCache
{
    struct Entry
    {
        std::atomic<ID3D12CommandSignature*> key = nullptr;
        Microsoft::WRL::ComPtr<ID3D12CommandSignature> signature;
        Microsoft::WRL::ComPtr<ID3D12RootSignature> root;
        IndirectBindingEffect effect;
    };
    std::array<Entry, 256> entries;
    std::mutex creation;
    static size_t first(ID3D12CommandSignature* signature)
    {
        auto value = reinterpret_cast<std::uintptr_t>(signature) >> 4;
        value ^= value >> 17;
        value *= 0x9e3779b97f4a7c15ull;
        return (value >> 32) & 255;
    }

  public:
    bool created(ID3D12CommandSignature* signature, ID3D12RootSignature* root, const D3D12_COMMAND_SIGNATURE_DESC& desc)
    {
        if (!signature)
            return false;
        std::lock_guard lock(creation);
        const auto start = first(signature);
        for (unsigned i = 0; i < 32; ++i)
        {
            auto& entry = entries[(start + i) & 255];
            const auto key = entry.key.load(std::memory_order_acquire);
            if (key == signature)
                return true;
            if (key)
                continue;
            entry.effect = IndirectBindingEffect::describe(desc);
            entry.signature = signature;
            entry.root = root;
            entry.key.store(signature, std::memory_order_release);
            return true;
        }
        return false;
    }
    bool apply(ID3D12CommandSignature* signature, GraphicsRootBindings& bindings) const
    {
        if (signature)
        {
            const auto start = first(signature);
            for (unsigned i = 0; i < 32; ++i)
            {
                const auto& entry = entries[(start + i) & 255];
                const auto key = entry.key.load(std::memory_order_acquire);
                if (!key)
                    break;
                if (key == signature)
                    return entry.effect.apply(bindings, entry.root.Get());
            }
        }
        bindings.invalidate();
        return false;
    }
};
} // namespace GlassFg
