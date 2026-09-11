#include "../ExperimentAbi.h"
#include <windows.h>
#include <new>
#ifndef FIXTURE_ID
#define FIXTURE_ID 1
#endif
namespace
{
int32_t create(const GlassExperimentHost*, void** context)
{
    *context = new (std::nothrow) int(FIXTURE_ID);
    return FIXTURE_ID == 99 || !*context ? -1 : 0;
}
int32_t event(void* context, const GlassExperimentEvent* value)
{
    if (value->payloadVersion == 99 && value->payloadBytes == 2 * sizeof(HANDLE))
    {
        const auto* events = static_cast<const HANDLE*>(value->payload);
        SetEvent(events[0]);
        WaitForSingleObject(events[1], 10000); // Test-only blocked CPU callback.
    }
    return *static_cast<int*>(context);
}
void destroy(void* context) { delete static_cast<int*>(context); }
const GlassExperimentApi api { sizeof(api), FIXTURE_ID == 98 ? 999u : GLASS_EXPERIMENT_ABI,
                              GlassExperimentDraw | GlassExperimentFg | GlassExperimentCensus, create, event, destroy };
}
extern "C" __declspec(dllexport) const GlassExperimentApi* GlassExperimentQuery() { return &api; }
