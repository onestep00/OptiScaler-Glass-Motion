#pragma once
#include "ExperimentPipelineAbi.h"
#include "GeometryPipelineCache.h"
#include <new>
#include <atomic>

namespace GlassFg
{
using ExperimentPipelineLease = std::shared_ptr<const GeometryPipelineEntry>;
using ExperimentVertexCaptureRequest = bool (*)(ID3D12PipelineState*) noexcept;
inline std::atomic<ExperimentVertexCaptureRequest> experimentVertexCaptureRequest = nullptr;
inline int32_t RequestExperimentVertexCapture(const void* token)
{
    const auto request = experimentVertexCaptureRequest.load(std::memory_order_acquire);
    if (!token || !request) return 0;
    const auto& lease = *static_cast<const ExperimentPipelineLease*>(token);
    return lease && request(lease->original.Get()) ? 1 : 0;
}
inline void* RetainExperimentPipeline(const void* source)
{
    if (!source) return nullptr;
    const auto& lease = *static_cast<const ExperimentPipelineLease*>(source);
    if (!lease || !lease->root) return nullptr;
    return new (std::nothrow) ExperimentPipelineLease(lease);
}
inline int32_t ViewExperimentPipeline(const void* token, GlassExperimentPipelineView* out)
{
    if (!token || !out || out->size != sizeof(*out)) return 0;
    const auto& lease = *static_cast<const ExperimentPipelineLease*>(token);
    if (!lease || !lease->root) return 0;
    const auto& root = *lease->root;
    *out = { sizeof(*out), sizeof(lease->description), lease->identity, &lease->description,
             root.original.Get(), root.extended.Get(), root.layout == GeometryLayout::PerInstance ? 1u : 0u,
             root.dwords, root.constantsSlot, root.previousSlot, root.currentSlot, root.materialSlot,
             root.captureSlot, root.instanceSlot, sizeof(D3D12_ROOT_PARAMETER1),
             static_cast<uint32_t>(root.originalParameters.size()), root.originalParameters.data(),
             static_cast<uint32_t>(root.originalSerialized.size()), root.originalNodeMask, root.originalSerialized.data(),
             lease->vertexOnlyCapture ? 1u : 0u, RequestExperimentVertexCapture };
    return 1;
}
inline void ReleaseExperimentPipeline(void* token)
{
    delete static_cast<ExperimentPipelineLease*>(token);
}
inline GlassExperimentPipelineAccess BorrowExperimentPipeline(const ExperimentPipelineLease& lease)
{
    return { &lease, RetainExperimentPipeline, ViewExperimentPipeline, ReleaseExperimentPipeline };
}
} // namespace GlassFg
