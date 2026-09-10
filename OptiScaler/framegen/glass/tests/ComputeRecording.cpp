#include "pch.h"
#include "../ComputeRecording.h"
#include <cstdio>
#include <stdexcept>

static void check(bool ok)
{
    if (!ok)
        throw std::runtime_error("compute recording contract failed");
}

int main()
{
    using Gate = GlassFg::ComputeRecording;
    Gate gate;
    int command = 0, other = 0, pipeline = 0;
    // Losing any callback, including predication or newer state objects, must
    // reject insertion even if the remaining observations all look fresh.
    for (unsigned method = 0; method < 16; ++method)
    {
        gate.bind(&command, true, Gate::AllMethods, Gate::AllMethods & ~(1u << method));
        gate.onReset(&command, true, nullptr);
        check(!gate.begin(&command));
    }
    gate.bind(&command, true, Gate::AllMethods, Gate::AllMethods);
    check(!gate.begin(&command)); // Installing hooks mid-recording is not Reset.
    gate.onReset(&command, false, nullptr);
    check(!gate.begin(&command));
    gate.onReset(&command, true, &pipeline);
    check(!gate.begin(&command));
    gate.onReset(&command, true, nullptr);
    check(!gate.begin(&other));
    gate.onMutation(&other);
    auto first = gate.begin(&command);
    check(bool(first) && !gate.begin(&command));
    check(gate.finish(&command, first) && !gate.finish(&command, first));
    check(!gate.begin(&command)); // At most one insertion per fresh recording.
    gate.onReset(&command, true, nullptr);
    auto second = gate.begin(&command);
    check(bool(second) && !gate.finish(&command, first));
    gate.onMutation(&command); // Interleaved application state invalidates it.
    check(!gate.finish(&command, second));
    gate.onReset(&command, true, nullptr);
    auto discarded = gate.begin(&command);
    gate.onReset(&command, true, nullptr);
    auto current = gate.begin(&command);
    check(!gate.finish(&command, discarded) && gate.finish(&command, current));
    gate.onReset(&command, true, nullptr);
    auto oldIdentity = gate.begin(&command);
    gate.bind(&other, true, Gate::BaseMethods, Gate::BaseMethods);
    check(!gate.finish(&command, oldIdentity) && !gate.begin(&other));
    gate.onReset(&other, true, nullptr);
    auto base = gate.begin(&other);
    check(bool(base) && gate.finish(&other, base));
    gate.bind(&command, false, Gate::AllMethods, Gate::AllMethods);
    gate.onReset(&command, true, nullptr);
    check(!gate.begin(&command)); // DIRECT/bundles require another state contract.
    std::puts("COMPUTE_RECORDING_OK missing_hooks=16 stale_tickets=3 unknown_and_noncompute_rejected=1");
}
