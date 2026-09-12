#include "../DiagnosticCallerContext.h"
#include <cstdio>

struct Observation { CONTEXT context {}; unsigned steps = 0; };
extern "C" bool DiagnosticCallerFixture(std::uint64_t, std::uint64_t, std::uint64_t, Observation*);
extern "C" __declspec(noinline) bool DiagnosticCallerObserve(std::uint64_t target, Observation* result)
{
    const auto accepted = GlassFg::DiagnosticCallerContext(target, result->context, result->steps);
    // Keep an actual intervening frame in optimized builds as well.
    volatile auto steps = result->steps;
    return accepted && steps > 0;
}
int main()
{
    unsigned maximum = 0;
    for (unsigned i = 0; i < 100; ++i)
    {
        Observation result;
        if (!DiagnosticCallerFixture(0x10001000ULL + i, 0x20002000ULL + i,
                                     0x30003000ULL + i, &result) ||
            result.context.Rdi != 0x10001000ULL + i || result.context.Rbx != 0x20002000ULL + i ||
            result.context.R14 != 0x30003000ULL + i || !result.context.Rip || !result.context.Rsp)
            return 1;
        if (result.steps > maximum) maximum = result.steps;
    }
    Observation rejected;
    if (GlassFg::DiagnosticCallerContext(0, rejected.context, rejected.steps) ||
        GlassFg::DiagnosticCallerContext(1, rejected.context, rejected.steps)) return 2;
    std::printf("PASS caller_context=1 independent_asm_registers=300 max_unwind_steps=%u "
                "missing_caller_rejected=1 game_hooks=0\n", maximum);
}
