#pragma once
#include "GeometryPipelineCache.h"
#include "GraphicsRootBindings.h"
#include "GeometryDrawBatch.h"

namespace GlassFg
{
struct GeometryIndexedArguments
{
    UINT indices, instances, startIndex;
    INT baseVertex;
    UINT startInstance;
};

// Returned only after the owner has reserved immutable mappings and retained
// all GPU resources/pipeline leases through completion AND recording discard.
// No allocation, shader compilation, resource transition or wait in this seam.
struct GeometryPreparedDraw
{
    ID3D12PipelineState* pipeline = nullptr;
    InstanceHistoryConstants history {};
    D3D12_GPU_VIRTUAL_ADDRESS previous = 0, current = 0, material = 0, capture = 0, mapping = 0;

    bool bindable() const
    {
        return pipeline && previous && current && material && capture && mapping && !(material & 255) &&
               !(previous & 3) && !(current & 3) && !(capture & 3) && !(mapping & 3) && history.instances &&
               history.frame && history.mappingCapacity &&
               std::uint64_t(history.mappingBase) + history.instances <= history.mappingCapacity;
    }
    void bind(ID3D12GraphicsCommandList* command, const GeometryRoot& root,
              const GraphicsRootBindings& original) const
    {
        original.replay(command, root.extended.Get());
        command->SetPipelineState(pipeline);
        command->SetGraphicsRoot32BitConstants(root.constantsSlot, 8, &history, 0);
        command->SetGraphicsRootShaderResourceView(root.previousSlot, previous);
        command->SetGraphicsRootUnorderedAccessView(root.currentSlot, current);
        command->SetGraphicsRootConstantBufferView(root.materialSlot, material);
        command->SetGraphicsRootUnorderedAccessView(root.captureSlot, capture);
        command->SetGraphicsRootShaderResourceView(root.instanceSlot, mapping);
    }
};

// A process-resident owner. prepare must not mutate command state, and must
// return false unless pass provenance, viewport, mapping, resources and ordering
// are all established. A true result reserves lifetime even if binding rejects.
// finish reports recording, never GPU execution/completion or FG substitution.
struct GeometryDrawCaptureOwner
{
    virtual bool prepare(ID3D12GraphicsCommandList*, const GeometryDrawView&, const GeometryIndexedArguments&,
                         const std::shared_ptr<const GeometryPipelineEntry>&, const GraphicsRootBindings&,
                         GeometryPreparedDraw&) noexcept = 0;
    virtual void finish(ID3D12GraphicsCommandList*, bool recorded) noexcept = 0;
    virtual void submitted(ID3D12CommandQueue*, UINT, ID3D12CommandList* const*) noexcept {}
    virtual void discarded(ID3D12GraphicsCommandList*) noexcept {}
  protected:
    ~GeometryDrawCaptureOwner() = default;
};
void NotifyGeometryCaptureSubmit(ID3D12CommandQueue*, UINT, ID3D12CommandList* const*) noexcept;
} // namespace GlassFg
