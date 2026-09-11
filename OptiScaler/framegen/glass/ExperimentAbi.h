#pragma once
#include <stdint.h>

// Owned experiment modules only. No STL, exceptions, allocation ownership or
// private engine layout crosses this boundary. Export: GlassExperimentQuery.
#define GLASS_EXPERIMENT_ABI 1u
enum GlassExperimentEventKind
{
    GlassExperimentDraw = 1,
    GlassExperimentFg = 2,
    GlassExperimentCapture = 4,
    GlassExperimentCensus = 8,
    GlassExperimentSubmission = 16
};
struct GlassExperimentHost
{
    uint32_t size, abi;
    uint64_t capabilities;
    void* device; // borrowed ID3D12Device; module retains it if needed
};
struct GlassExperimentEvent
{
    uint32_t size, kind;
    uint64_t frame;
    uint32_t phase, phaseCount, payloadVersion, payloadBytes;
    const void* payload; // borrowed, valid only for this callback
};
struct GlassExperimentApi
{
    uint32_t size, abi;
    uint64_t capabilities;
    int32_t (*create)(const GlassExperimentHost*, void** context);
    int32_t (*event)(void* context, const GlassExperimentEvent*);
    // Called after no frame/recording leases remain. Must stop/join private
    // jobs and release resources before returning. Never called in DllMain.
    void (*destroy)(void* context);
};
typedef const GlassExperimentApi* (*GlassExperimentQueryFn)(void);
