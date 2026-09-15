#pragma once
#include <atomic>

namespace GlassFg
{
// Bounded step trace shared by the host, the session and the packed pass. Every
// trace line is written once per frame per step, so an unthrottled stream grew
// the module log to tens of megabytes in one session. A short burst keeps
// attribution for the first frames after a change; afterwards the trace is
// sampled. This lives in its own header so a fixture that only compiles the
// packed pass does not have to pull in the surface pass.
inline bool TraceWanted() noexcept
{
    static std::atomic<unsigned> burst { 0 };
    if (burst.load(std::memory_order_relaxed) < 2000)
        return burst.fetch_add(1, std::memory_order_relaxed) < 2000;
    static std::atomic<unsigned> sampled { 0 };
    return (sampled.fetch_add(1, std::memory_order_relaxed) % 240) == 0;
}
} // namespace GlassFg
