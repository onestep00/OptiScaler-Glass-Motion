#pragma once
#include "ExperimentDrawAbi.h"
#include "ExperimentPipelineService.h"
#include "GeometryDrawCapture.h"
#include "GeometryRasterState.h"
#include "CyberpunkDraws.h"
#include <atomic>

namespace GlassFg
{
// Handler must live in the resident host, never in a replaceable module.
using ExperimentDrawObserver = void (*)(const GlassExperimentEvent&) noexcept;
inline std::atomic<ExperimentDrawObserver> experimentDrawObserver = nullptr;
inline bool RegisterExperimentDrawObserver(ExperimentDrawObserver handler)
{
    if (!handler) return false;
    ExperimentDrawObserver expected = nullptr;
    return experimentDrawObserver.compare_exchange_strong(expected, handler) || expected == handler;
}
inline int32_t ExperimentObjectAt(const void* source, uint32_t index, GlassExperimentObject* out)
{
    if (!source || !out) return 0;
    const auto& draw = *static_cast<const GeometryDrawView*>(source);
    if (index >= draw.objects.size()) return 0;
    const auto& object = draw.objects[index];
    const auto& id = object.identity;
    *out = { id.proxy, id.mesh, id.slot, id.generation, object.first, object.count, object.transformIndex,
             object.global ? 1u : 0u };
    return 1; // Unknown identity remains zero; its original interval survives.
}
inline int32_t ExperimentMeshShape(const void* source, GlassExperimentMesh* out)
{
    if (!source || !out) return 0;
    *out = {};
    const auto shape = ReadCyberpunkMeshShape(*static_cast<const GeometryDrawView*>(source));
    if (!shape) return 0;
    out->chunkAddress = shape.chunkAddress;
    out->vertexBuffer = shape.vertexBuffer; out->indexBuffer = shape.indexBuffer;
    out->vertices = shape.vertices; out->indices = shape.indices; out->streams = shape.streams;
    out->indexOffset = shape.indexOffset; out->indexType = shape.indexType; out->vertexFactory = shape.vertexFactory;
    for (unsigned i = 0; i < 5; ++i) out->streamOffsets[i] = shape.streamOffsets[i];
    return 1;
}
inline GlassExperimentDrawInput MakeExperimentDrawInput(ID3D12GraphicsCommandList* command, uint64_t recording,
                                  const GeometryDrawView& draw, const GeometryIndexedArguments& args,
                                  const GeometryRasterState& raster, const GraphicsRootBindings& bindings,
                                  const ExperimentPipelineLease& pipeline) noexcept
{
    GlassExperimentDrawInput input {};
    input.size = sizeof(input); input.command = command; input.recording = recording;
    input.mesh = draw.mesh; input.chunk = draw.chunk;
    input.originalPipeline = bindings.pipeline; input.originalRoot = bindings.root;
    input.indices = args.indices; input.instances = args.instances;
    input.startIndex = args.startIndex; input.baseVertex = args.baseVertex; input.startInstance = args.startInstance;
    input.objectCount = static_cast<uint32_t>(draw.objects.size());
    input.rasterKnown = raster.usable();
    if (pipeline)
    {
        input.pipelineAccess = BorrowExperimentPipeline(pipeline);
        input.pipelineIdentity = pipeline->identity;
        input.descriptor = &pipeline->description; input.descriptorBytes = sizeof(pipeline->description);
        input.rootReplayable = bindings.canReplay(*pipeline->root, pipeline->original.Get());
    }
    if (input.rasterKnown)
    {
        const auto& v = raster.viewport; const auto& s = raster.scissor;
        const float viewport[] { v.TopLeftX, v.TopLeftY, v.Width, v.Height, v.MinDepth, v.MaxDepth };
        for (unsigned i = 0; i < 6; ++i) input.viewport[i] = viewport[i];
        input.scissor[0] = s.left; input.scissor[1] = s.top; input.scissor[2] = s.right; input.scissor[3] = s.bottom;
        input.renderTargetCount = raster.targetCount; input.depthTarget = raster.depth.ptr;
        for (unsigned i = 0; i < raster.targetCount; ++i) input.renderTargets[i] = raster.targets[i].ptr;
    }
    input.source = &draw; input.objectAt = ExperimentObjectAt; input.meshShape = ExperimentMeshShape;
    return input;
}
inline void ObserveExperimentDraw(ID3D12GraphicsCommandList* command, uint64_t recording,
                                  const GeometryDrawView& draw, const GeometryIndexedArguments& args,
                                  const GeometryRasterState& raster, const GraphicsRootBindings& bindings,
                                  const ExperimentPipelineLease& pipeline) noexcept
{
    const auto observer = experimentDrawObserver.load(std::memory_order_acquire);
    if (!observer) return;
    const auto input = MakeExperimentDrawInput(command, recording, draw, args, raster, bindings, pipeline);
    const GlassExperimentEvent event { sizeof(event), GlassExperimentDraw, draw.frame, 0, 0, 2,
                                       sizeof(input), &input };
    observer(event);
}
} // namespace GlassFg
