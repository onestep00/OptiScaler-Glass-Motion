#pragma once
#include "GlassFgPass.h"
#include "PackedMotionPass.h"
#include "PackedMotionCapture.h"
#include "ComputeRecording.h"
#include "GeometryCommands.h"
#include "CommandLifetime.h"
#include "SurfaceQueueLink.h"
#include "SurfaceSnapshotPool.h"
#include <array>
#include <optional>

namespace GlassFg
{
// One native FG feature on one COMPUTE queue. The platform adapter holds its
// lock across real queue calls plus these callbacks, and suppresses callbacks
// only while recording this module's own commands. It identifies the surface
// with CyberpunkSurfacePass before calling captureIdentifiedSurface.
class NativeSession
{
    Pass pass;
    PackedMotionPass objectPass;
    PackedMotionProvider objectProvider;
    SurfaceSnapshotPool pool;
    SurfaceQueueLink link;
    ComputeRecording recording;
    GpuTimer timer;
    FILE* log = nullptr;
    ID3D12GraphicsCommandList* fgCommand = nullptr;
    // The FG path decides the list type: the game's own NGX passthrough submits
    // a compute list, while Streamline's DLSS-G evaluation arrives on a direct
    // list. bindFgCommand used to accept only compute and marked the whole
    // session unavailable for the Streamline path.
    D3D12_COMMAND_LIST_TYPE fgCommandType = D3D12_COMMAND_LIST_TYPE_COMPUTE;
    ID3D12CommandQueue* fgQueue = nullptr;
    ID3D12Fence* completion = nullptr;
    // The engine alternating between two frame generation command lists (one
    // per back buffer) is the observed reality: the same inputs and the same
    // feature instance arrive on a different list every other evaluation.
    // Binding a single list retired and rebuilt the whole session each frame,
    // which reallocated the packed outputs and cancelled the pending compose.
    // The engine rebuilds its frame generation lists on focus regain, swap-chain
    // changes and frame generation restarts, so four identities were not enough:
    // the fifth list retired and rebuilt the whole session (packed outputs,
    // pipeline creation) inside the evaluation callback, which is the frame
    // stall and the engine assert that followed. The set holds the lists the
    // engine still submits; the least recently adopted identity is dropped when
    // it is full.
    static constexpr unsigned kFgCommandSlots = 16;
    ID3D12GraphicsCommandList* fgCommands[kFgCommandSlots] {};
    // The list type belongs to the command list, not to the session: Streamline
    // submits the frame generation list on a compute queue for the driver-level
    // block and on a direct queue for the tagged evaluation. A session-wide
    // type made the second submission look illegal and failed the session, and
    // a failed session could never be released again.
    D3D12_COMMAND_LIST_TYPE fgTypes[kFgCommandSlots] {};
    CommandLifetime fgLifetimes[kFgCommandSlots];
    // Adoption order per slot, used only to pick the victim when the identity
    // set is full.
    std::uint64_t fgUse[kFgCommandSlots] {};
    std::uint64_t fgUseClock = 0;
    unsigned fgCommandCount = 0;
    std::array<CommandLifetime, 64> producers {};
    std::array<ID3D12Fence*, 64> nativeFences {};
    SurfaceSnapshotPool::Token newest {};
    SurfaceSnapshot batchSnapshot {};
    uint64_t generation = 0, submitted = 0;
    unsigned candidates = 0;
    bool initialized = false, stopped = false, failed = false;
    bool outputRecording = false, timing = false, objectMode = false;
    // Our compose list is queued on the FG queue immediately before the batch
    // that carries the FG command. Nothing else covers it with the completion
    // fence while the input swap is inactive, so it is tracked explicitly and
    // the teardown waits for it before freeing the packed outputs.
    bool composeInFlight = false;
    // Producer dependency for the current packed frame, consumed by the host's
    // pre-submit hook on the queue that executes the FG command list.
    ID3D12Fence* pendingProducerFence = nullptr;
    std::uint64_t pendingProducerValue = 0;
    // Producer queue of the acquired frame; a wait for it on the same queue is a
    // self wait and has to be skipped.
    void* pendingProducerQueue = nullptr;

    bool retainProducer(ID3D12GraphicsCommandList* command)
    {
        collectDestroyed();
        for (const auto& entry : producers)
            if (entry.identity() == command)
                return true;
        for (auto& entry : producers)
            if (!entry.identity())
                return entry.attach(command);
        return false;
    }

    void discardRecording(const void* command, bool destroyed = false)
    {
        objectPass.discardRecording(command);
        if (objectMode)
            objectProvider.discard(command, destroyed);
        else
        {
            pool.discardRecording(command);
            link.resetCommand(command);
        }
        timer.discardRecording(command);
        if (knowsFgCommand(static_cast<ID3D12GraphicsCommandList*>(const_cast<void*>(command))))
            outputRecording = false;
    }

    bool knowsFgCommand(ID3D12GraphicsCommandList* command) const
    {
        if (command == nullptr)
            return false;
        for (unsigned i = 0; i < kFgCommandSlots; ++i)
            if (fgCommands[i] == command)
                return true;
        return false;
    }

    ID3D12GraphicsCommandList* firstKnownFgCommand() const
    {
        for (unsigned i = 0; i < kFgCommandSlots; ++i)
            if (fgCommands[i] != nullptr)
                return fgCommands[i];
        return nullptr;
    }

    // Slot of a known frame generation list, or kFgCommandSlots when the list
    // does not belong to this session.
    unsigned fgCommandSlot(ID3D12GraphicsCommandList* command) const
    {
        for (unsigned i = 0; i < kFgCommandSlots; ++i)
            if (fgCommands[i] == command)
                return i;
        return kFgCommandSlots;
    }

    // Drops a list the engine destroyed and re-arms the primary identity. The
    // watch state is dropped rather than moved: CommandLifetime is intentionally
    // not movable, and a leftover callback token only publishes a flag.
    void forgetFgCommand(ID3D12GraphicsCommandList* command)
    {
        for (unsigned i = 0; i < kFgCommandSlots; ++i)
            if (fgCommands[i] == command)
            {
                fgCommands[i] = nullptr;
                fgTypes[i] = D3D12_COMMAND_LIST_TYPE_COMPUTE;
                fgLifetimes[i].forget();
                if (fgCommand == command)
                    fgCommand = firstKnownFgCommand();
                return;
            }
    }

    void collectDestroyed()
    {
        for (unsigned i = 0; i < kFgCommandSlots; ++i)
        {
            const auto command = fgLifetimes[i].takeDestroyed();
            if (command == nullptr)
                continue;
            // A destroyed frame generation list only loses its identity: the
            // session keeps its packed outputs and re-arms on the next list.
            static std::atomic<unsigned> logged { 0 };
            if (log != nullptr && logged.fetch_add(1, std::memory_order_relaxed) < 4)
            {
                std::fprintf(log, "NATIVE_SESSION fg_command_destroyed=1\n");
                std::fflush(log);
            }
            recording.onMutation(static_cast<ID3D12GraphicsCommandList*>(const_cast<void*>(command)));
            discardRecording(command, true);
            forgetFgCommand(static_cast<ID3D12GraphicsCommandList*>(const_cast<void*>(command)));
        }
        for (auto& producer : producers)
            if (const auto command = producer.takeDestroyed())
                discardRecording(command, true);
    }

  public:
    NativeSession() = default;
    NativeSession(const NativeSession&) = delete;
    NativeSession& operator=(const NativeSession&) = delete;

    bool initialize(ID3D12Device* device, const D3D12_RESOURCE_DESC (&descs)[3], const wchar_t* seedShader,
                    const wchar_t* regionShader, FILE* log)
    {
        if (!device || initialized || completion || stopped || failed)
            return false;
        if (!pass.initialize(device, descs, seedShader, regionShader, log) ||
            !pool.initialize(device, static_cast<UINT>(descs[0].Width), descs[0].Height) ||
            FAILED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&completion))))
        {
            releaseAfterGpuDrain(); // No commands can have been recorded yet.
            failed = true;
            return false;
        }
        timing = timer.initialize(device); // Optional measurement must not disable correction.
        this->log = log;
        initialized = true;
        return true;
    }

    bool initializePacked(ID3D12Device* device, const D3D12_RESOURCE_DESC (&descs)[3], const wchar_t* shader,
                          FILE* log, PackedMotionProvider provider)
    {
        if (!device || !provider || initialized || completion || stopped || failed)
            return false;
        if (!objectPass.initialize(device, descs, shader, log) ||
            FAILED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&completion))))
        {
            objectPass.releaseAfterGpuDrain();
            failed = true;
            return false;
        }
        timing = timer.initialize(device);
        this->log = log;
        objectMode = true;
        objectProvider = provider;
        initialized = true;
        return true;
    }

    // The interface identity and complete method coverage come from the actual
    // host hooks. Never call repeatedly: rebinding would erase Reset evidence.
    bool bindFgCommand(ID3D12GraphicsCommandList* command, uint32_t supported, uint32_t observed)
    {
        if (!initialized || stopped || failed || !command || fgCommand)
            return false;
        return adoptFgCommand(command, supported, observed);
    }

    ID3D12GraphicsCommandList* fgCommandIdentity() const { return fgCommand; }
    // Public view of the identity set: the host matches submitted batches and
    // evaluations against every list the session accepted.
    bool handlesFgCommand(ID3D12GraphicsCommandList* command) const { return knowsFgCommand(command); }

    // Adds a command list identity to this session. The engine alternates
    // between frame generation command lists (one per back buffer) for the same
    // inputs, so both have to belong to the same session; only the identity is
    // added, the packed outputs and the compose list stay alive.
    bool adoptFgCommand(ID3D12GraphicsCommandList* command, uint32_t supported, uint32_t observed)
    {
        if (!initialized || stopped || failed || !command)
            return false;
        if (command == fgCommand)
            return true;
        if (knowsFgCommand(command))
        {
            fgCommand = command;
            return true;
        }
        const auto type = command->GetType();
        if (type != D3D12_COMMAND_LIST_TYPE_COMPUTE && type != D3D12_COMMAND_LIST_TYPE_DIRECT)
            return false;
        unsigned slot = kFgCommandSlots;
        for (unsigned i = 0; i < kFgCommandSlots; ++i)
            if (fgCommands[i] == nullptr)
            {
                slot = i;
                break;
            }
        if (slot == kFgCommandSlots)
        {
            // Full set: drop the least recently adopted identity instead of
            // retiring the session. The list the engine is evaluating right now
            // is never the victim.
            for (unsigned i = 0; i < kFgCommandSlots; ++i)
                if (fgCommands[i] != fgCommand && (slot == kFgCommandSlots || fgUse[i] < fgUse[slot]))
                    slot = i;
            if (slot == kFgCommandSlots)
                return false;
            static std::atomic<unsigned> evicted { 0 };
            if (log != nullptr && evicted.fetch_add(1, std::memory_order_relaxed) < 8)
            {
                std::fprintf(log, "NATIVE_SESSION fg_command_evicted slot=%u command=%p\n", slot,
                             static_cast<void*>(fgCommands[slot]));
                std::fflush(log);
            }
            // Only the identity watch is dropped: the list may still be alive
            // and its recording belongs to the frame slot that reuses it.
            fgLifetimes[slot].forget();
            fgCommands[slot] = nullptr;
            fgTypes[slot] = D3D12_COMMAND_LIST_TYPE_COMPUTE;
            fgUse[slot] = 0;
        }
        if (!link.registerFgCommand(command) || !fgLifetimes[slot].attach(command))
            return false;
        fgCommands[slot] = command;
        fgTypes[slot] = type;
        fgUse[slot] = ++fgUseClock;
        fgCommand = command;
        fgCommandType = type;
        recording.bind(command, true, supported, observed);
        return true;
    }

    void onReset(ID3D12GraphicsCommandList* command, bool success, ID3D12PipelineState* initialPipeline)
    {
        collectDestroyed();
        recording.onReset(command, success, initialPipeline);
        if (!success)
            return;
        discardRecording(command);
        if (stopped && knowsFgCommand(command))
            for (auto& lifetime : fgLifetimes)
                if (lifetime.identity() == command)
                    lifetime.detachLive(command);
        for (auto& producer : producers)
            if (producer.identity() == command)
                producer.detachLive(command);
    }

    // Includes Close and every applicable state setter, including predication.
    void onStateMutation(ID3D12GraphicsCommandList* command) { recording.onMutation(command); }

    // A compute command list may be executed on a compute or a direct queue; a
    // direct list only on a direct queue. Streamline's frame-generation plugin
    // submits the same list on both queue types, so the queue check must allow
    // that instead of pinning one queue identity.
    static bool compatibleQueue(D3D12_COMMAND_LIST_TYPE list, D3D12_COMMAND_LIST_TYPE queue) noexcept
    {
        return list == queue ||
               (list == D3D12_COMMAND_LIST_TYPE_COMPUTE && queue == D3D12_COMMAND_LIST_TYPE_DIRECT);
    }

    // Called immediately AFTER the verified game's depth transition. Multiple
    // matching candidates between phase-1 evaluations make that batch ambiguous.
    bool captureIdentifiedSurface(ID3D12GraphicsCommandList* command, ID3D12Resource* depth,
                                  D3D12_RESOURCE_STATES state)
    {
        collectDestroyed();
        if (objectMode)
            return false;
        if (!initialized || stopped || failed || !command || !depth ||
            command->GetType() != D3D12_COMMAND_LIST_TYPE_DIRECT || !retainProducer(command))
            return false;
        candidates = std::min(candidates + 1, 2u);
        pool.retire(newest);
        newest = pool.capture(command, depth, state, ++generation);
        if (!newest || !link.recordSurface(command, generation))
            return false;
        return true;
    }

    // Called after the real ExecuteCommandLists while still serialized with
    // subsequent native Signal/Wait/Reset. Completion signals serve ownership;
    // the timer reuses this stream and never adds a signal of its own.
    bool afterSubmit(ID3D12CommandQueue* queue, UINT count, ID3D12CommandList* const* commands)
    {
        collectDestroyed();
        if (!initialized || failed || !queue || !commands || !count)
            return false;
        bool usesOutput = false;
        ID3D12GraphicsCommandList* matched = nullptr;
        for (UINT i = 0; i < count; ++i)
        {
            auto* submitted = static_cast<ID3D12GraphicsCommandList*>(commands[i]);
            const auto slot = fgCommandSlot(submitted);
            if (slot != kFgCommandSlots)
            {
                matched = submitted;
                const auto queueType = queue->GetDesc().Type;
                // The list carries its own type: Streamline submits the same
                // frame generation list on a compute queue for the driver-level
                // block and on a direct queue for the tagged evaluation. A
                // session-wide type turned the second case into a permanent
                // session failure, and a failed session could never be released
                // or replaced, which left every later frame uncorrected.
                if (!compatibleQueue(fgTypes[slot], queueType))
                {
                    static std::atomic<unsigned> incompatible { 0 };
                    if (log)
                    {
                        if (incompatible.fetch_add(1, std::memory_order_relaxed) < 8)
                        {
                            std::fprintf(log,
                                         "NATIVE_HOST fg_queue_incompatible list=%u queue_type=%u queue=%p\n",
                                         static_cast<unsigned>(fgTypes[slot]), static_cast<unsigned>(queueType),
                                         static_cast<void*>(queue));
                            std::fflush(log);
                        }
                    }
                }
                else if (fgQueue != queue)
                {
                    // The engine rebuilds its frame-generation queue when the
                    // feature or the swap chain is recreated (resolution and
                    // quality changes). The compose list matches the type and
                    // the fences are device objects, so follow the engine
                    // instead of leaving the correction permanently off.
                    // The engine alternates two queues every frame, so this is
                    // bounded: the log reached 68k duplicate pairs in one session.
                    static std::atomic<unsigned> queueAdoptions { 0 };
                    if (log && queueAdoptions.fetch_add(1, std::memory_order_relaxed) < 8)
                    {
                        std::fprintf(log, "NATIVE_HOST fg_queue adopt=1 old=%p new=%p type=%u\n",
                                     static_cast<void*>(fgQueue), static_cast<void*>(queue),
                                     static_cast<unsigned>(queueType));
                        std::fflush(log);
                    }
                    if (fgQueue)
                        fgQueue->Release();
                    fgQueue = queue;
                    fgQueue->AddRef();
                    fgCommandType = fgTypes[slot];
                }
                // A deferred compose was queued on this queue just before the
                // batch, so the completion signal covers it as well as a
                // substituted input.
                usesOutput |= outputRecording || composeInFlight;
            }
            if (!objectMode)
                link.submit(queue, commands[i]);
        }
        if (!objectMode && (!pool.afterSubmit(queue, count, commands) || !link.healthy()))
            failed = true;
        if (usesOutput)
        {
            if (!fgQueue || fgQueue != queue || !compatibleQueue(fgCommandType, queue->GetDesc().Type))
                failed = true;
            else
            {
                if (FAILED(queue->Signal(completion, ++submitted)))
                    failed = true;
                else
                {
                    composeInFlight = false;
                    if (timing && matched != nullptr && !timer.submitted(matched, queue, completion, submitted))
                        timing = false;
                }
            }
        }
        return !failed;
    }

    // Only native, successful GPU synchronization calls belong here. Exclude
    // this module's own completion signals; they do not establish input order.
    void onSignal(ID3D12CommandQueue* queue, ID3D12Fence* fence, uint64_t value)
    {
        if (objectMode || failed || !fence || !link.isProducerQueue(queue))
            return;
        bool retained = false;
        for (auto entry : nativeFences)
            retained |= entry == fence;
        if (!retained)
            for (auto& entry : nativeFences)
                if (!entry)
                {
                    fence->AddRef();
                    entry = fence;
                    retained = true;
                    break;
                }
        if (!retained)
        {
            failed = true;
            return;
        }
        link.signal(queue, fence, value);
    }
    void onWait(ID3D12CommandQueue* queue, ID3D12Fence* fence, uint64_t value)
    {
        if (!objectMode && !failed && queue == fgQueue)
            link.wait(queue, fence, value);
    }

    PreparedInputs prepare(ID3D12GraphicsCommandList* command, const Inputs& inputs,
                           const D3D12_RESOURCE_STATES (&states)[3], Controls controls,
                           bool allowAnyState = false)
    {
        static std::atomic<unsigned> entered { 0 };
        if (log != nullptr && entered.fetch_add(1, std::memory_order_relaxed) < 6)
        {
            std::fprintf(log, "NATIVE_PREPARE enter index=%u objectMode=%u trace=%u active=%u initialized=%u\n",
                         inputs.index, objectMode ? 1u : 0u, controls.trace ? 1u : 0u, controls.active() ? 1u : 0u,
                         initialized ? 1u : 0u);
            std::fflush(log);
        }
        collectDestroyed();
        if (!initialized || stopped || failed || !knowsFgCommand(command))
        {
            // Bounded attribution for a frame the host admitted but the session
            // then refused, which otherwise leaves no trace at all.
            static std::atomic<unsigned> rejected { 0 };
            if (log != nullptr && rejected.fetch_add(1, std::memory_order_relaxed) < 6)
            {
                std::fprintf(log, "NATIVE_PREPARE skip initialized=%u stopped=%u failed=%u cmdMatch=%u\n",
                             initialized ? 1u : 0u, stopped ? 1u : 0u, failed ? 1u : 0u,
                             knowsFgCommand(command) ? 1u : 0u);
                std::fflush(log);
            }
            return {};
        }
        if (objectMode)
        {
            // The colour of this evaluation, named by the frame generation call
            // itself. Recorded on every evaluation and read only while a dump is
            // outstanding, so the frame path pays two stores. Only the packed
            // object path owns the dump, which is why this sits inside it.
            if (inputs.color != nullptr)
                objectPass.setDumpColor(inputs.color, states[1]);
            PackedMotionFrame objectFrame;
            if (inputs.index == 1)
            {
                auto ticket = recording.begin(command);
                const bool fresh = ticket && recording.finish(command, ticket);
                // The pristine-list requirement (Reset observed, nothing set
                // since) cannot hold for the graphics list Streamline's frame
                // generation plugin evaluates on: it resets and then binds its
                // own state before calling the NGX core. The compose runs on
                // this module's own list, so the engine list's state is not a
                // precondition there; allowAnyState carries that distinction.
                // The pristine-list requirement ("Reset seen, nothing set
                // since") cannot hold for the graphics list Streamline's frame
                // generation plugin evaluates on: it resets and then binds its
                // own state before calling the NGX core. In packed mode the
                // compose runs on this module's own list, so the engine list's
                // binding history is not a precondition for the substitution.
                if (!controls.active() || !inputs.valid() || (!fresh && !allowAnyState && !objectMode))
                {
                    static std::atomic<unsigned> rejectLog { 0 };
                    if (log != nullptr && rejectLog.fetch_add(1, std::memory_order_relaxed) < 6)
                    {
                        std::fprintf(log, "NATIVE_PREPARE reject active=%u valid=%u fresh=%u ticket=%u\n",
                                     controls.active() ? 1u : 0u, inputs.valid() ? 1u : 0u, fresh ? 1u : 0u,
                                     ticket ? 1u : 0u);
                        std::fflush(log);
                    }
                    objectPass.invalidateHistory();
                    return {};
                }
                const auto description = inputs.motion->GetDesc();
                objectFrame = objectProvider.acquire(command, static_cast<std::uint32_t>(description.Width),
                                                     description.Height, inputs.frame, inputs.reset != 0);
                // A frame can be evaluated more than once (one per back-buffer
                // command list). Only the evaluation that actually received a
                // packed frame may publish the producer dependency; an empty
                // acquire used to overwrite it with zero and the pre-submit hook
                // then found nothing to queue the compose with.
                if (objectFrame)
                {
                    pendingProducerFence = objectFrame.producerFence;
                    pendingProducerValue = objectFrame.producerValue;
                    pendingProducerQueue = objectFrame.producerQueue;
                }
            }
            auto prepared = objectPass.prepare(command, inputs, objectFrame, states, controls,
                                               timing ? &timer : nullptr, allowAnyState,
                                               GetGeometryCommandStats().lastFrame);
            outputRecording |= prepared.motion != nullptr;
            if (controls.trace && log && TraceWanted())
            {
                // The motion pointer separates a harmless second evaluation of an
                // already corrected frame from an evaluation whose frame never
                // received the correction at all.
                std::fprintf(log,
                             "TRACE_SUBSTITUTE index=%u applied=%u frame=%llu count=%u motion=%p command=%p "
                             "scale=%.6f,%.6f\n",
                             inputs.index, prepared.motion ? 1u : 0u,
                             static_cast<unsigned long long>(inputs.frame), inputs.count,
                             static_cast<void*>(inputs.motion), static_cast<void*>(command), inputs.scaleX,
                             inputs.scaleY);
                std::fflush(log);
            }
            return prepared;
        }
        if (inputs.index == 1)
        {
            batchSnapshot = {};
            const auto candidateCount = candidates;
            candidates = 0;
            const auto ordered = link.generationForFgCommand(command);
            auto ticket = recording.begin(command);
            const bool fresh = ticket && recording.finish(command, ticket);
            if (!controls.active() || candidateCount != 1 || !ordered || !newest || newest.generation != ordered ||
                !inputs.valid() || !fresh)
            {
                pass.invalidateHistory();
                pool.retire(newest);
                return {};
            }
            auto* surface = pool.beginRead(newest, command, ordered);
            if (!surface)
            {
                pass.invalidateHistory();
                pool.retire(newest);
                return {};
            }
            batchSnapshot = { surface, ordered, D3D12_RESOURCE_STATE_COMMON };
        }
        auto prepared = pass.prepare(command, inputs, batchSnapshot, states, controls, timing ? &timer : nullptr);
        if (inputs.index == 1)
        {
            pool.retire(newest); // Only the phase-1 copy reads the surface snapshot.
            if (prepared.motion)
                command->ClearState(nullptr); // Admission proved fresh bindings under the host lock.
        }
        outputRecording |= prepared.motion != nullptr;
        return prepared;
    }

    void nativeFailure() { objectMode ? objectPass.invalidateHistory() : pass.invalidateHistory(); }
    bool accepting()
    {
        collectDestroyed();
        return initialized && !stopped && !failed;
    }
    void bypass(unsigned index)
    {
        if (objectMode)
        {
            objectPass.invalidateHistory();
            return;
        }
        pass.invalidateHistory();
        if (index == 1)
        {
            candidates = 0;
            batchSnapshot = {};
            pool.retire(newest);
        }
    }
    std::optional<GpuTimer::Sample> pollTiming() { return timing ? timer.poll() : std::optional<GpuTimer::Sample> {}; }
    // Live debug channel: recompile the packed compose shader in place.
    bool reloadPackedShader(const wchar_t* shader, FILE* log)
    {
        return objectMode && objectPass.reloadShader(shader, log);
    }
    // Live diagnostics: motion/depth dump and submit correlation.
    void requestDump()
    {
        if (objectMode)
            objectPass.requestDump();
    }
    bool serviceDump() { return objectMode && objectPass.serviceDump(); }
    FILE* logFileHandle() const { return log; }
    bool initializedForDiagnostics() const { return initialized; }
    // Deferred compose submission (called from the host pre-submit hook).
    bool pendingFor(const void* command) const { return objectMode && objectPass.pendingFor(command); }
    bool executePending(ID3D12CommandQueue* queue, const void* submittedCommand)
    {
        if (!objectMode || !objectPass.executePending(queue, submittedCommand))
            return false;
        composeInFlight = true;
        return true;
    }
    // Second consumer (DLSS-NR). The neural rendering pass runs on the game's
    // own command list, ahead of the frame generation batch, so the frame is
    // composed inline here and handed back as this evaluate's guides. The frame
    // generation substitution of the same frame reuses the pair.
    bool secondConsumerGuides(ID3D12GraphicsCommandList* command, ID3D12Resource* motion, ID3D12Resource* depth,
                              D3D12_RESOURCE_STATES motionArrival, D3D12_RESOURCE_STATES depthArrival, float jitterX,
                              float jitterY, float scaleX, float scaleY, ID3D12Resource** outMotion,
                              ID3D12Resource** outDepth)
    {
        if (outMotion != nullptr)
            *outMotion = nullptr;
        if (outDepth != nullptr)
            *outDepth = nullptr;
        if (!objectMode || !initialized || stopped || failed || command == nullptr || motion == nullptr ||
            depth == nullptr)
            return false;
        const auto controls = ReadControls();
        if (!controls.active() || !controls.nrMotion)
            return false;
        const auto description = motion->GetDesc();
        if (description.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || description.Width == 0 ||
            description.Height == 0)
            return false;
        const auto engineFrame = GetGeometryCommandStats().lastFrame;
        auto frame = objectProvider.acquireSecondConsumer(static_cast<std::uint32_t>(description.Width),
                                                         description.Height, engineFrame);
        if (!frame)
            return false;
        // The compose copies from the game's own motion and depth, so it starts
        // from the states the caller declares for them. The composed pair it
        // hands back is left in the frame generation input convention
        // (COPY_DEST), which is what the pass is told for its own guides.
        if (!objectPass.composeInline(command, frame, motion, depth, motionArrival, depthArrival, jitterX, jitterY,
                                      scaleX, scaleY, controls))
            return false;
        if (outMotion != nullptr)
            *outMotion = objectPass.motionOutput();
        if (outDepth != nullptr)
            *outDepth = objectPass.depthOutput();
        return true;
    }
    // Release diagnostics for the host log: proves that our own GPU work never
    // outlives the packed outputs it writes into.
    bool packedComposeInFlight() const
    {
        return composeInFlight || (objectMode && objectPass.composeInFlight());
    }
    std::uint64_t packedComposeSubmitted() const { return objectMode ? objectPass.composeSubmitted() : 0; }
    std::uint64_t packedComposeCompleted() const { return objectMode ? objectPass.composeCompleted() : 0; }
    std::uint64_t packedComposeForced() const { return objectMode ? objectPass.composeForced() : 0; }
    // The compose runs on the queue that owns the packed records; the frame
    // generation queue waits on this fence instead of cross-queue reading them.
    ID3D12Fence* composeFence() const { return objectMode ? objectPass.composeFence() : nullptr; }
    // Cross-queue dependency of the packed raster read. The caller performs the
    // wait on the queue that submits the FG command list.
    bool takeProducerWait(ID3D12Fence*& fence, std::uint64_t& value, void*& producerQueue)
    {
        fence = pendingProducerFence;
        value = pendingProducerValue;
        producerQueue = pendingProducerQueue;
        pendingProducerFence = nullptr;
        pendingProducerValue = 0;
        pendingProducerQueue = nullptr;
        return fence != nullptr && value != 0;
    }
    void dumpSubmitted(ID3D12CommandQueue* queue)
    {
        if (objectMode)
            objectPass.dumpSubmitted(queue);
    }
    uint64_t renderedDispatches() const
    {
        return objectMode ? objectPass.renderedDispatches() : pass.renderedDispatches();
    }
    ID3D12Resource* selection() const { return objectMode ? objectPass.selection() : pass.selection(); }
    ID3D12Resource* failures() { return objectMode ? nullptr : pass.failures(); }

    void stop()
    {
        stopped = true;
        if (objectMode)
        {
            objectPass.invalidateHistory();
            // Retired sessions must not submit a compose that was prepared for
            // the frame they no longer own.
            objectPass.cancelPending();
        }
        else
        {
            pass.invalidateHistory();
            pool.retire(newest);
        }
    }

    bool readyToRelease()
    {
        collectDestroyed();
        // The completion fence only covers a compose once the batch that carried
        // the FG command was submitted, so the packed fence is the direct proof
        // that our own GPU work finished. Checking it first also releases the
        // in-flight flag when the signal path never ran.
        if (objectMode && !objectPass.drained())
            return false;
        composeInFlight = false;
        // A failed session still has to become releasable: the host keeps at
        // most two retiring slots, and a session that could never be released
        // blocked every later substitution for the rest of the process.
        if (!stopped || outputRecording || (!objectMode && !pool.idle()))
            return false;
        for (const auto& producer : producers)
            if (producer.identity())
                return false;
        if (!submitted)
            return true;
        auto done = completion->GetCompletedValue();
        return done != UINT64_MAX && done >= submitted;
    }

    // Caller first stops admission, discards all outstanding recordings and
    // verifies completion (readyToRelease or an explicit drain/device teardown).
    // No hidden destructor releases resources still used by a GPU recording.
    void releaseAfterGpuDrain()
    {
        composeInFlight = false;
        if (objectMode)
        {
            objectPass.cancelPending();
            objectPass.releaseAfterGpuDrain();
        }
        else
        {
            pass.releaseAfterGpuDrain();
            pool.releaseAfterGpuDrain();
        }
        timer.releaseAfterGpuDrain();
        for (auto& producer : producers)
            producer.forget();
        for (unsigned i = 0; i < kFgCommandSlots; ++i)
        {
            fgLifetimes[i].forget();
            fgCommands[i] = nullptr;
        }
        for (auto& fence : nativeFences)
            if (fence)
            {
                fence->Release();
                fence = nullptr;
            }
        if (fgQueue)
            fgQueue->Release();
        if (completion)
            completion->Release();
        fgCommand = nullptr;
        fgQueue = nullptr;
        completion = nullptr;
        initialized = false;
        objectMode = false;
        objectProvider = {};
        stopped = true;
    }
};
} // namespace GlassFg
