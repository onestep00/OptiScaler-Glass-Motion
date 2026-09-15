#pragma once
#include <atomic>
#include <chrono>
#include <cstdint>

namespace GlassFg
{
// Wall clock seconds, for correlating a log line with the Windows event log.
// Only the 2 s health line and the bounded stall lines call this; the per-frame
// path never does.
inline double EpochSeconds() noexcept
{
    return std::chrono::duration<double>(std::chrono::system_clock::now().time_since_epoch()).count();
}

// CPU cost of the two host callbacks that run inside the engine's frame
// generation path, in microseconds. The counters are only ever incremented with
// relaxed atomics from the render thread and formatted by the health thread, so
// no allocation, formatting or locking happens per frame.
struct HostTiming
{
    std::atomic<std::uint64_t> count { 0 };
    std::atomic<std::uint64_t> totalUs { 0 };
    std::atomic<std::uint64_t> maxUs { 0 };

    void add(std::uint64_t microseconds) noexcept
    {
        count.fetch_add(1, std::memory_order_relaxed);
        totalUs.fetch_add(microseconds, std::memory_order_relaxed);
        auto previous = maxUs.load(std::memory_order_relaxed);
        while (microseconds > previous &&
               !maxUs.compare_exchange_weak(previous, microseconds, std::memory_order_relaxed))
        {
        }
    }
    void reset() noexcept
    {
        count.store(0, std::memory_order_relaxed);
        totalUs.store(0, std::memory_order_relaxed);
        maxUs.store(0, std::memory_order_relaxed);
    }
};

// Session acquisition, NGX parameter read and the deferred compose preparation.
inline HostTiming& EvaluationTiming() noexcept
{
    static HostTiming value;
    return value;
}
// Pre-submit hook: producer wait plus the compose submission.
inline HostTiming& SubmissionTiming() noexcept
{
    static HostTiming value;
    return value;
}
// Pre-submit hook split: the packed capture's own command list submission and
// the deferred compose. Both run inside the engine's ExecuteCommandLists call,
// so a stall in either one is a stall of the engine's submission.
inline HostTiming& CaptureTiming() noexcept
{
    static HostTiming value;
    return value;
}
inline HostTiming& ComposeTiming() noexcept
{
    static HostTiming value;
    return value;
}
// Inside the pre-submit hook: the command recording and the submission of the
// deferred compose (allocator reset, list recording, ExecuteCommandLists,
// Signal). The surrounding ComposeTiming also covers the producer hand-off, so
// the difference between the two attributes a block to the driver submission
// instead of to a descheduled render thread.
inline HostTiming& ComposeQueueTiming() noexcept
{
    static HostTiming value;
    return value;
}
// Inside ComposeQueueTiming: the individual driver calls of one compose
// submission. A 17,6 s block on the frame generation queue was measured inside
// this scope, so the phases are split to attribute the next one: the allocator
// and list Reset, the recording, the submission and its Signal.
inline HostTiming& ComposeResetTiming() noexcept
{
    static HostTiming value;
    return value;
}
inline HostTiming& ComposeRecordTiming() noexcept
{
    static HostTiming value;
    return value;
}
inline HostTiming& ComposeExecuteTiming() noexcept
{
    static HostTiming value;
    return value;
}
inline HostTiming& ComposeSignalTiming() noexcept
{
    static HostTiming value;
    return value;
}
} // namespace GlassFg
