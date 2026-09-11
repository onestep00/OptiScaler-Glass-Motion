#pragma once
#include "ExperimentPipelineAbi.h"
#include "GeometryPipelineCache.h"
#include <new>

namespace GlassFg
{
using ExperimentPipelineLease = std::shared_ptr<const GeometryPipelineEntry>;
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
             static_cast<uint32_t>(root.originalParameters.size()), root.originalParameters.data() };
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
