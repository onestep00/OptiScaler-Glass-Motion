#pragma once
#include "ExperimentAbi.h"
#include "ExperimentPipelineAbi.h"

struct GlassExperimentObject
{
    uint64_t proxy, mesh;
    uint32_t slot, generation, first, count, transformIndex, globalRange;
};
struct GlassExperimentMesh
{
    uint64_t chunkAddress;
    uint32_t vertexBuffer, indexBuffer, vertices, indices, streams, indexOffset;
    uint32_t streamOffsets[5], indexType, vertexFactory;
};
// Borrowed observation only: no GPU commands or retained raw pointers through
// this payload. The explicit pipeline service may retain immutable compiler
// inputs only. GPU capture uses the separately owned draw-preparation seam.
struct GlassExperimentDrawInput
{
    uint32_t size, descriptorBytes;
    uint64_t recording, pipelineIdentity, mesh;
    void* command;
    void* originalPipeline;
    void* originalRoot;
    const void* descriptor; // D3D12_GRAPHICS_PIPELINE_STATE_DESC, nullable
    uint32_t chunk, indices, instances, startIndex;
    int32_t baseVertex;
    uint32_t startInstance, objectCount, rasterKnown, rootReplayable;
    float viewport[6];
    int32_t scissor[4];
    uint64_t renderTargets[8], depthTarget;
    uint32_t renderTargetCount, reserved;
    const void* source;
    int32_t (*objectAt)(const void*, uint32_t, GlassExperimentObject*);
    int32_t (*meshShape)(const void*, GlassExperimentMesh*);
    GlassExperimentPipelineAccess pipelineAccess;
};
