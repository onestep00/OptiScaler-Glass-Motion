#pragma once
#include "GeometryPipeline.h"
#include <array>
#include <bit>
#include <cstdint>
#include <cstring>

namespace GlassFg
{
// Observe from command creation/successful Reset, including every graphics root
// setter and ClearState. Bundles/indirect root changes make the record unknown.
// This stores no COM ownership. The command/PSO/root owner must outlive replay.
class GraphicsRootBindings
{
  public:
    void reset(ID3D12PipelineState* initial = nullptr)
    {
        written = 0;
        root = nullptr;
        complete = true;
        pipeline = initial;
    }
    void invalidate() { complete = false; }
    void setPipeline(ID3D12PipelineState* value) { pipeline = value; }
    void setRoot(ID3D12RootSignature* value)
    {
        if (root != value)
        {
            written = 0;
            root = value;
        }
    }
    // Actual change of descriptor heaps invalidates table bindings only.
    void heapsChanged()
    {
        for (auto remaining = written; remaining; remaining &= remaining - 1)
        {
            const auto index = std::countr_zero(remaining);
            if (slots[index].type == D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE)
                written &= ~(UINT64(1) << index);
        }
    }
    void table(UINT slot, D3D12_GPU_DESCRIPTOR_HANDLE value)
    {
        address(slot, D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE, value.ptr);
    }
    void address(UINT slot, D3D12_ROOT_PARAMETER_TYPE type, UINT64 value)
    {
        if (slot >= slots.size() || !root ||
            (type != D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE && type != D3D12_ROOT_PARAMETER_TYPE_CBV &&
             type != D3D12_ROOT_PARAMETER_TYPE_SRV && type != D3D12_ROOT_PARAMETER_TYPE_UAV))
        {
            invalidate();
            return;
        }
        auto& s = slots[slot];
        s.type = type;
        s.address = value;
        s.known = 1;
        written |= UINT64(1) << slot;
    }
    void constants(UINT slot, UINT count, const void* values, UINT offset)
    {
        if (slot >= slots.size() || !root || offset > 64 || count > 64 - offset || (count && !values))
        {
            invalidate();
            return;
        }
        if (!count)
            return;
        auto& s = slots[slot];
        if (!(written & (UINT64(1) << slot)))
            s.known = 0;
        else if (s.type != D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS)
        {
            invalidate();
            return;
        }
        s.type = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        std::memcpy(s.values.data() + offset, values, count * sizeof(UINT));
        const auto mask = count == 64 ? UINT64_MAX : ((UINT64(1) << count) - 1) << offset;
        s.known |= mask;
        written |= UINT64(1) << slot;
    }
    bool canReplay(const GeometryRoot& expected, ID3D12PipelineState* originalPipeline) const
    {
        if (!complete || !root || root != expected.original.Get() || !pipeline || pipeline != originalPipeline ||
            expected.originalParameters.size() > slots.size())
            return false;
        for (auto remaining = written; remaining; remaining &= remaining - 1)
        {
            const auto i = std::countr_zero(remaining);
            const auto& s = slots[i];
            if (static_cast<size_t>(i) >= expected.originalParameters.size())
                return false;
            const auto& p = expected.originalParameters[i];
            if (p.ParameterType != s.type)
                return false;
            if (s.type == D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS && p.Constants.Num32BitValues < 64 &&
                (s.known >> p.Constants.Num32BitValues))
                return false;
        }
        return true;
    }
    // Caller validated canReplay and suppresses its own observer callbacks.
    // Replays every value known since Reset, including partially set constants.
    // Never reads/writes a descriptor heap or changes the application's tables.
    void replay(ID3D12GraphicsCommandList* command, ID3D12RootSignature* destination) const
    {
        command->SetGraphicsRootSignature(destination);
        for (auto remaining = written; remaining; remaining &= remaining - 1)
        {
            const auto i = static_cast<UINT>(std::countr_zero(remaining));
            const auto& s = slots[i];
            switch (s.type)
            {
            case D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE:
                command->SetGraphicsRootDescriptorTable(i, { s.address });
                break;
            case D3D12_ROOT_PARAMETER_TYPE_CBV:
                command->SetGraphicsRootConstantBufferView(i, s.address);
                break;
            case D3D12_ROOT_PARAMETER_TYPE_SRV:
                command->SetGraphicsRootShaderResourceView(i, s.address);
                break;
            case D3D12_ROOT_PARAMETER_TYPE_UAV:
                command->SetGraphicsRootUnorderedAccessView(i, s.address);
                break;
            case D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS:
                for (UINT first = 0; first < 64;)
                {
                    if (!(s.known & (UINT64(1) << first)))
                    {
                        ++first;
                        continue;
                    }
                    UINT end = first + 1;
                    while (end < 64 && (s.known & (UINT64(1) << end)))
                        ++end;
                    command->SetGraphicsRoot32BitConstants(i, end - first, s.values.data() + first, first);
                    first = end;
                }
                break;
            default:
                break;
            }
        }
    }
    ID3D12PipelineState* pipeline = nullptr;
    ID3D12RootSignature* root = nullptr;

  private:
    struct Slot
    {
        D3D12_ROOT_PARAMETER_TYPE type = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        UINT64 address = 0, known = 0;
        std::array<UINT, 64> values {};
    };
    // Reset/root changes invalidate one mask. Stale payload bytes are never
    // read unless that slot is written again; partial constants have their
    // own validity mask. Address setters do not clear 64 unused constants.
    std::array<Slot, 64> slots {};
    UINT64 written = 0;
    bool complete = false;
};
} // namespace GlassFg
