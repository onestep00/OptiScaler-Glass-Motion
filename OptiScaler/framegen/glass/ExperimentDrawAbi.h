#pragma once
#include "ExperimentAbi.h"
#include "ExperimentPipelineAbi.h"
inline constexpr uint32_t GLASS_EXPERIMENT_DRAW_VERSION = 4;

struct GlassExperimentBinding
{
    uint32_t size, type;
    uint64_t address, knownConstants;
    uint32_t constants[64]; // Only knownConstants bits are valid; other words are zero.
};

struct GlassExperimentTarget
{
    uint32_t size, kind, defaultDescriptor, nullResource;
    uint64_t handle, heap, revision, resource, address;
    uint32_t allocationBytes, descriptorBytes;
    uint8_t allocation[64]; // Exact D3D12_RESOURCE_DESC bytes, not a retained resource.
    uint8_t descriptor[32]; // Exact explicit RTV/DSV descriptor; zero bytes for default.
};

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
    const void* targetSource;
    // Callback-scoped OM binding snapshots: 0..7 RTV, 8 DSV. Unknown returns 0.
    int32_t (*targetAt)(const void*, uint32_t, GlassExperimentTarget*);
    const void* bindingSource;
    // Synchronous CPU metadata only. Addresses are not retained GPU resources.
    int32_t (*bindingAt)(const void*, uint32_t, GlassExperimentBinding*);
};
