#pragma once
// CPU cost probe and idle gate for the D3D12 and engine hooks.
//
// Measured 2026-09-16 (2560x1440, 29 engine fps): the hooks see ~26,000
// command-list calls and ~12,000 engine draw callbacks per engine frame, so
// timing every call would cost more than the code it measures. Every call is
// counted with one thread-local increment and one call in HookCostSampling is
// timed with the TSC. The 2 s health thread sums the per-thread slots and
// reports milliseconds of hook time per engine frame.
//
// Two properties keep the probe honest:
//   * Each thread owns one cache line, so the hot path never contends and the
//     counters are plain relaxed loads/stores rather than locked increments.
//     Locked increments were measured to dominate the number they reported.
//   * A sample longer than HookCostStallCycles is recorded as a stall (a
//     descheduled thread or a lock wait) instead of self time, so a blocking
//     path cannot masquerade as CPU work.
//   * The timer is stopped around the engine's own call (HookCostPause), so the
//     number is our code, not the D3D12 runtime the hook happens to wrap. A
//     scope that wrapped the original call reported the engine's recording cost
//     as hook cost.
//
// One call in HookCostSampling is timed, so the reported total has to be scaled
// by calls/timed before it can be compared with a measured frame cost.
// GlassHookCostMsPerFrame() does that scaling for every report.
//
// Both thread-local counters the hot path touches are constant-initialized
// inline variables. A function-local `static thread_local` with a dynamic
// initializer turns every access into a guard call, which the probe measured
// as part of the code it wraps (26,000 command calls and 12,000 draw callbacks
// per engine frame).
//
// The idle gate keeps "Enable correction = off" at one relaxed load per call:
// the settings layer raises it while the correction is off and every hot hook
// hands the call straight back to the engine.
#include <atomic>
#include <cstdint>
#include <intrin.h>

namespace GlassFg
{
constexpr unsigned HookCostSampling = 64;
// ~33 us at 3 GHz: far above any measured hook body, far below a scheduler
// slice. Samples above it are reported separately as stalls.
constexpr std::uint64_t HookCostStallCycles = 100000;
constexpr unsigned HookCostSlots = 64;

// Stage ids for the diagnostic split of the two dominant hook shapes. The
// switch is off in normal runs; when it is on, sampled calls of those shapes
// pay an extra clock read per stage. Such runs are attribution diagnostics,
// not normal-cost acceptance measurements.
constexpr unsigned HookCostStageCount = 12;
constexpr unsigned HookStageAppendRead = 0;
constexpr unsigned HookStageAppendParent = 1;
constexpr unsigned HookStageAppendTail = 2;
constexpr unsigned HookStageIndexedHead = 3;
constexpr unsigned HookStageIndexedCapture = 4;
constexpr unsigned HookStageIndexedTail = 5;
// Second-level split of the two dominant shapes. The aggregate stages above
// (append_parent, indexed_head) cover the whole phase; these break the same
// bytes of work down so the next optimisation targets the measured part
// instead of the phase. Same 1/64 sample as the aggregate, so they cost
// nothing outside an attribution run.
constexpr unsigned HookStageAppendTicket = 6;
constexpr unsigned HookStageAppendFields = 7;
constexpr unsigned HookStageAppendSelect = 8;
constexpr unsigned HookStageIndexedRead = 9;
constexpr unsigned HookStageIndexedFind = 10;
constexpr unsigned HookStageIndexedPipeline = 11;

inline std::atomic<bool> hookStageTiming { false };
inline bool HookStageTiming() noexcept { return hookStageTiming.load(std::memory_order_relaxed); }
inline void SetHookStageTiming(bool value) noexcept { hookStageTiming.store(value, std::memory_order_relaxed); }

// true = the hooks return immediately. Raised while the correction is disabled
// and until the settings layer has loaded.
inline std::atomic<bool> hookIdle { true };
inline bool HooksIdle() noexcept { return hookIdle.load(std::memory_order_relaxed); }
inline void SetHooksIdle(bool idle) noexcept { hookIdle.store(idle, std::memory_order_relaxed); }

// Diagnostic only, set from the live control channel: stage the command hooks
// are cut back to, so the cost of each stage can be measured in the running
// game without a restart. 0 = full, 1 = keep the record lookup but drop the
// root binding update, 2 = drop the lookup as well. Values above 1 stop the
// correction from working; they exist to attribute cost, not to ship.
//
// 3 and 4 are the cumulative ladder steps (3 = state hooks and geometry
// callbacks, 4 = those and DrawIndexedInstanced). 5 idles DrawIndexedInstanced
// alone: the geometry callbacks keep running, so the module's own frame
// counters stay alive and the indexed share can be taken from the same
// session as the full-cost window. The ladder stops reporting those counters
// from stage 3 on.
inline std::atomic<unsigned> hookSkipMode { 0 };
inline unsigned HookSkipMode() noexcept { return hookSkipMode.load(std::memory_order_relaxed); }
inline void SetHookSkipMode(unsigned value) noexcept { hookSkipMode.store(value, std::memory_order_relaxed); }

constexpr unsigned HookGateState = 2;
constexpr unsigned HookGateDraws = 3;
constexpr unsigned HookGateAll = 4;
constexpr unsigned HookGateIndexed = 5;

// Pure predicates so the gate truth table is asserted at compile time instead
// of being re-derived from the numeric ladder at each call site.
constexpr bool BindingsOnlyFor(unsigned mode) noexcept { return mode == 1; }
constexpr bool SkipStateTrackingFor(unsigned mode) noexcept
{
    return mode == HookGateState || mode == HookGateDraws || mode == HookGateAll;
}
constexpr bool SkipDrawCallbacksFor(unsigned mode) noexcept
{
    return mode == HookGateDraws || mode == HookGateAll;
}
constexpr bool SkipIndexedCallbackFor(unsigned mode) noexcept
{
    return mode == HookGateAll || mode == HookGateIndexed;
}
static_assert(!SkipDrawCallbacksFor(HookGateIndexed) && SkipIndexedCallbackFor(HookGateIndexed),
              "the indexed-only gate has to leave the geometry callbacks running");
static_assert(SkipDrawCallbacksFor(HookGateDraws) && !SkipIndexedCallbackFor(HookGateDraws),
              "the draw gate has to leave DrawIndexedInstanced running");
static_assert(SkipStateTrackingFor(HookGateDraws) && SkipStateTrackingFor(HookGateAll) &&
                  !SkipStateTrackingFor(HookGateIndexed) && !SkipStateTrackingFor(0),
              "only the cumulative ladder stages cut the state hooks");

inline bool DrawHooksIdle() noexcept { return HooksIdle() || SkipDrawCallbacksFor(HookSkipMode()); }
inline bool IndexedHooksIdle() noexcept { return HooksIdle() || SkipIndexedCallbackFor(HookSkipMode()); }

// Sampling window counter, one per thread. Constant (zero) initialization so
// the access is a plain TLS load/store.
//
// The sample is taken from an LCG's high bits rather than a fixed stride. A
// stride of 64 correlates with the engine's own repeating call pattern (the
// reported per-family average of a shape that only occurs at one phase of the
// pattern is then biased by the whole body of that shape), and the low bits of
// a power-of-two LCG have a short period, so the high bits are used.
inline thread_local unsigned hookCostSampleState = 0x9e3779b9u;
inline bool HookCostSampleDue() noexcept
{
    hookCostSampleState = hookCostSampleState * 1664525u + 1013904223u;
    return (hookCostSampleState >> 26) == 0;
}
// This thread's slot. Claimed on the first hook call, then only read.
inline thread_local struct HookCostSlot* hookCostOwnSlot = nullptr;

struct HookCostCounters
{
    std::atomic<std::uint64_t> calls { 0 };
    std::atomic<std::uint64_t> timed { 0 };
    std::atomic<std::uint64_t> selfCycles { 0 };
    std::atomic<std::uint64_t> stalls { 0 };
    std::atomic<std::uint64_t> stallCycles { 0 };
    std::atomic<std::uint64_t> maxCycles { 0 };

    // Written only by the owning thread, read and reset by the health thread.
    // `sample` is false for the tail segment of a paused call: the call was
    // already counted as one sample, only its cycles are added.
    void note(std::uint64_t count, std::uint64_t self, std::uint64_t stall, bool sample = true) noexcept
    {
        this->calls.store(this->calls.load(std::memory_order_relaxed) + count, std::memory_order_relaxed);
        if (!self && !stall)
            return;
        if (sample)
            timed.store(timed.load(std::memory_order_relaxed) + 1, std::memory_order_relaxed);
        const auto elapsed = self + stall;
        if (elapsed > maxCycles.load(std::memory_order_relaxed))
            maxCycles.store(elapsed, std::memory_order_relaxed);
        if (stall)
        {
            stalls.store(stalls.load(std::memory_order_relaxed) + 1, std::memory_order_relaxed);
            stallCycles.store(stallCycles.load(std::memory_order_relaxed) + stall, std::memory_order_relaxed);
        }
        else
            selfCycles.store(selfCycles.load(std::memory_order_relaxed) + self, std::memory_order_relaxed);
    }

    void clear() noexcept
    {
        calls.store(0, std::memory_order_relaxed);
        timed.store(0, std::memory_order_relaxed);
        selfCycles.store(0, std::memory_order_relaxed);
        stalls.store(0, std::memory_order_relaxed);
        stallCycles.store(0, std::memory_order_relaxed);
        maxCycles.store(0, std::memory_order_relaxed);
    }
};

// One cache line per thread. The owner id claims the slot; a thread that finds
// every slot taken falls back to slot 0, which costs accuracy, never safety.
struct HookCostSlot
{
    alignas(64) std::atomic<std::uint32_t> owner { 0 };
    HookCostCounters command;
    // The command-list family is split so each engine phase is attributed on
    // its own: the root/binding setters are one call shape and
    // DrawIndexedInstanced is another (it carries the per-draw capture work).
    HookCostCounters indexed;
    // The engine geometry callbacks. `draw` in the report is their sum; the
    // split names which callback owns the cost.
    HookCostCounters run;
    HookCostCounters append;
    HookCostCounters rigid;
    HookCostCounters skinned;
    HookCostCounters observer;
    // Stage split inside the two dominant hook shapes (append, indexed). Used
    // only while HookStageTiming() is on; every stage pays one relaxed add then.
    HookCostCounters stages[HookCostStageCount];
    // Keep the next slot's owner on its own line.
    std::uint64_t padding[6] {};
};
inline HookCostSlot hookCostSlots[HookCostSlots];

inline HookCostSlot* ClaimHookCostSlot() noexcept
{
    const auto id = static_cast<std::uint32_t>(GetCurrentThreadId());
    for (unsigned i = 0; i < HookCostSlots; ++i)
    {
        std::uint32_t expected = 0;
        if (hookCostSlots[i].owner.compare_exchange_strong(expected, id, std::memory_order_relaxed))
            return &hookCostSlots[i];
    }
    return &hookCostSlots[0];
}

// Command-list hooks (state capture and the draw callbacks).
inline HookCostCounters& CommandHookCost() noexcept
{
    auto* slot = hookCostOwnSlot;
    if (!slot)
        slot = hookCostOwnSlot = ClaimHookCostSlot();
    return slot->command;
}
// DrawIndexedInstanced: state lookup plus the per-draw capture path.
inline HookCostCounters& IndexedHookCost() noexcept
{
    auto* slot = hookCostOwnSlot;
    if (!slot)
        slot = hookCostOwnSlot = ClaimHookCostSlot();
    return slot->indexed;
}
// Engine instance/geometry callbacks that feed object identity.
inline HookCostCounters& RunHookCost() noexcept
{
    auto* slot = hookCostOwnSlot;
    if (!slot)
        slot = hookCostOwnSlot = ClaimHookCostSlot();
    return slot->run;
}
inline HookCostCounters& AppendHookCost() noexcept
{
    auto* slot = hookCostOwnSlot;
    if (!slot)
        slot = hookCostOwnSlot = ClaimHookCostSlot();
    return slot->append;
}
inline HookCostCounters& RigidHookCost() noexcept
{
    auto* slot = hookCostOwnSlot;
    if (!slot)
        slot = hookCostOwnSlot = ClaimHookCostSlot();
    return slot->rigid;
}
inline HookCostCounters& SkinnedHookCost() noexcept
{
    auto* slot = hookCostOwnSlot;
    if (!slot)
        slot = hookCostOwnSlot = ClaimHookCostSlot();
    return slot->skinned;
}
// Command-queue and command-list observer hooks (state setters, resets, submissions).
inline HookCostCounters& ObserverHookCost() noexcept
{
    auto* slot = hookCostOwnSlot;
    if (!slot)
        slot = hookCostOwnSlot = ClaimHookCostSlot();
    return slot->observer;
}
// Stage counters for the thread's current call shape.
inline HookCostCounters& StageHookCost(unsigned stage) noexcept
{
    auto* slot = hookCostOwnSlot;
    if (!slot)
        slot = hookCostOwnSlot = ClaimHookCostSlot();
    return slot->stages[stage < HookCostStageCount ? stage : HookCostStageCount - 1];
}

struct HookCostTotals
{
    unsigned long long calls = 0, timed = 0, selfCycles = 0, stalls = 0, stallCycles = 0, maxCycles = 0;
};

// Sums and resets one family across every thread slot. Called once per 2 s
// report by the health thread; a slot being written concurrently can lose that
// increment, which is irrelevant for a rate estimate.
inline HookCostTotals TakeHookCost(HookCostCounters HookCostSlot::* family) noexcept
{
    HookCostTotals total;
    for (auto& slot : hookCostSlots)
    {
        auto& counters = slot.*family;
        total.calls += counters.calls.exchange(0, std::memory_order_relaxed);
        total.timed += counters.timed.exchange(0, std::memory_order_relaxed);
        total.selfCycles += counters.selfCycles.exchange(0, std::memory_order_relaxed);
        total.stalls += counters.stalls.exchange(0, std::memory_order_relaxed);
        total.stallCycles += counters.stallCycles.exchange(0, std::memory_order_relaxed);
        total.maxCycles = (std::max)(total.maxCycles, counters.maxCycles.exchange(0, std::memory_order_relaxed));
    }
    return total;
}

// Sums several families after they have been taken. Report-only, once per 2 s.
inline HookCostTotals SumHookCosts(const HookCostTotals& first, const HookCostTotals& second) noexcept
{
    HookCostTotals total;
    total.calls = first.calls + second.calls;
    total.timed = first.timed + second.timed;
    total.selfCycles = first.selfCycles + second.selfCycles;
    total.stalls = first.stalls + second.stalls;
    total.stallCycles = first.stallCycles + second.stallCycles;
    total.maxCycles = (std::max)(first.maxCycles, second.maxCycles);
    return total;
}

// One time-base conversion for every counter. Calibrated once on a worker
// thread before any hook can time a call; QPC and the TSC both advance at a
// constant rate on the supported configuration.
inline double TscHertz() noexcept
{
    static const double value = [] {
        LARGE_INTEGER frequency {}, start {}, end {};
        if (!QueryPerformanceFrequency(&frequency) || !QueryPerformanceCounter(&start) ||
            frequency.QuadPart <= 0)
            return 3.0e9;
        const auto begin = __rdtsc();
        Sleep(20);
        QueryPerformanceCounter(&end);
        const auto cycles = static_cast<double>(__rdtsc() - begin);
        const auto seconds =
            static_cast<double>(end.QuadPart - start.QuadPart) / static_cast<double>(frequency.QuadPart);
        return seconds > 0.0 ? cycles / seconds : 3.0e9;
    }();
    return value;
}
inline void WarmHookCostProbe() noexcept { (void)TscHertz(); }

// Times the enclosing scope on every HookCostSampling-th call. `sampled` is the
// outermost-hook flag, so a bundled or re-entrant call is never counted twice.
// Stop()/Resume() exclude a nested original call without ending the sample.
class HookCostScope
{
public:
    HookCostScope(HookCostCounters& counters, bool sampled) noexcept : counters_(&counters)
    {
        sample_ = sampled && HookCostSampleDue();
        if (sample_)
        {
            // One per call: the sampled call counts itself, the other calls in
            // the window counted themselves. Scaling by calls/timed then gives
            // the total for every call.
            calls_ = 1;
            begin_ = __rdtsc();
        }
        else if (sampled)
            counters_->note(1, 0, 0);
    }
    HookCostScope(const HookCostScope&) = delete;
    HookCostScope& operator=(const HookCostScope&) = delete;
    ~HookCostScope() noexcept { Stop(); }
    // True when this call is one of the sampled ones. Stage diagnostics must
    // also be explicitly enabled: instrumentation only on timed samples would
    // otherwise be amplified by calls/timed in the normal cost estimate.
    bool Sampled() const noexcept { return sample_; }
    // const: every hook declares the scope as `const HookCostScope cost(...)`
    // so the counters cannot be rebound by accident in a hook body.
    void Stop() const noexcept
    {
        if (!begin_)
            return;
        const auto elapsed = __rdtsc() - begin_;
        begin_ = 0;
        const auto first = calls_ != 0;
        calls_ = 0;
        if (elapsed >= HookCostStallCycles)
            counters_->note(first ? 1 : 0, 0, elapsed, first);
        else
            counters_->note(first ? 1 : 0, elapsed, 0, first);
    }
    void Resume() const noexcept
    {
        if (sample_ && !begin_)
            begin_ = __rdtsc();
    }

private:
    HookCostCounters* counters_ = nullptr;
    mutable std::uint64_t begin_ = 0;
    mutable std::uint64_t calls_ = 0;
    bool sample_ = false;
};

// Excludes a nested call (the engine's own implementation) from the sample.
// The scope keeps running afterwards, so a hook that continues after the
// original call is still measured.
class HookCostPause
{
public:
    explicit HookCostPause(const HookCostScope& scope) noexcept : scope_(&scope) { scope_->Stop(); }
    HookCostPause(const HookCostPause&) = delete;
    HookCostPause& operator=(const HookCostPause&) = delete;
    ~HookCostPause() noexcept { scope_->Resume(); }

private:
    const HookCostScope* scope_;
};

// Splits one hook body into named stages for the attribution run. Each Split
// charges the cycles since the previous split to that stage and records that
// the stage ran. Report-only: the counts are per sample, and the value printed
// is cycles per sample, so a stage that only runs sometimes is still readable.
class HookStageTimer
{
public:
    explicit HookStageTimer(bool active) noexcept : active_(active)
    {
        if (active_)
            last_ = __rdtsc();
    }
    HookStageTimer(const HookStageTimer&) = delete;
    HookStageTimer& operator=(const HookStageTimer&) = delete;
    void Split(unsigned stage) noexcept
    {
        if (!active_)
            return;
        const auto now = __rdtsc();
        auto& counters = StageHookCost(stage);
        counters.calls.store(counters.calls.load(std::memory_order_relaxed) + 1, std::memory_order_relaxed);
        counters.selfCycles.store(counters.selfCycles.load(std::memory_order_relaxed) + (now - last_),
                                  std::memory_order_relaxed);
        last_ = now;
    }

private:
    bool active_ = false;
    std::uint64_t last_ = 0;
};

// Sums and resets the stage counters across every thread slot. Report-only.
struct HookStageTotals
{
    unsigned long long calls[HookCostStageCount] {};
    unsigned long long cycles[HookCostStageCount] {};
};
inline HookStageTotals TakeHookStages() noexcept
{
    HookStageTotals total;
    for (auto& slot : hookCostSlots)
        for (unsigned stage = 0; stage < HookCostStageCount; ++stage)
        {
            total.calls[stage] += slot.stages[stage].calls.exchange(0, std::memory_order_relaxed);
            total.cycles[stage] += slot.stages[stage].selfCycles.exchange(0, std::memory_order_relaxed);
        }
    return total;
}

// One place for the scaling above. `calls` and `timed` come from the same
// window as the cycles.
inline double HookCostScaledMs(unsigned long long cycles, unsigned long long calls, unsigned long long timed) noexcept
{
    if (!cycles || !calls || !timed)
        return 0.0;
    const auto scale = static_cast<double>(calls) / static_cast<double>(timed);
    return static_cast<double>(cycles) * scale / TscHertz() * 1000.0;
}
} // namespace GlassFg
