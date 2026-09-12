#pragma once
#include "ExperimentDrawAbi.h"

#define GLASS_EXPERIMENT_CENSUS_VERSION 2u
enum GlassExperimentDrawOperation
{
    GlassCensusIndexed = 1, GlassCensusInstanced = 2, GlassCensusIndirect = 3
};
enum GlassExperimentRasterFlags
{
    GlassCensusViewport = 1, GlassCensusScissor = 2, GlassCensusTargets = 4,
    GlassCensusPredicated = 8, GlassCensusRenderPass = 16, GlassCensusUnknownState = 32
};
// CPU-only observation before the original API call. No GPU work/retention is
// permitted. Sequence is CPU observation order, not submission/execution order.
// Frame is the validated engine draw tick for Cyberpunk, including non-mesh
// draws. Zero means no engine frame was established. Unknown recording stays zero.
struct GlassExperimentCensusInput
{
    uint32_t size, operation, rasterFlags, reserved;
    uint64_t sequence, callsite;
    // For non-indexed draws, draw.indices/startIndex mean vertex count/start.
    // For indirect, draw argument counts are zero: maxCommands is only an upper
    // bound. Buffers/signature are numeric observations, never retained handles.
    uint64_t signature, arguments, argumentOffset, counter, counterOffset;
    uint32_t maxCommands, reserved2;
    GlassExperimentDrawInput draw;
};
