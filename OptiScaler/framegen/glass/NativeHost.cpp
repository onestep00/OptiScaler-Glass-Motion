#include "pch.h"
#include "GeometryDrawCapture.h"
#include "GeometryCreation.h"
#include "GeometryHealth.h"
#include "NativeHost.h"
#include "NativeSession.h"
#include "FgOutputDump.h"
#include "D3D12Observer.h"
#include "StreamlineTagBridge.h"
#include "PackedMotionCapture.h"
#include "GlassMotionIdentity.h"
#include "GlassDebugControl.h"
#include "NvngxDlssgBridge.h"
#include "NgxParameterProbe.h"
#include "GeometryHealth.h"
#include "GlassHostTiming.h"
#include "GeometryCommands.h"
#include "GlassControls.h"
#include <hooks/Streamline_Hooks.h>
#include <Util.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <memory>
#include <mutex>
#include <thread>
#include <wrl/client.h>

namespace GlassFg
{
namespace
{
// Rotation limit for the module log. Enforced at open and again during the
// session, because the step trace keeps writing while the game runs.
constexpr std::uintmax_t kLogLimit = 32ull * 1024 * 1024;
// Per-callback CPU cost, reported once per health sample. Two steady_clock
// reads and one relaxed atomic add are the whole cost on the normal path.
struct TimingScope
{
    HostTiming& timing;
    std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
    explicit TimingScope(HostTiming& value) : timing(value) {}
    ~TimingScope()
    {
        timing.add(static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - start)
                .count()));
    }
};

struct Entry
{
    NativeSession session;
    const NVSDK_NGX_Handle* handle = nullptr;
    ID3D12GraphicsCommandList* command = nullptr;
    D3D12_RESOURCE_DESC descriptions[3] {};
    // Frame generation input identities of the run this session was created
    // for. The session does not own them; they only make "the same frame
    // generation inputs" decidable when the feature instance changes.
    ID3D12Resource* motionInput = nullptr;
    ID3D12Resource* depthInput = nullptr;
    unsigned evaluations = 0;
    bool releaseLogged = false;

    bool matches(ID3D12GraphicsCommandList* candidate, const Inputs& inputs) const
    {
        (void) candidate; // Command identity is owned by the session (one per back buffer).
        ID3D12Resource* resources[] = { inputs.motion, inputs.color, inputs.depth };
        for (unsigned i = 0; i < 3; ++i)
        {
            const auto desc = resources[i]->GetDesc();
            const auto& old = descriptions[i];
            if (desc.Width != old.Width || desc.Height != old.Height || desc.Format != old.Format ||
                desc.Dimension != old.Dimension || desc.SampleDesc.Count != old.SampleDesc.Count ||
                desc.MipLevels != old.MipLevels || desc.DepthOrArraySize != old.DepthOrArraySize)
                return false;
        }
        return true;
    }
};
// Command lists whose Reset a session has to see: every frame generation list
// a session adopted and every list the second consumer composed on. All
// session state keyed by a list identity (known lists, the recording gate, the
// pending compose owners, the packed batch, the inline compose key) is created
// for one of these lists, under Runtime::mutex, while that list is open on the
// calling thread, so no Reset of it can race the add. Read without the lock by
// the observer's Reset predicate. The table is emptied only once no session
// entry remains (reap); a list that does not fit makes every Reset tracked
// until then.
class alignas(64) TrackedLists
{
  public:
    void add(const void* list) noexcept
    {
        const auto used = count.load(std::memory_order_relaxed);
        if (!list || used > Capacity || contains(list))
            return;
        if (used == Capacity)
        {
            count.store(Capacity + 1, std::memory_order_release);
            return;
        }
        lists[used].store(list, std::memory_order_relaxed);
        count.store(used + 1, std::memory_order_release);
    }
    void clear() noexcept
    {
        if (count.load(std::memory_order_relaxed) != 0)
            count.store(0, std::memory_order_release);
    }
    bool contains(const void* list) const noexcept
    {
        const auto used = count.load(std::memory_order_acquire);
        if (used > Capacity)
            return true;
        for (unsigned i = 0; i < used; ++i)
            if (lists[i].load(std::memory_order_relaxed) == list)
                return true;
        return false;
    }

  private:
    static constexpr unsigned Capacity = 64;
    // Published entries, Capacity + 1 once a list did not fit. First, so the
    // usual two to six lists share its cache line.
    std::atomic<unsigned> count { 0 };
    std::array<std::atomic<const void*>, Capacity> lists {};
};
struct Runtime
{
    std::recursive_mutex mutex;
    std::shared_ptr<Entry> active;
    // Read without the lock by the observer predicates: activeCommand on every
    // binding setter of the device (~6k per engine frame), tracked on every
    // Reset (~70). They change only when a session adopts or retires a list or
    // the second consumer composes on a new one, so they are kept off the cache
    // lines that every lock, unlock and evaluation counter writes.
    alignas(64) std::atomic<ID3D12GraphicsCommandList*> activeCommand = nullptr;
    std::atomic<bool> submissionObserved = false;
    TrackedLists tracked;
    std::array<std::shared_ptr<Entry>, 2> retiring;
    FILE* log = nullptr;
    bool logAttempted = false, unavailable = false, stopped = false;
    uint64_t evaluations = 0, substitutions = 0, captures = 0;
    // Last frame generation result, reported by the background log pass instead
    // of by the callback that observes it.
    unsigned lastResult = 0;
    // Per-generated-frame index split of the two counters above. Index 0 is the
    // real frame, 1..n are the generated ones; a missing substitution on any of
    // them means that frame kept the engine's original motion vectors.
    uint64_t evaluationsByIndex[8] {}, substitutionsByIndex[8] {};
    // Which of the two evaluation paths in the hook produced the counts above.
    // Path 0 carries the DLSS-G names, path 1 is the MotionVectors/Depth alias
    // that the upscaler and Ray Reconstruction use too. Only path 0 can feed the
    // frame generator, so a correction that never lands there explains a display
    // that does not change. Order: evaluations/substitutions/prepared.
    uint64_t evaluationsByPath[2] {}, substitutionsByPath[2] {}, preparedByPath[2] {};
    // A skipped substitution is only harmless when the motion texture it would
    // have written was already corrected by an earlier evaluation of the same
    // engine frame (the host evaluates one frame once per back buffer). A skip
    // on a texture that was never corrected leaves that frame with the engine's
    // own motion vectors.
    ID3D12Resource* correctedMotion[4] {};
    unsigned correctedMotionNext = 0;
    uint64_t unsubstitutedReusedMotion = 0, unsubstitutedFreshMotion = 0;
    // Evaluations that are provably not the frame generator: the upscaler and
    // Ray Reconstruction read the same MotionVectors/Depth names. They are
    // counted and passed through untouched, so "the correction never touched
    // another NGX feature" is a number in the status, not a claim. No
    // substitution counter is needed for them: reaching a substitution would
    // require passing this gate, which returns before any session exists.
    uint64_t nonFrameGenerationEvaluations = 0;
    // Second consumer (DLSS-NR) outcomes, under mutex.
    uint64_t secondConsumerServed = 0, secondConsumerRefused = 0;
    // Evaluations the DLSS-G provider hook proved are the frame generator even
    // though the driver-level parameter table named only MotionVectors/Depth.
    // The provider created that handle for NVSDK_NGX_Feature_FrameGeneration, so
    // the identity does not come from the parameter names and the upscaler or
    // Ray Reconstruction, which never register that way, stay untouched.
    uint64_t providerConfirmedEvaluations = 0, providerUnconfirmedEvaluations = 0;
    // Evaluations admitted on the calling module's identity instead of the
    // handle the provider created. Counted apart from providerConfirmed so the
    // log shows which of the two proofs the live session actually used.
    uint64_t callerConfirmedEvaluations = 0;
    // Every substituted evaluation is read back afterwards: the shared
    // parameter names have to hold the engine's own textures again, otherwise
    // the next feature that reads them (the upscaler or Ray Reconstruction)
    // would receive our composed motion. restoreFailures has to stay 0.
    uint64_t restoreChecks = 0, restoreFailures = 0;
    // Runtime extent change: the frame-generation inputs were rebuilt at a new
    // size. The draw capture is process-resident, so it is released and rebuilt
    // at a quiescent point (no active or retiring session) instead of leaving
    // the correction off for the rest of the session.
    std::uint32_t rebuildWidth = 0, rebuildHeight = 0;
    bool rebuildPending = false;
    unsigned rebuildAttempts = 0;
    Microsoft::WRL::ComPtr<ID3D12Device> rebuildDevice;

    void reap()
    {
        InternalD3D12Scope ownCalls;
        for (auto& entry : retiring)
            if (entry && !entry->evaluations)
            {
                if (!entry->session.readyToRelease())
                {
                    // Evidence for the 2026-09-15 reset: the previous build
                    // released the packed outputs here while the compose list
                    // was still executing on the FG queue.
                    if (log && !entry->releaseLogged)
                    {
                        entry->releaseLogged = true;
                        std::fprintf(log,
                                     "NATIVE_RETIRE blocked compose_inflight=%u submitted=%llu completed=%llu\n",
                                     entry->session.packedComposeInFlight() ? 1u : 0u,
                                     static_cast<unsigned long long>(entry->session.packedComposeSubmitted()),
                                     static_cast<unsigned long long>(entry->session.packedComposeCompleted()));
                        std::fflush(log);
                    }
                    continue;
                }
                if (log)
                {
                    std::fprintf(
                        log, "NATIVE_RETIRE release compose_inflight=%u submitted=%llu completed=%llu forced=%llu\n",
                        entry->session.packedComposeInFlight() ? 1u : 0u,
                        static_cast<unsigned long long>(entry->session.packedComposeSubmitted()),
                        static_cast<unsigned long long>(entry->session.packedComposeCompleted()),
                        static_cast<unsigned long long>(entry->session.packedComposeForced()));
                    std::fflush(log);
                }
                // A dump recorded into this entry's compose list has to be read
                // out before the entry and its readback targets go away. The
                // compose fence is already drained at this point, so this is a
                // non-blocking map. 2026-09-16: without this the request that
                // landed on an entry which retired before the health pass saw
                // it left the live channel waiting for a frame forever.
                entry->session.serviceDump();
                entry->session.releaseAfterGpuDrain();
                entry.reset();
            }
        // No session is left to refer to a list, so the Reset table starts over
        // with the next one.
        if (!active && !retiring[0] && !retiring[1])
            tracked.clear();
        if (rebuildPending && !active && !retiring[0] && !retiring[1] && rebuildDevice)
        {
            const auto previous = ReadPackedMotionCaptureStatus();
            const auto width = rebuildWidth, height = rebuildHeight;
            rebuildPending = false;
            const bool released = ReleasePackedMotionCapture();
            const bool ready = InitializePackedMotionCapture(rebuildDevice.Get(), width, height, log,
                                                             MakeGlassMotionIdentityProvider());
            ++rebuildAttempts;
            if (log)
            {
                std::fprintf(log,
                             "PACKED_CAPTURE extent_rebuild released=%u old=%ux%u new=%ux%u ready=%u attempt=%u\n",
                             released ? 1u : 0u, previous.width, previous.height, width, height, ready ? 1u : 0u,
                             rebuildAttempts);
                std::fflush(log);
            }
            if (ready)
            {
                rebuildAttempts = 0;
                rebuildDevice.Reset();
            }
            else if (rebuildAttempts < 3)
                rebuildPending = true; // Retry on the next health pass.
            else
                rebuildDevice.Reset();
        }
    }
    bool retire()
    {
        reap();
        if (!active)
            return true;
        activeCommand.store(nullptr, std::memory_order_release);
        active->session.stop();
        for (auto& slot : retiring)
            if (!slot)
            {
                slot = std::move(active);
                return true;
            }
        return false; // Bounded VRAM retention; no wait or unsafe early free.
    }
    template <class Function> void each(Function function)
    {
        if (active)
            function(*active);
        for (auto& entry : retiring)
            if (entry)
                function(*entry);
    }
    // Compose-fence view across every retained entry. Called with mutex held.
    std::uint64_t activeComposeSubmitted() const
    {
        std::uint64_t value = 0;
        if (active) value = std::max(value, active->session.packedComposeSubmitted());
        for (const auto& entry : retiring)
            if (entry) value = std::max(value, entry->session.packedComposeSubmitted());
        return value;
    }
    std::uint64_t activeComposeCompleted() const
    {
        std::uint64_t value = 0;
        if (active) value = std::max(value, active->session.packedComposeCompleted());
        for (const auto& entry : retiring)
            if (entry) value = std::max(value, entry->session.packedComposeCompleted());
        return value;
    }
    std::uint64_t activeComposeForced() const
    {
        std::uint64_t value = 0;
        if (active) value = std::max(value, active->session.packedComposeForced());
        for (const auto& entry : retiring)
            if (entry) value = std::max(value, entry->session.packedComposeForced());
        return value;
    }
    bool anyComposeInFlight() const
    {
        if (active && active->session.packedComposeInFlight())
            return true;
        for (const auto& entry : retiring)
            if (entry && entry->session.packedComposeInFlight())
                return true;
        return false;
    }
};
// Hooks and outstanding destruction notifications can outlive NGX shutdown.
// No static destructor releases resources from an in-flight recording.
Runtime& runtime()
{
    static auto* value = new Runtime;
    return *value;
}

// Live control requests are queued from the file poll (health thread) and
// consumed on the FG evaluation thread, which is the only place the native
// recordings may be retired while the game keeps running.
std::atomic<bool> softReloadRequested = false;
std::atomic<bool> dumpRequested = false;
// Bounded retry counter for a dump that arrived while no session was active.
std::atomic<unsigned> dumpRetries { 0 };
// Set from the loader lock by NoteProcessAttach, written into the session log
// by the first health-service tick.
std::atomic<bool> attachPending = false;
// The driver-level frame generation block carries no Streamline frame token, so
// the packed capture is keyed by this counter instead.


std::filesystem::path packedShaderPath()
{
    return Util::DllPath().parent_path() / L"Glass" / L"GlassObjectMotion.hlsl";
}

// Module log sink for the output dump. Takes the host mutex itself; the dump
// never calls it while holding its own lock.
void fgDumpLog(const char* text) noexcept
{
    try
    {
        auto& r = runtime();
        std::lock_guard lock(r.mutex);
        if (!r.logAttempted)
        {
            r.logAttempted = true;
            r.log = _wfopen((Util::DllPath().parent_path() / L"OptiScaler.Glass.log").c_str(), L"a");
        }
        if (r.log)
        {
            std::fputs(text, r.log);
            std::fflush(r.log);
        }
    }
    catch (...)
    {
    }
}

// Crash forensics: this marker exists only while the batch that carries our
// compose is in flight. After a driver reset the file is still there, which
// proves the reset happened in a batch that contained our work.
// The file is a state marker, so it is written only on the transitions: one
// create when the first compose of a run is queued and one delete when the last
// one completes. Re-creating it every frame cost 1.0ms at the median and up to
// 19.9ms at the tail on the render thread (1500-op probe, 2026-09-16), which
// breaks the 1ms frame budget on its own.
std::atomic<bool> composeMarkerActive { false };
bool writeComposeMarker(UINT64 value, bool active)
{
    const auto path = Util::DllPath().parent_path() / L"Glass" / L"glass-compose.pending";
    if (!active)
    {
        std::error_code error;
        std::filesystem::remove(path, error);
        return true;
    }
    FILE* file = _wfopen(path.c_str(), L"wb");
    if (!file)
        return false;
    std::fprintf(file, "tick=%llu producer=%llu\n", static_cast<unsigned long long>(GetTickCount64()),
                 static_cast<unsigned long long>(value));
    std::fclose(file);
    return true;
}

void raiseComposeMarker(UINT64 value)
{
    if (composeMarkerActive.load(std::memory_order_relaxed))
        return;
    // The flag is published only after the file exists, so a failed create is
    // retried on the next frame instead of silently dropping the marker.
    if (writeComposeMarker(value, true))
        composeMarkerActive.store(true, std::memory_order_release);
}

void clearComposeMarker()
{
    if (!composeMarkerActive.exchange(false, std::memory_order_acq_rel))
        return;
    writeComposeMarker(0, false);
}

// Automatic staged ramp for unattended sessions: the same order the manual
// probe/apply protocol uses, driven by elapsed time after the first FG frame.
void runAutoStage() noexcept
{
    static std::atomic<std::uint64_t> startMs = 0;
    static std::atomic<unsigned> step = ~0u;
    const auto now = GetTickCount64();
    auto started = startMs.load(std::memory_order_relaxed);
    if (!started)
    {
        startMs.compare_exchange_strong(started, now, std::memory_order_relaxed);
        started = startMs.load(std::memory_order_relaxed);
    }
    const auto seconds = (now - started) / 1000;
    // Escalation ladder for the boundary pass. The FG input swap is never
    // enabled automatically: a reset must be attributable to one compute size.
    const unsigned target = seconds < 10 ? 0u : seconds < 40 ? 1u : 2u;
    if (step.load(std::memory_order_relaxed) == target)
        return;
    step.store(target, std::memory_order_relaxed);
    auto value = ReadControls();
    value.packedDispatch = true;
    value.packedRows = target == 0 ? 1u : target == 1 ? 240u : 1440u;
    value.packedSubstitute = false;
    WriteControls(value);
    if (auto& r = runtime(); r.log)
    {
        std::fprintf(r.log, "AUTO_STAGE step=%u rows=%u substitute=%u elapsed_s=%llu\n", target, value.packedRows,
                     value.packedSubstitute ? 1u : 0u, static_cast<unsigned long long>(seconds));
        std::fflush(r.log);
    }
}

D3D12Callbacks makeCallbacks()
{
    D3D12Callbacks value;
    value.context = &runtime();
    value.enter = [](void* p) { static_cast<Runtime*>(p)->mutex.lock(); };
    value.leave = [](void* p) { static_cast<Runtime*>(p)->mutex.unlock(); };
    value.stateTracked = [](void* p, ID3D12GraphicsCommandList* c)
    { return static_cast<Runtime*>(p)->activeCommand.load(std::memory_order_acquire) == c; };
    // Reset reaches the sessions only for the lists they may refer to (see
    // TrackedLists) and, while an output dump batch can hold a copy recorded
    // into an open list, for every list. Any other Reset has no session or
    // dump state to update; the packed capture drops its own recordings of it
    // in the geometry command observer's Reset hook.
    value.resetTracked = [](void* p, ID3D12GraphicsCommandList* c)
    { return static_cast<Runtime*>(p)->tracked.contains(c) || FgOutputDumpTracksResets(); };
    value.reset = [](void* p, ID3D12GraphicsCommandList* c, bool okay, ID3D12PipelineState* initial)
    {
        auto& r = *static_cast<Runtime*>(p);
        InternalD3D12Scope ownCalls;
        NoteFgOutputDumpReset(c);
        r.each([&](Entry& e) { e.session.onReset(c, okay, initial); });
        r.reap();
    };
    value.mutation = [](void* p, ID3D12GraphicsCommandList* c)
    { static_cast<Runtime*>(p)->each([&](Entry& e) { e.session.onStateMutation(c); }); };
    value.beforeSubmit = [](void* p, ID3D12CommandQueue* q, UINT count, ID3D12CommandList* const* lists)
    {
        auto& r = *static_cast<Runtime*>(p);
        TimingScope timing(SubmissionTiming());
        InternalD3D12Scope ownCalls;
        {
            TimingScope capture(CaptureTiming());
            NotifyGeometryCaptureBeforeSubmit(q, count, lists);
        }
        TimingScope compose(ComposeTiming());
        // The one rule this hook follows after the 2026-09-16 08:38 freeze: it
        // never adds a wait to an engine queue. A captured producer queue can
        // be destroyed by a session rebuild, and a wait whose value that queue
        // only reaches later stops the GPU until the engine watchdog kills the
        // process. Measured in that session: the compose fence stayed at zero
        // for 140 seconds while the frame generation queue waited on it. The
        // compose is queued on the frame generation queue itself, in submission
        // order ahead of the batch that consumes it, which is the ordering
        // every earlier verified session used.
        r.each([&](Entry& e)
        {
            if (!q || !e.command)
                return;
            for (UINT i = 0; i < count; ++i)
            {
                auto* candidate = static_cast<ID3D12GraphicsCommandList*>(lists[i]);
                const bool known = e.session.handlesFgCommand(candidate);
                // The batch that carries the frame generation list is matched by
                // the session identity set. The most recent evaluation identity
                // is accepted as well, because the engine can submit a list
                // recorded in an earlier evaluation of the same frame.
                const bool current = candidate == r.activeCommand.load(std::memory_order_acquire);
                if (!known && !current)
                    continue;
                if (!e.session.pendingFor(candidate))
                    continue;
                {
                    static std::atomic<unsigned> unmatched { 0 };
                    if (r.log != nullptr && !known && current && unmatched.fetch_add(1, std::memory_order_relaxed) < 4)
                    {
                        std::fprintf(r.log, "TRACE_PRERACE known=0 current=1 command=%p\n",
                                     static_cast<void*>(candidate));
                        std::fflush(r.log);
                    }
                }
                {
                    ID3D12Fence* fence = nullptr;
                    std::uint64_t value = 0;
                    void* producerQueue = nullptr;
                    const bool waited = e.session.takeProducerWait(fence, value, producerQueue);
                    bool queued = false;
                    {
                        // Driver submission of the compose: attributed
                        // separately from the rest of the compose scope so a
                        // block can be told apart from a descheduled thread.
                        TimingScope queue(ComposeQueueTiming());
                        queued = e.session.executePending(q, candidate);
                    }
                    // The marker now describes the queued compose on the GPU, not
                    // the CPU window, so it is written only when one was queued.
                    if (queued)
                        raiseComposeMarker(waited ? value : 0);
                    if (r.log != nullptr)
                    {
                        // Bounded attribution: the producer queue and its fence
                        // are read for the record only. They are never submitted
                        // to and never waited on, so a stale handle cannot stall
                        // the engine's own submission.
                        static std::atomic<unsigned> attribution { 0 };
                        if (attribution.fetch_add(1, std::memory_order_relaxed) < 8)
                        {
                            std::fprintf(r.log,
                                         "TRACE_WAIT_UNUSED waited=%u producer=%llu completed=%llu producerQueue=%p "
                                         "queue=%p submitted=%u\n",
                                         waited ? 1u : 0u, static_cast<unsigned long long>(value),
                                         static_cast<unsigned long long>(fence != nullptr ? fence->GetCompletedValue() : 0),
                                         producerQueue, static_cast<void*>(q), queued ? 1u : 0u);
                            std::fflush(r.log);
                        }
                        else if (ReadControls().trace && TraceWanted())
                        {
                            std::fprintf(r.log, "TRACE_WAIT_SKIP producer=%llu queue=%p submitted=%u\n",
                                         static_cast<unsigned long long>(value), static_cast<void*>(q),
                                         queued ? 1u : 0u);
                            std::fflush(r.log);
                        }
                    }
                    break;
                }
            }
        });
    };
    value.submit = [](void* p, ID3D12CommandQueue* q, UINT count, ID3D12CommandList* const* lists)
    {
        auto& r = *static_cast<Runtime*>(p);
        InternalD3D12Scope ownSignals;
        NotifyGeometryCaptureSubmit(q, count, lists);
        NoteFgOutputDumpSubmit(q, count, lists, fgDumpLog);
        // One-time, off-thread: the packed object-motion HLSL compile measured
        // 153ms when it ran inside the frame generation callback, which is the
        // exact moment the engine rebuilds its lists (focus regain).
        WarmPackedShaderOnce();
        r.each([&](Entry& e) { e.session.afterSubmit(q, count, lists); });
        // Crash forensics: the marker describes the GPU state, not the CPU
        // submission window, so it is cleared only once the deferred compose of
        // every live entry has actually completed. A reset that frees the packed
        // outputs while the compose is still executing therefore leaves it.
        // The frame generation batch can carry any of the lists the session
        // knows (one per back buffer), so the match is by membership.
        bool matched = false;
        if (auto* fg = r.activeCommand.load(std::memory_order_acquire))
            for (UINT i = 0; i < count && !matched; ++i)
                matched = lists[i] == fg;
        if (!matched && r.active)
            for (UINT i = 0; i < count && !matched; ++i)
                matched = r.active->session.handlesFgCommand(static_cast<ID3D12GraphicsCommandList*>(lists[i]));
        if (matched)
        {
            bool inFlight = false;
            r.each([&](Entry& e) { inFlight |= e.session.packedComposeInFlight(); });
            if (!inFlight)
                clearComposeMarker();
            // Attribute a driver reset to the exact submitted batch that carried
            // our substituted inputs.
            if (r.active)
                r.active->session.dumpSubmitted(q);
            if (r.log && ReadControls().trace && TraceWanted())
            {
                std::fprintf(r.log, "TRACE_SUBMIT fg=1 lists=%u queue=%p\n", count, q);
                std::fflush(r.log);
            }
        }
        r.reap();
        // The live channel and the periodic log must not depend on the
        // OptiScaler overlay being rendered: this hook runs every frame.
        RefreshGeometryHealthIfNeeded(GetTickCount64());
    };
    value.signal = [](void*, ID3D12CommandQueue* q, ID3D12Fence* fence, UINT64 number)
    { NotifyGeometryCaptureSignal(q, fence, number); };
    value.wait = [](void*, ID3D12CommandQueue* q, ID3D12Fence* fence, UINT64 number)
    { NotifyGeometryCaptureWait(q, fence, number); };
    return value;
}

// The state observer is a Detours transaction that suspends every thread in the
// process. Installing it inside the first frame generation evaluation put the
// whole transaction on the engine render thread at the moment the game regains
// focus (observer_ms=134.9 in the 05:46 session). Every command list of the
// same device exposes the same vtable targets, and the install path proves that
// by comparing a second list and both queue types before it attaches, so the
// transaction runs once at device creation instead and the render thread only
// re-validates the identity of the list it was handed.
std::atomic<double> observerPreinstallMs { 0.0 };

bool preinstallNativeObserver(ID3D12Device* device) noexcept
{
    try
    {
        const auto start = std::chrono::steady_clock::now();
        const bool installed = PreinstallD3D12Observer(device, makeCallbacks());
        observerPreinstallMs.store(
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count(),
            std::memory_order_relaxed);
        return installed;
    }
    catch (...)
    {
        return false;
    }
}

std::shared_ptr<Entry> acquire(Runtime& r, ID3D12GraphicsCommandList* command, const NVSDK_NGX_Handle* handle,
                               const Inputs& inputs, Controls controls)
{
    TimingScope timing(EvaluationTiming());
    r.reap();
    if (r.stopped || r.unavailable || !command || !handle || !inputs.valid())
        return {};
    if (r.active)
    {
        const bool accepting = r.active->session.accepting();
        const auto observedMask = ObservedComputeMethods();
        // The engine alternates between frame generation command lists (one per
        // back buffer) for the same inputs, so a list the session does not know
        // is added instead of retiring the session. The replacement list can
        // land on the same address as a destroyed one, so the missing identity
        // is checked as well as a changed pointer.
        if (accepting && !r.active->session.handlesFgCommand(command) &&
            r.active->session.adoptFgCommand(command, observedMask, observedMask))
        {
            r.active->command = command;
            r.activeCommand.store(command, std::memory_order_release);
            r.tracked.add(command);
        }
        const bool sameInputs =
            r.active->session.handlesFgCommand(command) && r.active->matches(command, inputs);
        if (accepting && sameInputs && r.active->handle != handle)
        {
            // The feature instance pointer is not part of the correction state:
            // the packed outputs are keyed by extent and format, and every frame
            // reads its own inputs. Streamline was observed alternating between
            // two instances for the same frame generation inputs on the same
            // command list, and treating that as a different session rebuilt the
            // packed outputs and cancelled the compose on every frame.
            static std::atomic<unsigned> adopted { 0 };
            if (r.log != nullptr && adopted.fetch_add(1, std::memory_order_relaxed) < 8)
            {
                std::fprintf(r.log, "NATIVE_HANDLE adopt=1 old=%p new=%p sameMotion=%u sameDepth=%u\n",
                             static_cast<const void*>(r.active->handle), static_cast<const void*>(handle),
                             inputs.motion == r.active->motionInput ? 1u : 0u,
                             inputs.depth == r.active->depthInput ? 1u : 0u);
                std::fflush(r.log);
            }
            r.active->handle = handle;
        }
        if (!accepting || !sameInputs || r.active->handle != handle)
        {
            // Bounded attribution for the per-frame session churn: the four
            // conditions were indistinguishable in the log, so a recreated
            // session could not be traced to its cause.
            static std::atomic<unsigned> churn { 0 };
            if (r.log != nullptr && churn.fetch_add(1, std::memory_order_relaxed) < 8)
            {
                std::fprintf(r.log, "NATIVE_CHURN accepting=%u inputs=%u handle=%u active=%p incoming=%p\n",
                             accepting ? 1u : 0u, sameInputs ? 1u : 0u,
                             r.active->handle == handle ? 1u : 0u, static_cast<const void*>(r.active->handle),
                             static_cast<const void*>(handle));
                // Which field of the incoming run differs: without it the second
                // evaluation per frame cannot be identified at all.
                const auto live = [](ID3D12Resource* value) { return value ? value->GetDesc() : D3D12_RESOURCE_DESC {}; };
                const auto motion = live(inputs.motion), depth = live(inputs.depth);
                std::fprintf(r.log,
                             "NATIVE_CHURN_IN incoming motion=%llux%u f=%u depth=%llux%u f=%u "
                             "active motion=%llux%u f=%u depth=%llux%u f=%u command=%u sameMotion=%u sameDepth=%u\n",
                             static_cast<unsigned long long>(motion.Width), motion.Height,
                             static_cast<unsigned>(motion.Format), static_cast<unsigned long long>(depth.Width),
                             depth.Height, static_cast<unsigned>(depth.Format),
                             static_cast<unsigned long long>(r.active->descriptions[0].Width),
                             r.active->descriptions[0].Height, static_cast<unsigned>(r.active->descriptions[0].Format),
                             static_cast<unsigned long long>(r.active->descriptions[2].Width),
                             r.active->descriptions[2].Height, static_cast<unsigned>(r.active->descriptions[2].Format),
                             r.active->command == command ? 1u : 0u,
                             inputs.motion == r.active->motionInput ? 1u : 0u,
                             inputs.depth == r.active->depthInput ? 1u : 0u);
                std::fflush(r.log);
            }
            if (!r.retire())
                return {};
        }
    }
    if (r.active)
        return r.active;
    if (!controls.active() || inputs.index != 1)
        return {};
    for (const auto& entry : r.retiring)
        if (entry)
            return {}; // Drain old-size recordings before reuse.
    InternalD3D12Scope ownCalls;
    if (!r.logAttempted)
    {
        r.logAttempted = true;
        const auto path = Util::DllPath().parent_path() / L"OptiScaler.Glass.log";
        // The step trace is per frame, so the file grows without bound if it is
        // only ever appended to. Rotate a bounded file once and start fresh.
        std::error_code sizeError;
        if (std::filesystem::exists(path, sizeError) && !sizeError &&
            std::filesystem::file_size(path, sizeError) > kLogLimit && !sizeError)
        {
            const auto previous = Util::DllPath().parent_path() / L"OptiScaler.Glass.previous.log";
            std::error_code rotateError;
            std::filesystem::remove(previous, rotateError);
            std::filesystem::rename(path, previous, rotateError);
        }
        r.log = _wfopen(path.c_str(), L"a");
        if (!r.log)
            r.unavailable = true;
    }
    if (r.unavailable)
        return {};
    // Session creation runs on the engine's render thread when the frame
    // generation lists are rebuilt, which is the focus-regain path. The three
    // phases are timed apart so a remaining hitch can be attributed instead of
    // guessed: observer install, packed capture construction, session GPU state.
    const auto observerStart = std::chrono::steady_clock::now();
    const bool observerInstalled = InstallD3D12Observer(command, makeCallbacks());
    const auto observerMs =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - observerStart).count();
    if (!observerInstalled)
    {
        r.unavailable = true;
        std::fprintf(r.log, "NATIVE_HOST ready=0 reason=observer_coverage mismatch_slot=%d\n",
                     MismatchD3D12ObserverSlot(command));
        std::fflush(r.log);
        return {};
    }
    r.submissionObserved.store(true, std::memory_order_release);
    auto entry = std::make_shared<Entry>();
    entry->handle = handle;
    entry->command = command;
    entry->motionInput = inputs.motion;
    entry->depthInput = inputs.depth;
    entry->descriptions[0] = inputs.motion->GetDesc();
    entry->descriptions[1] = inputs.color->GetDesc();
    entry->descriptions[2] = inputs.depth->GetDesc();
    Microsoft::WRL::ComPtr<ID3D12Device> device;
    if (FAILED(command->GetDevice(IID_PPV_ARGS(&device))))
        return {};
    const auto width = static_cast<std::uint32_t>(entry->descriptions[0].Width);
    const auto height = entry->descriptions[0].Height;
    const auto captureStart = std::chrono::steady_clock::now();
    const bool captureReady =
        InitializePackedMotionCapture(device.Get(), width, height, r.log, MakeGlassMotionIdentityProvider());
    const auto captureMs =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - captureStart).count();
    if (!captureReady)
    {
        // A different extent means the frame-generation inputs were rebuilt at
        // a new size (resolution or DLSS quality change). The capture is sized
        // at runtime from those inputs, so it has to be rebuilt; defer that to
        // the health thread, which can prove no session still references it.
        const auto current = ReadPackedMotionCaptureStatus();
        if (current.initialized && (current.width != width || current.height != height))
        {
            r.rebuildWidth = width;
            r.rebuildHeight = height;
            r.rebuildPending = true;
            r.rebuildAttempts = 0;
            r.rebuildDevice = device;
            if (r.log)
            {
                std::fprintf(r.log, "PACKED_CAPTURE extent_change requested=%ux%u current=%ux%u deferred=1\n",
                             width, height, current.width, current.height);
                std::fflush(r.log);
            }
        }
        return {};
    }
    const auto shaders = Util::DllPath().parent_path() / L"Glass";
    const auto sessionStart = std::chrono::steady_clock::now();
    if (!entry->session.initializePacked(device.Get(), entry->descriptions,
                                         (shaders / L"GlassObjectMotion.hlsl").c_str(), r.log,
                                         { AcquirePackedMotionFrame, AcquirePackedMotionFrameForSecondConsumer,
                                           DiscardPackedMotionRecording }))
    {
        r.unavailable = true;
        return {};
    }
    const auto sessionMs =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - sessionStart).count();
    const auto mask = ObservedComputeMethods();
    if (!entry->session.bindFgCommand(command, mask, mask))
    {
        entry->session.releaseAfterGpuDrain(); // Initialization recorded nothing.
        r.unavailable = true;
        return {};
    }
    r.active = entry;
    r.activeCommand.store(command, std::memory_order_release);
    r.tracked.add(command);
    std::fprintf(r.log,
                 "NATIVE_HOST ready=1 handle=%p command=%p methods=%x width=%llu height=%u observer_ms=%.1f "
                 "capture_ms=%.1f session_ms=%.1f preinstall_ms=%.1f\n",
                 handle, command, mask, entry->descriptions[0].Width, entry->descriptions[0].Height, observerMs,
                 captureMs, sessionMs, observerPreinstallMs.load(std::memory_order_relaxed));
    std::fflush(r.log);
    return entry;
}
} // namespace

bool PreinstallNativeObserver(ID3D12Device* device) noexcept
{
    return preinstallNativeObserver(device);
}

bool NativeCaptureSubmissionReady() noexcept
{
    return runtime().submissionObserved.load(std::memory_order_acquire);
}

void RequestNativeSoftReload() noexcept
{
    softReloadRequested.store(true, std::memory_order_release);
}

void RequestPackedDump() noexcept
{
    dumpRequested.store(true, std::memory_order_release);
}

bool RequestNativeFgOutputDump(unsigned count) noexcept
{
    return RequestFgOutputDump(count, fgDumpLog);
}

// Second consumer (DLSS 5 neural rendering). Called from the neural rendering
// evaluate before the model runs, with the guides that evaluate would otherwise
// read. On success the caller substitutes the returned pair for this evaluate
// only; the engine's own textures keep the values the game wrote.
bool SecondConsumerGuides(ID3D12GraphicsCommandList* command, ID3D12Resource* motion, ID3D12Resource* depth,
                          D3D12_RESOURCE_STATES motionArrival, D3D12_RESOURCE_STATES depthArrival, float jitterX,
                          float jitterY, float scaleX, float scaleY, ID3D12Resource** outMotion,
                          ID3D12Resource** outDepth) noexcept
{
    try
    {
        if (outMotion != nullptr)
            *outMotion = nullptr;
        if (outDepth != nullptr)
            *outDepth = nullptr;
        if (command == nullptr || motion == nullptr || depth == nullptr)
            return false;
        auto& r = runtime();
        std::lock_guard lock(r.mutex);
        if (!r.active)
        {
            ++r.secondConsumerRefused;
            return false;
        }
        // A served list holds session work until its Reset or destruction, and
        // the session fences every submission of it, so its Reset has to reach
        // the session from now on (see TrackedLists).
        const bool served = r.active->session.secondConsumerGuides(command, motion, depth, motionArrival,
                                                                   depthArrival, jitterX, jitterY, scaleX, scaleY,
                                                                   outMotion, outDepth);
        if (served)
        {
            r.tracked.add(command);
            ++r.secondConsumerServed;
        }
        else
            ++r.secondConsumerRefused;
        return served;
    }
    catch (...)
    {
        return false;
    }
}

void NoteStreamlineFeature(unsigned id) noexcept
{
    try
    {
        static std::atomic<unsigned> seen[32] {};
        static std::atomic<unsigned> logged { 0 };
        const auto slot = id % 32u;
        const auto bit = 1u << (id % 31u);
        if ((seen[slot].fetch_or(bit, std::memory_order_relaxed) & bit) != 0)
            return;
        if (logged.fetch_add(1, std::memory_order_relaxed) >= 8)
            return;
        auto& r = runtime();
        std::lock_guard lock(r.mutex);
        if (!r.logAttempted)
        {
            r.log = _wfopen((Util::DllPath().parent_path() / L"OptiScaler.Glass.log").c_str(), L"a");
            // Only a successful open may close the door: a failed one has to
            // stay retryable, otherwise the whole session stops logging.
            r.logAttempted = r.log != nullptr;
        }
        if (r.log)
        {
            std::fprintf(r.log, "SL_FEATURE id=%u\n", id);
            std::fflush(r.log);
        }
    }
    catch (...)
    {
    }
}

void NoteNgxFeature(unsigned feature, unsigned handleId, const char* provider) noexcept
{
    try
    {
        struct Pair
        {
            unsigned feature = 0;
            unsigned handle = 0;
        };
        static Pair seen[16] {};
        static std::atomic<unsigned> seenCount { 0 };
        static std::atomic<unsigned> logged { 0 };
        const auto count = (std::min)(seenCount.load(std::memory_order_relaxed), 16u);
        for (unsigned i = 0; i < count; ++i)
            if (seen[i].feature == feature && seen[i].handle == handleId)
                return;
        if (logged.fetch_add(1, std::memory_order_relaxed) >= 12)
            return;
        if (count < 16)
        {
            seen[count] = { feature, handleId };
            seenCount.store(count + 1, std::memory_order_relaxed);
        }
        auto& r = runtime();
        std::lock_guard lock(r.mutex);
        if (!r.logAttempted)
        {
            r.logAttempted = true;
            r.log = _wfopen((Util::DllPath().parent_path() / L"OptiScaler.Glass.log").c_str(), L"a");
        }
        if (r.log)
        {
            std::fprintf(r.log, "NGX_FEATURE feature=%u handleId=%u fgConstant=%u provider=%s\n", feature, handleId,
                         static_cast<unsigned>(NVSDK_NGX_Feature_FrameGeneration), provider ? provider : "-");
            std::fflush(r.log);
        }
    }
    catch (...)
    {
    }
}

void NoteProviderFrameGenerationIdentity(bool confirmed, unsigned handleId, const char* motionKey) noexcept
{
    try
    {
        static std::atomic<unsigned> logged { 0 };
        if (logged.fetch_add(1, std::memory_order_relaxed) >= 4)
            return;
        auto& r = runtime();
        std::lock_guard lock(r.mutex);
        if (!r.logAttempted)
        {
            r.logAttempted = true;
            r.log = _wfopen((Util::DllPath().parent_path() / L"OptiScaler.Glass.log").c_str(), L"a");
        }
        if (r.log)
        {
            std::fprintf(r.log, "NATIVE_FG_PROVIDER confirmed=%u handle=%u motionKey=%s\n", confirmed ? 1u : 0u,
                         handleId, motionKey != nullptr ? motionKey : "-");
            std::fflush(r.log);
        }
    }
    catch (...)
    {
    }
}

void NoteNgxCreate(unsigned feature, unsigned handleId, const char* route) noexcept
{
    try
    {
        struct Pair
        {
            unsigned feature = 0;
            unsigned handle = 0;
        };
        static Pair seen[16] {};
        static std::atomic<unsigned> seenCount { 0 };
        static std::atomic<unsigned> logged { 0 };
        const auto count = (std::min)(seenCount.load(std::memory_order_relaxed), 16u);
        for (unsigned i = 0; i < count; ++i)
            if (seen[i].feature == feature && seen[i].handle == handleId)
                return;
        if (logged.fetch_add(1, std::memory_order_relaxed) >= 16)
            return;
        if (count < 16)
        {
            seen[count] = { feature, handleId };
            seenCount.store(count + 1, std::memory_order_relaxed);
        }
        auto& r = runtime();
        std::lock_guard lock(r.mutex);
        if (!r.logAttempted)
        {
            r.logAttempted = true;
            r.log = _wfopen((Util::DllPath().parent_path() / L"OptiScaler.Glass.log").c_str(), L"a");
        }
        if (r.log)
        {
            std::fprintf(r.log, "NGX_CREATE feature=%u handleId=%u route=%s\n", feature, handleId, route ? route : "-");
            std::fflush(r.log);
        }
    }
    catch (...)
    {
    }
}

void NoteNvngxLoad(const wchar_t* name, bool redirect) noexcept
{
    try
    {
        auto& r = runtime();
        std::lock_guard lock(r.mutex);
        if (!r.logAttempted)
        {
            r.logAttempted = true;
            r.log = _wfopen((Util::DllPath().parent_path() / L"OptiScaler.Glass.log").c_str(), L"a");
        }
        if (r.log)
        {
            std::fprintf(r.log, "GLASS_NVNGX_LOAD name=%ls redirect=%u\n", name ? name : L"-", redirect ? 1u : 0u);
            std::fflush(r.log);
        }
    }
    catch (...)
    {
    }
}

void NoteHookStage(const wchar_t* name, unsigned stage) noexcept
{
    try
    {
        static std::atomic<unsigned> logged { 0 };
        if (logged.fetch_add(1, std::memory_order_relaxed) >= 8)
            return;
        auto& r = runtime();
        std::lock_guard lock(r.mutex);
        if (!r.logAttempted)
        {
            r.log = _wfopen((Util::DllPath().parent_path() / L"OptiScaler.Glass.log").c_str(), L"a");
            r.logAttempted = r.log != nullptr;
        }
        if (r.log)
        {
            std::fprintf(r.log, "GLASS_NGX_HOOK module=%ls stage=%u\n", name ? name : L"-", stage);
            std::fflush(r.log);
        }
    }
    catch (...)
    {
    }
}

void NoteFrameGenerationHandle(bool creation, const void* handle, unsigned handleId, bool confirmed) noexcept
{
    try
    {
        static std::atomic<unsigned> created { 0 }, createdLogged { 0 }, evaluatedLogged { 0 };
        if (creation)
        {
            if (createdLogged.fetch_add(1, std::memory_order_relaxed) >= 12)
                return;
            created.fetch_add(1, std::memory_order_relaxed);
        }
        else
        {
            // An evaluation is only informative once a creation has been seen;
            // spending the budget on the startup evaluations would hide the
            // ones that follow it.
            if (created.load(std::memory_order_relaxed) == 0)
                return;
            if (evaluatedLogged.fetch_add(1, std::memory_order_relaxed) >= 12)
                return;
        }
        auto& r = runtime();
        std::lock_guard lock(r.mutex);
        if (!r.logAttempted)
        {
            r.logAttempted = true;
            r.log = _wfopen((Util::DllPath().parent_path() / L"OptiScaler.Glass.log").c_str(), L"a");
        }
        if (r.log)
        {
            std::fprintf(r.log, "GLASS_FG_HANDLE event=%s handle=%p id=%u confirmed=%u\n",
                         creation ? "create" : "evaluate", handle, handleId, confirmed ? 1u : 0u);
            std::fflush(r.log);
        }
    }
    catch (...)
    {
    }
}

void NoteFrameGenerationCaller(const void* address, const void* handle, unsigned handleId,
                               bool classified) noexcept
{
    try
    {
        // Only the opening evaluations are informative: they show which module
        // issues the driver-level calls, and after that the classification is
        // cached and every line would repeat the same path. The counter check
        // runs before any module lookup, so the steady state costs one atomic.
        static std::atomic<unsigned> logged { 0 };
        if (logged.load(std::memory_order_relaxed) >= 24)
            return;
        MEMORY_BASIC_INFORMATION info {};
        const void* base = nullptr;
        if (address != nullptr && VirtualQuery(address, &info, sizeof(info)) == sizeof(info))
            base = info.AllocationBase;
        // A repeated caller adds nothing: every evaluation of the session comes
        // from the same one or two modules, so only a change of the caller
        // allocation is worth a line.
        static std::atomic<const void*> lastBase { nullptr };
        if (base != nullptr && lastBase.load(std::memory_order_relaxed) == base)
            return;
        if (logged.fetch_add(1, std::memory_order_relaxed) >= 24)
            return;
        lastBase.store(base, std::memory_order_relaxed);
        wchar_t path[MAX_PATH] {};
        HMODULE module = nullptr;
        if (address != nullptr &&
            GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                               static_cast<LPCWSTR>(address), &module) &&
            module != nullptr)
            GetModuleFileNameW(module, path, MAX_PATH);
        if (path[0] == L'\0' && address != nullptr)
        {
            // The over-the-air Streamline plugins are mapped as images without a
            // loader entry, so their name has to come from the mapping.
            if (base != nullptr)
            {
                static const auto getMappedFileName = []() {
                    const auto kernel = GetModuleHandleW(L"kernel32.dll");
                    return kernel != nullptr
                               ? reinterpret_cast<DWORD(WINAPI*)(HANDLE, LPVOID, LPWSTR, DWORD)>(
                                     GetProcAddress(kernel, "GetMappedFileNameW"))
                               : nullptr;
                }();
                if (getMappedFileName != nullptr)
                    getMappedFileName(GetCurrentProcess(), const_cast<void*>(base), path, MAX_PATH);
            }
        }
        auto& r = runtime();
        std::lock_guard lock(r.mutex);
        if (!r.logAttempted)
        {
            r.logAttempted = true;
            r.log = _wfopen((Util::DllPath().parent_path() / L"OptiScaler.Glass.log").c_str(), L"a");
        }
        if (r.log)
        {
            std::fprintf(r.log, "GLASS_FG_CALLER handle=%p id=%u classified=%u caller=%ls\n", handle, handleId,
                         classified ? 1u : 0u, path[0] != L'\0' ? path : L"-");
            std::fflush(r.log);
        }
    }
    catch (...)
    {
    }
}

LONG WINAPI glassUnhandledFilter(EXCEPTION_POINTERS* info) noexcept
{
    try
    {
        auto& r = runtime();
        std::lock_guard lock(r.mutex);
        if (!r.logAttempted)
        {
            r.logAttempted = true;
            r.log = _wfopen((Util::DllPath().parent_path() / L"OptiScaler.Glass.log").c_str(), L"a");
        }
        if (r.log)
        {
            const auto* record = info != nullptr ? info->ExceptionRecord : nullptr;
            std::fprintf(r.log, "GLASS_EXCEPTION code=0x%08lX address=%p\n",
                         record != nullptr ? static_cast<unsigned long>(record->ExceptionCode) : 0ul,
                         record != nullptr ? static_cast<void*>(record->ExceptionAddress) : nullptr);
            std::fflush(r.log);
        }
    }
    catch (...)
    {
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

LONG CALLBACK glassVectoredHandler(EXCEPTION_POINTERS* info) noexcept
{
    try
    {
        const auto* record = info != nullptr ? info->ExceptionRecord : nullptr;
        if (record == nullptr)
            return EXCEPTION_CONTINUE_SEARCH;
        const auto code = static_cast<unsigned long>(record->ExceptionCode);
        // Only fault-class exceptions: first-chance C++/DRM noise is not useful
        // and must not fill the log.
        if (code != 0xC0000005ul && code != 0xC000001Dul && code != 0xC0000094ul && code != 0xC0000096ul &&
            code != 0xC00000FDul && code != 0xC0000409ul && code != 0x80000003ul)
            return EXCEPTION_CONTINUE_SEARCH;
        static std::atomic<unsigned> faults { 0 };
        if (faults.fetch_add(1, std::memory_order_relaxed) >= 8)
            return EXCEPTION_CONTINUE_SEARCH;
        wchar_t moduleName[MAX_PATH] {};
        MEMORY_BASIC_INFORMATION memory {};
        const auto* address = static_cast<const unsigned char*>(record->ExceptionAddress);
        const auto* base = static_cast<const unsigned char*>(nullptr);
        if (record->ExceptionAddress != nullptr &&
            VirtualQuery(record->ExceptionAddress, &memory, sizeof(memory)) == sizeof(memory) &&
            memory.AllocationBase != nullptr)
        {
            GetModuleFileNameW(static_cast<HMODULE>(memory.AllocationBase), moduleName, MAX_PATH);
            base = static_cast<const unsigned char*>(memory.AllocationBase);
        }
        auto& r = runtime();
        std::lock_guard lock(r.mutex);
        if (!r.logAttempted)
        {
            r.log = _wfopen((Util::DllPath().parent_path() / L"OptiScaler.Glass.log").c_str(), L"a");
            r.logAttempted = r.log != nullptr;
        }
        if (r.log)
        {
            std::fprintf(r.log, "GLASS_FAULT code=0x%08lX address=%p base=%p offset=0x%llX module=%ls\n", code,
                         static_cast<const void*>(address), static_cast<const void*>(base),
                         static_cast<unsigned long long>(address != nullptr && base != nullptr ? address - base : 0),
                         moduleName[0] != 0 ? moduleName : L"-");
            std::fflush(r.log);
        }
    }
    catch (...)
    {
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

void InstallProcessDiagnostics() noexcept
{
    // The engine installs its own unhandled filter later, which replaced this
    // one; a vectored handler is not replaced and still sees the fatal fault.
    AddVectoredExceptionHandler(1, &glassVectoredHandler);
    SetUnhandledExceptionFilter(&glassUnhandledFilter);
}

void NoteProcessAttach() noexcept
{
    // No file work from the loader lock: the module path resolved there lands
    // outside the overlay the rest of the session logs into, and RootBuilder
    // deletes that copy when the game exits. The marker is written by the first
    // health-service tick instead, into the same log as every other line.
    attachPending.store(true, std::memory_order_release);
}

void NoteProcessDetach() noexcept
{
    try
    {
        auto& r = runtime();
        // Never open a handle from the loader lock: only report through a log
        // that a running session already opened.
        if (!r.log)
            return;
        std::lock_guard lock(r.mutex);
        if (r.log)
        {
            std::fprintf(r.log, "GLASS_PROCESS detach=1\n");
            std::fflush(r.log);
        }
    }
    catch (...)
    {
    }
}


void ServiceNativeDiagnostics() noexcept
{
    // Outside the runtime lock on purpose: installing a detour suspends the
    // process briefly, and a suspended thread must not be holding a lock this
    // call would need.
    InstallLoadedNgxHooks();
    auto& r = runtime();
    {
        std::lock_guard lock(r.mutex);
        if (attachPending.exchange(false, std::memory_order_acq_rel))
        {
            if (!r.logAttempted)
            {
                r.log = _wfopen((Util::DllPath().parent_path() / L"OptiScaler.Glass.log").c_str(), L"a");
                r.logAttempted = r.log != nullptr;
            }
            if (r.log)
            {
                std::fprintf(r.log, "GLASS_PROCESS attach=1 pid=%lu\n", GetCurrentProcessId());
                std::fflush(r.log);
            }
        }
        r.each([&](Entry& e) { e.session.serviceDump(); });
    }
    // Outside the host lock: readback allocation and the file write of the
    // output dump must not hold up the frame generation evaluation.
    ServiceFgOutputDump(fgDumpLog);
}

NativeHostStatus ReadNativeHostStatus() noexcept
{
    auto& r = runtime();
    std::lock_guard lock(r.mutex);
    NativeHostStatus status;
    status.evaluations = r.evaluations;
    status.substitutions = r.substitutions;
    status.captures = r.captures;
    for (unsigned i = 0; i < 8; ++i)
    {
        status.evaluationsByIndex[i] = r.evaluationsByIndex[i];
        status.substitutionsByIndex[i] = r.substitutionsByIndex[i];
    }
    status.unsubstitutedReusedMotion = r.unsubstitutedReusedMotion;
    status.unsubstitutedFreshMotion = r.unsubstitutedFreshMotion;
    status.nonFrameGenerationEvaluations = r.nonFrameGenerationEvaluations;
    status.secondConsumerServed = r.secondConsumerServed;
    status.secondConsumerRefused = r.secondConsumerRefused;
    status.providerConfirmedEvaluations = r.providerConfirmedEvaluations;
    status.providerUnconfirmedEvaluations = r.providerUnconfirmedEvaluations;
    status.callerConfirmedEvaluations = r.callerConfirmedEvaluations;
    status.restoreChecks = r.restoreChecks;
    status.restoreFailures = r.restoreFailures;
    for (unsigned i = 0; i < 2; ++i)
    {
        status.evaluationsByPath[i] = r.evaluationsByPath[i];
        status.substitutionsByPath[i] = r.substitutionsByPath[i];
        status.preparedByPath[i] = r.preparedByPath[i];
    }
    status.active = r.active ? 1u : 0u;
    status.retiring = (r.retiring[0] ? 1u : 0u) + (r.retiring[1] ? 1u : 0u);
    status.stopped = r.stopped ? 1u : 0u;
    status.unavailable = r.unavailable ? 1u : 0u;
    return status;
}

void ReportNativeHostLog() noexcept
{
    try
    {
        // Snapshot under the runtime lock, format outside it: the render thread
        // must never wait for the log. The old call site held the lock across the
        // whole report, which is why a busy file system stalled the render thread.
        const auto status = ReadNativeHostStatus();
        auto& r = runtime();
        unsigned lastResult = 0;
        FILE* log = nullptr;
        {
            std::lock_guard lock(r.mutex);
            lastResult = r.lastResult;
            log = r.log;
        }
        if (log == nullptr)
            return;
        std::fprintf(log, "NATIVE_HOST evaluations=%llu substitutions=%llu captures=%llu result=%x\n",
                     static_cast<unsigned long long>(status.evaluations),
                     static_cast<unsigned long long>(status.substitutions),
                     static_cast<unsigned long long>(status.captures), lastResult);
        std::fprintf(log, "NATIVE_HOST_BY_INDEX");
        for (unsigned i = 0; i < 8; ++i)
            if (status.evaluationsByIndex[i] != 0)
                std::fprintf(log, " %u:%llu/%llu", i,
                             static_cast<unsigned long long>(status.substitutionsByIndex[i]),
                             static_cast<unsigned long long>(status.evaluationsByIndex[i]));
        std::fprintf(log, " (substituted/evaluated) skipped_reused=%llu skipped_uncorrected=%llu\n",
                     static_cast<unsigned long long>(status.unsubstitutedReusedMotion),
                     static_cast<unsigned long long>(status.unsubstitutedFreshMotion));
        std::fprintf(log, "NATIVE_HOST_BY_PATH dlssg=%llu/%llu alias=%llu/%llu prepared=%llu/%llu\n",
                     static_cast<unsigned long long>(status.substitutionsByPath[0]),
                     static_cast<unsigned long long>(status.evaluationsByPath[0]),
                     static_cast<unsigned long long>(status.substitutionsByPath[1]),
                     static_cast<unsigned long long>(status.evaluationsByPath[1]),
                     static_cast<unsigned long long>(status.preparedByPath[0]),
                     static_cast<unsigned long long>(status.preparedByPath[1]));
        // DLSS FG only: the non-generator evaluations that were passed through
        // without a session or a swap, and the read-back that proves the shared
        // parameter names were returned to the engine's own textures.
        std::fprintf(log,
                     "NATIVE_HOST_FG_ONLY nonfg_passed=%llu provider_confirmed=%llu provider_unconfirmed=%llu "
                     "caller_confirmed=%llu restore_checked=%llu restore_failed=%llu\n",
                     static_cast<unsigned long long>(status.nonFrameGenerationEvaluations),
                     static_cast<unsigned long long>(status.providerConfirmedEvaluations),
                     static_cast<unsigned long long>(status.providerUnconfirmedEvaluations),
                     static_cast<unsigned long long>(status.callerConfirmedEvaluations),
                     static_cast<unsigned long long>(status.restoreChecks),
                     static_cast<unsigned long long>(status.restoreFailures));
        // Second consumer: DLSS-NR evaluates that read the composed pair.
        std::fprintf(log, "NATIVE_HOST_SECOND served=%llu refused=%llu\n",
                     static_cast<unsigned long long>(status.secondConsumerServed),
                     static_cast<unsigned long long>(status.secondConsumerRefused));
        ReportGeometryHost(log);
        std::fflush(log);
    }
    catch (...)
    {
    }
}

void WarmPackedShaderOnce() noexcept
{
    try
    {
        static std::once_flag once;
        std::call_once(once, []
        {
            std::thread([] {
                try
                {
                    auto& r = runtime();
                    FILE* log = nullptr;
                    {
                        std::lock_guard lock(r.mutex);
                        log = r.log;
                    }
                    const bool warmed = PackedMotionGpu::warmShaderCode(packedShaderPath().c_str(), log);
                    if (warmed && log != nullptr)
                    {
                        std::fprintf(log, "OBJECT_SHADER warmed=1\n");
                        std::fflush(log);
                    }
                }
                catch (...)
                {
                }
            }).detach();
        });
    }
    catch (...)
    {
    }
}

NVSDK_NGX_Result EvaluateNativeFG(ID3D12GraphicsCommandList* command, const NVSDK_NGX_Handle* handle,
                                  NVSDK_NGX_Parameter* parameters, PFN_NVSDK_NGX_ProgressCallback callback,
                                  NativeEvaluate original, bool providerFrameGeneration, bool dlssgProviderModule,
                                  bool frameGenerationCaller)
{
    if (!original)
        return NVSDK_NGX_Result_FAIL_FeatureNotFound;
    auto& r = runtime();
    // Hard log cap, applied from this thread so the handle is never closed while
    // a hook on another thread writes to it.
    if (r.log != nullptr && r.evaluations != 0 && (r.evaluations % 900) == 0)
    {
        const auto path = Util::DllPath().parent_path() / L"OptiScaler.Glass.log";
        std::error_code sizeError;
        const auto size = std::filesystem::file_size(path, sizeError);
        if (!sizeError && size > kLogLimit)
        {
            std::fflush(r.log);
            std::fclose(r.log);
            const auto previous = Util::DllPath().parent_path() / L"OptiScaler.Glass.previous.log";
            std::error_code rotateError;
            std::filesystem::remove(previous, rotateError);
            std::filesystem::rename(path, previous, rotateError);
            r.log = _wfopen(path.c_str(), L"a");
            if (r.log != nullptr)
            {
                std::fprintf(r.log, "GLASS_LOG rotated=1 size_bytes=%llu\n",
                             static_cast<unsigned long long>(size));
                std::fflush(r.log);
            }
        }
    }
    // File-driven live controls. Both run on this thread because it owns the
    // native recordings; the request file itself is polled elsewhere.
    if (softReloadRequested.exchange(false, std::memory_order_acq_rel))
    {
        std::lock_guard lock(r.mutex);
        if (r.retire())
        {
            r.reap();
            ResetPackedMotionCounters();
            if (r.log)
            {
                std::fprintf(r.log, "NATIVE_HOST soft_reload=1 retired=1\n");
                std::fflush(r.log);
            }
        }
        else
            softReloadRequested.store(true, std::memory_order_release); // Retry after a drain.
    }
    if (dumpRequested.exchange(false, std::memory_order_acq_rel))
    {
        std::lock_guard lock(r.mutex);
        // The session is created and retired inside each evaluation, so the
        // health thread rarely sees an active entry. A retiring entry still owns
        // its packed outputs until the compose fence completes, and the dump
        // service below runs for every entry, so target either.
        Entry* target = r.active.get();
        if (!target)
            for (auto& entry : r.retiring)
                if (entry)
                    target = entry.get();
        if (target)
        {
            dumpRetries.store(0, std::memory_order_relaxed);
            target->session.requestDump();
        }
        else
        {
            // The session is created later in this same evaluation call, so a
            // request that lands between frames has to survive until one is
            // active instead of being dropped.
            const auto attempt = dumpRetries.fetch_add(1, std::memory_order_relaxed);
            if (attempt < 240)
                dumpRequested.store(true, std::memory_order_release);
            if (r.log && (attempt < 3 || attempt == 240u))
            {
                std::fprintf(r.log, "PACKED_DUMP %s\n", attempt < 240 ? "requeued" : "skipped reason=no_active_entry");
                std::fflush(r.log);
            }
        }
    }
    if (TakeShaderReloadRequest())
    {
        std::lock_guard lock(r.mutex);
        const auto shader = packedShaderPath();
        bool reloaded = false;
        if (r.active && r.log)
            reloaded = r.active->session.reloadPackedShader(shader.c_str(), r.log);
        if (r.log)
        {
            std::fprintf(r.log, "NATIVE_HOST shader_reload=%u path=%ls\n", reloaded ? 1u : 0u, shader.c_str());
            std::fflush(r.log);
        }
    }
    static std::atomic<std::uint64_t> tagHits = 0, tagMisses = 0;
    Inputs inputs;
    std::shared_ptr<Entry> entry;
    PreparedInputs prepared;
    // 0 = DLSS-G parameter names, 1 = MotionVectors/Depth alias. Declared outside
    // the read block because the trace, the counters and the report all use it
    // after the block closes.
    unsigned pathIndex = 0;
    // Set once the identity gate admitted this evaluation as the frame
    // generator; the output dump records nothing for any other feature.
    bool frameGenerationEvaluation = false;
    auto controls = ReadControls();
    if (controls.autoStage)
    {
        runAutoStage();
        controls = ReadControls();
    }
    if (Inputs::read(parameters, inputs))
    {
        // Frame depth convention for the compose's opaque-occlusion test.
        SetFrameDepthInverted(inputs.depthInverted);
        // Path 0: the evaluation carries the DLSS-G names (DLSSG.MVecs/Depth and
        // the frame indices), so it is the frame generator. Path 1: only
        // MotionVectors/Depth are named, which is also how the upscaler and Ray
        // Reconstruction read their inputs. Both reach this hook because it
        // wraps the NGX provider export, so they have to be counted apart: the
        // parameter trace cannot tell which feature read a shared key.
        const bool dlssgPath = inputs.motionKey == nullptr || std::strcmp(inputs.motionKey, "DLSSG.MVecs") == 0;
        pathIndex = dlssgPath ? 0u : 1u;
        if (!dlssgPath)
        {
            // The driver-level block (Streamline's frame generation plugin
            // through _nvngx.dll) names only the two textures. The motion-vector
            // scale lives in the Streamline constants this module already
            // tracks, so take it from there instead of assuming 1.
            const auto scale = StreamlineHooks::GlassMvecScale();
            if (std::isfinite(scale.x) && std::isfinite(scale.y) && scale.x > 0.f && scale.y > 0.f)
            {
                inputs.scaleX = scale.x;
                inputs.scaleY = scale.y;
            }
        }
        // Frame generation identity gate (C9). Only a proof about this handle
        // admits the evaluation:
        // - providerFrameGeneration: OptiScaler's own NGX seam checked the
        //   feature id of the handle the game created, or the provider's create
        //   hook registered this handle pointer for
        //   NVSDK_NGX_Feature_FrameGeneration (ProviderConfirmsFrameGeneration);
        // - callerIdentity: the calling module is the frame generation plugin,
        //   which never evaluates the upscaler or Ray Reconstruction.
        // The parameter table is not an identity. Streamline shares one block
        // between the upscaler, Ray Reconstruction and frame generation, so once
        // the generator has run the DLSSG.* keys are present in the other two
        // features' evaluations as well, and the native feature handles all
        // carry id 1. Admitting by table keys and by learned ids let the native
        // upscaler and Ray Reconstruction evaluations through in the 2026-09-23
        // session (pid 50008: about 80,775 of 113,619 admitted evaluations),
        // which then prepared a session and read the composed motion and depth.
        const bool callerIdentity = frameGenerationCaller;
        const bool providerIdentity = providerFrameGeneration || callerIdentity;
        // Diagnostic only: whether the table names DLSS-G keys.
        static std::atomic<unsigned> tableIdentityLogged { 0 };
        if (dlssgProviderModule && tableIdentityLogged.fetch_add(1, std::memory_order_relaxed) < 6)
        {
            std::lock_guard lock(r.mutex);
            if (!r.logAttempted)
            {
                r.logAttempted = true;
                r.log = _wfopen((Util::DllPath().parent_path() / L"OptiScaler.Glass.log").c_str(), L"a");
            }
            if (r.log)
            {
                std::fprintf(r.log, "GLASS_FG_TABLE handle=%u mask=%x table=%u key=%s proven=%u\n",
                             handle != nullptr ? handle->Id : 0u, inputs.identityMask ? inputs.identityMask : 0u,
                             inputs.frameGeneration ? 1u : 0u, inputs.motionKey != nullptr ? inputs.motionKey : "-",
                             providerIdentity ? 1u : 0u);
                std::fflush(r.log);
            }
        }
        if (!providerIdentity)
        {
            {
                std::lock_guard lock(r.mutex);
                ++r.evaluationsByPath[pathIndex];
                ++r.nonFrameGenerationEvaluations;
                if (dlssgProviderModule)
                    ++r.providerUnconfirmedEvaluations;
                static std::atomic<unsigned> gateLogged { 0 };
                if (gateLogged.fetch_add(1, std::memory_order_relaxed) < 8)
                {
                    if (!r.logAttempted)
                    {
                        r.log = _wfopen((Util::DllPath().parent_path() / L"OptiScaler.Glass.log").c_str(), L"a");
                        r.logAttempted = r.log != nullptr;
                    }
                    if (r.log)
                    {
                        std::fprintf(r.log,
                                     "NATIVE_FG_GATE skip handle=%u ptr=%p motionKey=%s provider=%u caller=%u "
                                     "table=%u mask=%x evaluations=%llu\n",
                                     handle != nullptr ? handle->Id : 0u, static_cast<const void*>(handle),
                                     inputs.motionKey != nullptr ? inputs.motionKey : "-",
                                     providerFrameGeneration ? 1u : 0u, frameGenerationCaller ? 1u : 0u,
                                     inputs.frameGeneration ? 1u : 0u,
                                     inputs.identityMask ? inputs.identityMask : 0u,
                                     static_cast<unsigned long long>(r.nonFrameGenerationEvaluations));
                        std::fflush(r.log);
                    }
                }
                // Diagnostic: the texture the upscaler or Ray Reconstruction
                // writes and the colour it reads, so the GATE_DETAIL rt0/dsv ptr
                // of a refused draw can be matched offline against the
                // super-resolution output. Read-only Gets on the table handed
                // through below, which stays untouched. Output is read on every
                // pass-through to see it change; Color and the descriptions only
                // for a printed line: 24 lines, then one each time Output changes.
                static std::atomic<unsigned> srLines { 0 };
                static std::uint64_t srLastOutput = 0;
                ID3D12Resource* srOutput = nullptr;
                if (r.log != nullptr &&
                    parameters->Get(NVSDK_NGX_Parameter_Output, &srOutput) == NVSDK_NGX_Result_Success &&
                    srOutput != nullptr)
                {
                    const std::uint64_t output = reinterpret_cast<std::uintptr_t>(srOutput);
                    ID3D12Resource* srColor = nullptr;
                    if ((output != srLastOutput || srLines.load(std::memory_order_relaxed) < 24) &&
                        parameters->Get(NVSDK_NGX_Parameter_Color, &srColor) == NVSDK_NGX_Result_Success &&
                        srColor != nullptr)
                    {
                        srLines.fetch_add(1, std::memory_order_relaxed);
                        srLastOutput = output;
                        const auto outputDesc = srOutput->GetDesc();
                        const auto colorDesc = srColor->GetDesc();
                        std::fprintf(r.log, "NATIVE_SR_IO output=%llx %ux%u fmt%u color=%llx %ux%u fmt%u\n",
                                     static_cast<unsigned long long>(output), static_cast<unsigned>(outputDesc.Width),
                                     outputDesc.Height, static_cast<unsigned>(outputDesc.Format),
                                     static_cast<unsigned long long>(reinterpret_cast<std::uintptr_t>(srColor)),
                                     static_cast<unsigned>(colorDesc.Width), colorDesc.Height,
                                     static_cast<unsigned>(colorDesc.Format));
                        std::fflush(r.log);
                    }
                }
            }
            return original(command, handle, parameters, callback);
        }
        // A handle proven by the provider (OptiScaler's feature-checked seam or
        // the create hook) is remembered by pointer, so an evaluation of the
        // same handle that reaches the provider hook without the seam's check is
        // recognised there; both release paths forget it. A caller proof is not
        // remembered: it is re-checked on every call, and the live `fgcaller=any`
        // experiment would otherwise leave upscaler and Ray Reconstruction
        // pointers proven after the mode is switched back.
        if (providerFrameGeneration)
            RememberFrameGenerationHandle(static_cast<const void*>(handle), handle != nullptr ? handle->Id : 0u);
        frameGenerationEvaluation = true;
        {
            std::lock_guard lock(r.mutex);
            ++r.providerConfirmedEvaluations;
            if (callerIdentity)
                ++r.callerConfirmedEvaluations;
            NoteProviderFrameGenerationIdentity(true, handle != nullptr ? handle->Id : 0u, inputs.motionKey);
            // Diagnostic: the textures the generator is handed, so the
            // GATE_DETAIL rt0/dsv ptr of a refused draw can be matched offline
            // against the HUDless colour. Only after the genuine DLSSG.HUDLess
            // read: the driver-level block names no HUDless colour
            // (Inputs::read sets color = motion there). 24 lines, then one each
            // time the HUDless texture changes. frame is the engine render frame
            // the capture numbers its draws with (GATE_DETAIL frame=).
            static std::atomic<unsigned> fgInputLines { 0 };
            static std::uint64_t fgLastHudless = 0;
            const std::uint64_t hudless = reinterpret_cast<std::uintptr_t>(inputs.color);
            if (dlssgPath && r.log != nullptr &&
                (hudless != fgLastHudless || fgInputLines.load(std::memory_order_relaxed) < 24))
            {
                fgInputLines.fetch_add(1, std::memory_order_relaxed);
                fgLastHudless = hudless;
                const auto hudlessDesc = inputs.color->GetDesc();
                const auto motionDesc = inputs.motion->GetDesc();
                const auto depthDesc = inputs.depth->GetDesc();
                std::fprintf(r.log,
                             "NATIVE_FG_INPUTS frame=%llu hudless=%llx %ux%u fmt%u motion=%llx %ux%u fmt%u "
                             "depth=%llx %ux%u fmt%u\n",
                             static_cast<unsigned long long>(GetGeometryCommandStats().lastFrame),
                             static_cast<unsigned long long>(hudless), static_cast<unsigned>(hudlessDesc.Width),
                             hudlessDesc.Height, static_cast<unsigned>(hudlessDesc.Format),
                             static_cast<unsigned long long>(reinterpret_cast<std::uintptr_t>(inputs.motion)),
                             static_cast<unsigned>(motionDesc.Width), motionDesc.Height,
                             static_cast<unsigned>(motionDesc.Format),
                             static_cast<unsigned long long>(reinterpret_cast<std::uintptr_t>(inputs.depth)),
                             static_cast<unsigned>(depthDesc.Width), depthDesc.Height,
                             static_cast<unsigned>(depthDesc.Format));
                std::fflush(r.log);
            }
        }
        std::lock_guard lock(r.mutex);
        entry = acquire(r, command, handle, inputs, controls);
        if (!entry)
            PublishRuntimeStatus(r.stopped                                    ? RuntimeStatus::Stopped
                                 : r.unavailable                              ? RuntimeStatus::Unavailable
                                 : r.active || r.retiring[0] || r.retiring[1] ? RuntimeStatus::Retiring
                                                                              : RuntimeStatus::Waiting);
        if (entry)
        {
            static std::atomic<unsigned> flowLogged { 0 };
            if (r.log && flowLogged.fetch_add(1, std::memory_order_relaxed) < 4)
            {
                std::fprintf(r.log, "GLASS_FLOW host_evaluate index=%u driverKeys=%u\n", inputs.index,
                             inputs.motionKey != nullptr && std::strcmp(inputs.motionKey, "DLSSG.MVecs") != 0 ? 1u
                                                                                                             : 0u);
                std::fflush(r.log);
            }
            ++entry->evaluations;
            if (inputs.index < 8)
                ++r.evaluationsByIndex[inputs.index];
            ++r.evaluationsByPath[pathIndex];
            D3D12_RESOURCE_STATES states[3] {};
            ID3D12Resource* resources[] = { inputs.motion, inputs.color, inputs.depth };
            InternalD3D12Scope ownCalls;
            StreamlineInputFrame frame;
            if (ReadStreamlineStates(handle, inputs.index, inputs.count, resources, states, &frame))
            {
                inputs.frame = frame.frame;
                if (r.log && inputs.index == 1 && !(++tagHits % 300))
                    std::fprintf(r.log, "NATIVE_TAG ok hits=%llu frame=%llu\n",
                                 static_cast<unsigned long long>(tagHits.load()),
                                 static_cast<unsigned long long>(frame.frame));
                prepared = entry->session.prepare(command, inputs, states, controls);
            }
            else
            {
                if (r.log && inputs.index == 1 && !(++tagMisses % 300))
                    std::fprintf(r.log, "NATIVE_TAG miss count=%llu\n",
                                 static_cast<unsigned long long>(tagMisses.load()));
                // Driver-level block: Streamline's frame generation plugin hands
                // the two textures straight to the NGX core, so no tag state and
                // no Streamline frame token exist. The DLSS-G input convention
                // delivers both in COPY_DEST. This is the block that reads the
                // parameter table (the tagged engine evaluations read nothing),
                // so the correction has to be prepared here as well.
                if (inputs.motionKey != nullptr)
                {
                    states[0] = D3D12_RESOURCE_STATE_COPY_DEST;
                    states[2] = D3D12_RESOURCE_STATE_COPY_DEST;
                    if (std::strcmp(inputs.motionKey, "DLSSG.MVecs") != 0)
                    {
                        // The packed capture numbers its frames with the engine's
                        // render frame (the draw packets), so the driver path has to
                        // use that same counter. A private sequence would never match
                        // and the capture would report acquire_no_candidate.
                        const auto commandsFrame = GetGeometryCommandStats().lastFrame;
                        const auto healthFrame = ReadGeometryHealth().frame;
                        const auto engineFrame = commandsFrame != 0 ? commandsFrame : healthFrame;
                        inputs.frame = engineFrame != 0 ? engineFrame : UINT64_MAX;
                        static std::atomic<unsigned> driverPrepared { 0 };
                        if (r.log && driverPrepared.fetch_add(1, std::memory_order_relaxed) < 4)
                        {
                            std::fprintf(r.log, "GLASS_FLOW driver_prepare frame=%llu\n",
                                         static_cast<unsigned long long>(inputs.frame));
                            std::fprintf(r.log, "GLASS_FLOW session_log=%p host_log=%p initialized=%u\n",
                                         static_cast<void*>(entry->session.logFileHandle()), static_cast<void*>(r.log),
                                         entry->session.initializedForDiagnostics() ? 1u : 0u);
                            std::fflush(r.log);
                        }
                    }
                    else
                    {
                        // The table carries both naming conventions and the
                        // provider reads its own: the parameter trace shows
                        // MotionVectors/Depth on exactly these token-less
                        // evaluations. Replacing DLSSG.MVecs only would leave the
                        // key the provider reads pointing at the engine texture.
                        inputs.motionKey = "MotionVectors";
                        inputs.depthKey = "Depth";
                    }
                    prepared = entry->session.prepare(command, inputs, states, controls, true);
                }
                else
                    entry->session.invalidateHistory();
            }
        }
    }
    else
    {
        std::lock_guard lock(r.mutex);
        // A caller that already verified the feature id still cannot be served
        // when the parameter table exposes neither naming convention; counting
        // that apart from the alias refusals keeps the missing correction
        // attributable to the read probe instead of to the identity gate.
        if (providerFrameGeneration || dlssgProviderModule)
            ++r.providerUnconfirmedEvaluations;
        // Bounded diagnostic: which keys the provider did not supply. Without it
        // a rejected frame leaves no trace at all.
        static std::atomic<unsigned> readMisses { 0 };
        if (readMisses.fetch_add(1, std::memory_order_relaxed) < 3)
        {
            if (!r.logAttempted)
            {
                r.logAttempted = true;
                r.log = _wfopen((Util::DllPath().parent_path() / L"OptiScaler.Glass.log").c_str(), L"a");
            }
            // The driver-level evaluate does not use the game's parameter names,
            // so the probe also reports which candidate keys are present at all.
            const char* keys[] { "DLSSG.MVecs", "DLSSG.HUDLess", "DLSSG.Depth", "DLSSG.MultiFrameIndex",
                                 "DLSSG.MultiFrameCount", "DLSSG.Reset", "DLSSG.ClipToPrevClip" };
            const char* probes[] { "MVecs", "MotionVectors", "DLSSG.MotionVectors", "Depth", "DLSSG.DepthInverted",
                                   "HudlessColor", "DLSSG.Color", "DLSSG.HudlessColor", "Width", "Height",
                                   "DLSS.Feature.Create.Flags", "DLSSG.OpticalFlowEnabled", "DLSSG.CameraNear",
                                   "DLSSG.CameraFar", "DLSSG.JitterOffset", "DLSSG.MVecScale" };
            if (r.log)
            {
                std::fprintf(r.log, "GLASS_READFAIL missing=");
                for (const auto* key : keys)
                {
                    void* value = nullptr;
                    if (parameters == nullptr || parameters->Get(key, &value) != 1)
                        std::fprintf(r.log, " %s", key);
                }
                std::fprintf(r.log, "\n");
                std::fprintf(r.log, "GLASS_READPROBE present=");
                for (const auto* key : probes)
                {
                    void* value = nullptr;
                    if (parameters != nullptr && parameters->Get(key, &value) == 1)
                        std::fprintf(r.log, " %s", key);
                }
                std::fprintf(r.log, "\n");
                std::fflush(r.log);
            }
        }
        if (r.active && r.active->handle == handle)
            r.active->session.invalidateHistory();
        PublishRuntimeStatus(RuntimeStatus::Waiting);
    }
    NVSDK_NGX_Result result;
    bool applied = false;
    // Native NGX can use other threads. Do not hold the global observer mutex
    // across its evaluation; the entry is retained until this call completes.
    try
    {
        ScopedInputs<NVSDK_NGX_Parameter> substitute(parameters, prepared);
        applied = substitute.applied();
        // Live diagnostic: hand the provider a forwarding wrapper so the keys it
        // actually reads (and whether the motion/depth pointers it gets are the
        // substituted textures) are recorded instead of assumed. Only while the
        // trace switch is on; the normal path passes the original pointer.
        if (controls.trace || controls.packedSupply)
        {
            auto& probe = NgxParameterProbeInstance(pathIndex);
            probe.bind(parameters, r.log, inputs.index, inputs.count, inputs.frame, prepared.motion, prepared.depth,
                       inputs.motion, inputs.depth, pathIndex);
            if (controls.packedSupply)
                probe.supply(prepared.motion, prepared.depth, inputs.motionKey, inputs.depthKey,
                             prepared.layerMvecs, prepared.layerOpacity);
            else
                probe.supply(nullptr, nullptr, nullptr, nullptr);
            // Per path, so the generator path records its own provider module even
            // when the alias path was bound first.
            static std::atomic<unsigned> probeBinds[2] { 0, 0 };
            const unsigned bindSlot = pathIndex == 0 ? 0u : 1u;
            if (r.log != nullptr && probeBinds[bindSlot].fetch_add(1, std::memory_order_relaxed) < 4)
            {
                wchar_t providerPath[MAX_PATH] {};
                HMODULE owner = nullptr;
                if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                           GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                       reinterpret_cast<LPCWSTR>(original), &owner) &&
                    owner != nullptr)
                    GetModuleFileNameW(owner, providerPath, MAX_PATH);
                std::fprintf(r.log,
                             "GLASS_PARAM_BIND path=%s provider=%ls function=%p applied=%u supply=%u index=%u frame=%llu\n",
                             pathIndex == 0 ? "dlssg" : "alias", providerPath, reinterpret_cast<void*>(original),
                             applied ? 1u : 0u,
                             controls.packedSupply ? 1u : 0u, inputs.index,
                             static_cast<unsigned long long>(inputs.frame));
                std::fflush(r.log);
            }
            result = original(command, handle, &probe, callback);
            // Per-call key lists for the generator path only: the upscaler and
            // Ray Reconstruction go through the same hook and would flood the
            // log with their own reads. Path 1 is reported once on demand.
            static std::atomic<unsigned> probeCalls { 0 }, aliasProbeCalls { 0 };
            if (controls.trace)
            {
                const auto call = pathIndex == 0 ? probeCalls.fetch_add(1, std::memory_order_relaxed)
                                                 : aliasProbeCalls.fetch_add(1, std::memory_order_relaxed);
                const bool early = call < 24;
                const bool sparse = call < 2000 && (call % 200) == 0;
                if (r.log != nullptr && (early || sparse))
                    probe.finish(r.log);
            }
            if (controls.trace && r.evaluations % 600 == 0)
                probe.report(r.log);
        }
        else
            result = original(command, handle, parameters, callback);
    }
    catch (...)
    {
        if (entry)
        {
            std::lock_guard lock(r.mutex);
            entry->session.invalidateHistory();
            --entry->evaluations;
        }
        throw;
    }
    // Live diagnostic (fgdump=N): copy the output the provider just wrote,
    // into the same command list, whether or not the inputs were substituted.
    if (frameGenerationEvaluation && result == NVSDK_NGX_Result_Success && FgOutputDumpWanted())
    {
        std::lock_guard lock(r.mutex);
        FgDumpPhase phase;
        // The driver-level table carries no usable index in Inputs (it is
        // filled with 1/1), so the multi-frame keys are read directly.
        phase.index = inputs.index;
        phase.count = inputs.count;
        unsigned value = 0;
        if (parameters != nullptr && parameters->Get("DLSSG.MultiFrameIndex", &value) == NVSDK_NGX_Result_Success)
            phase.index = value;
        if (parameters != nullptr && parameters->Get("DLSSG.MultiFrameCount", &value) == NVSDK_NGX_Result_Success)
            phase.count = value;
        phase.packedFrame = inputs.frame != UINT64_MAX;
        phase.frame = phase.packedFrame ? inputs.frame : r.evaluations;
        phase.applied = applied;
        RecordFgOutputDump(command, parameters, phase, fgDumpLog);
    }
    if (entry)
    {
        std::lock_guard lock(r.mutex);
        // Read back the shared parameter names instead of trusting the scope:
        // once the evaluate returned they must hold the engine's own textures
        // again, or the next feature reading them would receive our motion.
        if (applied)
        {
            ++r.restoreChecks;
            const char* motionNames[] { prepared.motionKey, prepared.motionAlias };
            const char* depthNames[] { prepared.depthKey, prepared.depthAlias };
            bool restored = true;
            for (const char* name : motionNames)
            {
                ID3D12Resource* current = nullptr;
                if (name != nullptr && parameters != nullptr && parameters->Get(name, &current) == 1 &&
                    current != prepared.originalMotion)
                    restored = false;
            }
            for (const char* name : depthNames)
            {
                ID3D12Resource* current = nullptr;
                if (name != nullptr && parameters != nullptr && parameters->Get(name, &current) == 1 &&
                    current != prepared.originalDepth)
                    restored = false;
            }
            if (!restored)
            {
                ++r.restoreFailures;
                if (r.log && r.restoreFailures <= 4)
                {
                    std::fprintf(r.log, "GLASS_PARAM_RESTORE failed count=%llu index=%u frame=%llu\n",
                                 static_cast<unsigned long long>(r.restoreFailures), inputs.index,
                                 static_cast<unsigned long long>(inputs.frame));
                    std::fflush(r.log);
                }
            }
        }
        --entry->evaluations;
        ++r.evaluations;
        r.lastResult = static_cast<unsigned>(result);
        r.substitutions += applied;
        if (applied && inputs.index < 8)
            ++r.substitutionsByIndex[inputs.index];
        if (applied)
            ++r.substitutionsByPath[pathIndex];
        if (prepared.motion != nullptr)
            ++r.preparedByPath[pathIndex];
        if (applied)
        {
            r.correctedMotion[r.correctedMotionNext++ & 3] = inputs.motion;
        }
        else
        {
            bool reused = false;
            for (auto* pointer : r.correctedMotion)
                if (pointer != nullptr && pointer == inputs.motion)
                {
                    reused = true;
                    break;
                }
            if (reused)
                ++r.unsubstitutedReusedMotion;
            else
                ++r.unsubstitutedFreshMotion;
        }
        if (applied && result == NVSDK_NGX_Result_Success)
        {
            const auto now = GetTickCount64();
            GeometryTelemetry::counts[GeometryFgReplacements].fetch_add(1, std::memory_order_relaxed);
            GeometryTelemetry::changedMs[GeometryFgReplacements].store(now, std::memory_order_relaxed);
            GeometryTelemetry::fgMs.store(now, std::memory_order_relaxed);
            // Substituted evaluation whose packed capture frame drew at least
            // one graft variant (engine MotionMatrix previous clip) with the
            // vertex-history fallback off.
            if (prepared.graftDraws > 0 && !VertexHistoryFallbackEnabled())
                NoteGeometryGraft(NativePreviousEvaluations);
        }
        PublishRuntimeStatus(applied && result == NVSDK_NGX_Result_Success ? RuntimeStatus::Correcting
                                                                           : RuntimeStatus::Waiting);
        if (result != NVSDK_NGX_Result_Success || (prepared.motion && !applied))
            entry->session.invalidateHistory();
        if (auto timing = entry->session.pollTiming())
            PublishGpuMilliseconds(timing->milliseconds);
        // Live panel state: the swap counters plus the compose fence pair that
        // proves our own GPU work is not outliving the session.
        PublishLiveStatus(LiveStatusEvaluations, r.evaluations);
        PublishLiveStatus(LiveStatusSubstitutions, r.substitutions);
        PublishLiveStatus(LiveStatusComposeSubmitted, r.activeComposeSubmitted());
        PublishLiveStatus(LiveStatusComposeCompleted, r.activeComposeCompleted());
        PublishLiveStatus(LiveStatusComposeForced, r.activeComposeForced());
        PublishLiveStatus(LiveStatusComposeInFlight, r.anyComposeInFlight() ? 1u : 0u);
        PublishLiveStatus(LiveStatusUnavailable, r.unavailable ? 1u : 0u);
        PublishLiveStatus(LiveStatusRetiring, (r.retiring[0] ? 1u : 0u) + (r.retiring[1] ? 1u : 0u));
        // The periodic report is formatted by the host's background thread
        // (ReportNativeHostLog). It used to run here, inside the frame generation
        // callback, which is the engine's render thread: ~30 formatted lines plus
        // a flush while holding the runtime mutex.
        r.reap();
    }
    return result;
}

void RetireNativeFG(const NVSDK_NGX_Handle* handle)
{
    auto& r = runtime();
    std::lock_guard lock(r.mutex);
    if (r.active && r.active->handle == handle)
        r.retire();
    // Only a released frame generation feature ends an output dump; the
    // upscaler and Ray Reconstruction are released through the same call.
    if (handle != nullptr && ProviderConfirmsFrameGeneration(static_cast<const void*>(handle), false))
        RetireFgOutputDump(fgDumpLog);
    ForgetRememberedFrameGenerationHandle(static_cast<const void*>(handle), handle != nullptr ? handle->Id : 0u);
    r.reap();
}
void StopNativeFG()
{
    auto& r = runtime();
    std::lock_guard lock(r.mutex);
    if (r.log)
    {
        std::fprintf(r.log, "NATIVE_HOST stop=1 evaluations=%llu substitutions=%llu captures=%llu\n",
                     r.evaluations, r.substitutions, r.captures);
        std::fflush(r.log);
    }
    r.stopped = true;
    r.retire();
    RetireFgOutputDump(fgDumpLog);
    r.reap();
}
void CreatedNativeFG()
{
    auto& r = runtime();
    std::lock_guard lock(r.mutex);
    if (r.log)
    {
        std::fprintf(r.log, "NATIVE_HOST created_native_fg=1\n");
        std::fflush(r.log);
    }
    // Only the host's successful native FG creation reopens admission after
    // shutdown. Retiring recordings still prevent allocation until drained.
    r.stopped = false;
    r.reap();
}
} // namespace GlassFg
