#pragma once
#include "ExperimentCensusAbi.h"
#include "ExperimentDrawBridge.h"

namespace GlassFg
{
// Registered storage/callbacks belong to the resident host, not the module.
struct ExperimentCensusObserver
{
    void* context;
    bool (*enabled)(void*) noexcept;
    void (*event)(void*, const GlassExperimentEvent&) noexcept;
};
inline std::atomic<const ExperimentCensusObserver*> experimentCensusObserver = nullptr;
inline std::atomic<uint64_t> experimentCensusSequence = 0;
inline bool RegisterExperimentCensus(const ExperimentCensusObserver* observer)
{
    if (!observer || !observer->enabled || !observer->event) return false;
    const ExperimentCensusObserver* expected = nullptr;
    return experimentCensusObserver.compare_exchange_strong(expected, observer) || expected == observer;
}
inline const ExperimentCensusObserver* ActiveExperimentCensus() noexcept
{
    auto* observer = experimentCensusObserver.load(std::memory_order_acquire);
    return observer && observer->enabled(observer->context) ? observer : nullptr;
}
inline void ObserveExperimentCensus(const ExperimentCensusObserver& observer,
                                    GlassExperimentCensusInput& input, uint64_t frame,
                                    const GeometryRasterState* raster) noexcept
{
    input.size = sizeof(input); input.sequence = ++experimentCensusSequence;
    if (raster)
        input.rasterFlags = (raster->viewportKnown ? GlassCensusViewport : 0u) |
            (raster->scissorKnown ? GlassCensusScissor : 0u) | (raster->targetsKnown ? GlassCensusTargets : 0u) |
            (raster->predicate ? GlassCensusPredicated : 0u) | (raster->renderPass ? GlassCensusRenderPass : 0u) |
            (raster->unknown ? GlassCensusUnknownState : 0u);
    const GlassExperimentEvent event { sizeof(event), GlassExperimentCensus, frame, 0, 0,
                                       GLASS_EXPERIMENT_CENSUS_VERSION, sizeof(input), &input };
    observer.event(observer.context, event);
}
} // namespace GlassFg
