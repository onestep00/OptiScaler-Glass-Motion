#include "pch.h"
#include "../GlassHookProbe.h"
#include <cstdio>
#include <stdexcept>

// Offline contract for the diagnostic hookskip gate. Live family attribution
// depends on which hooks each stage cuts, and the stages are not a single
// ordered ladder any more (5 idles only DrawIndexedInstanced), so the table is
// pinned here instead of being re-derived from the numbers during a live
// window.
namespace
{
using namespace GlassFg;

int checks = 0;

void Require(bool value, const char* what)
{
    ++checks;
    if (!value) throw std::runtime_error(what);
}

struct GateRow
{
    unsigned mode;
    bool bindingsOnly;
    bool skipState;
    bool skipDraws;
    bool skipIndexed;
};

const GateRow kRows[] = {
    { 0, false, false, false, false },
    { 1, true, false, false, false },
    { HookGateState, false, true, false, false },
    { HookGateDraws, false, true, true, false },
    { HookGateAll, false, true, true, true },
    { HookGateIndexed, false, false, false, true },
};
} // namespace

int main()
{
    using namespace GlassFg;
    SetHooksIdle(false);
    for (const auto& row : kRows)
    {
        SetHookSkipMode(row.mode);
        Require(BindingsOnlyFor(row.mode) == row.bindingsOnly, "bindings predicate");
        Require(SkipStateTrackingFor(row.mode) == row.skipState, "state predicate");
        Require(SkipDrawCallbacksFor(row.mode) == row.skipDraws, "draw predicate");
        Require(SkipIndexedCallbackFor(row.mode) == row.skipIndexed, "indexed predicate");
        Require(DrawHooksIdle() == row.skipDraws, "draw wrapper follows the gate");
        Require(IndexedHooksIdle() == row.skipIndexed, "indexed wrapper follows the gate");
        Require(HookSkipMode() == row.mode, "mode round trip");
    }
    // The idle latch outranks every stage, and the measurement path has to be
    // able to restore the live configuration.
    SetHooksIdle(true);
    SetHookSkipMode(0);
    Require(DrawHooksIdle() && IndexedHooksIdle(), "idle latch");
    SetHooksIdle(false);
    SetHookSkipMode(0);
    Require(!DrawHooksIdle() && !IndexedHooksIdle(), "restored off");

    std::printf("HOOK_GATE_OK modes=%d checks=%d indexed_only_keeps_draws=1 state_gate_unchanged=1\n",
                static_cast<int>(sizeof(kRows) / sizeof(kRows[0])), checks);
    return 0;
}
