#pragma once
#include "GlassFgPass.h"
#include "ComputeRecording.h"
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
    SurfaceSnapshotPool pool;
    SurfaceQueueLink link;
    ComputeRecording recording;
    GpuTimer timer;
    ID3D12GraphicsCommandList* fgCommand = nullptr;
    ID3D12CommandQueue* fgQueue = nullptr;
    ID3D12Fence* completion = nullptr;
    CommandLifetime fgLifetime;
    std::array<CommandLifetime, 64> producers {};
    std::array<ID3D12Fence*, 64> nativeFences {};
    SurfaceSnapshotPool::Token newest {};
    SurfaceSnapshot batchSnapshot {};
    uint64_t generation = 0, submitted = 0;
    unsigned candidates = 0;
    bool initialized = false, stopped = false, failed = false;
    bool outputRecording = false, timing = false;

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

    void discardRecording(const void* command)
    {
        pool.discardRecording(command);
        link.resetCommand(command);
        timer.discardRecording(command);
        if (command == fgCommand)
            outputRecording = false;
    }

    void collectDestroyed()
    {
        if (const auto command = fgLifetime.takeDestroyed())
        {
            recording.onMutation(command);
            discardRecording(command);
            fgCommand = nullptr;
            stop();
        }
        for (auto& producer : producers)
            if (const auto command = producer.takeDestroyed())
                discardRecording(command);
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
        initialized = true;
        return true;
    }

    // The interface identity and complete method coverage come from the actual
    // host hooks. Never call repeatedly: rebinding would erase Reset evidence.
    bool bindFgCommand(ID3D12GraphicsCommandList* command, uint32_t supported, uint32_t observed)
    {
        if (!initialized || stopped || failed || !command || fgCommand ||
            command->GetType() != D3D12_COMMAND_LIST_TYPE_COMPUTE || !link.registerFgCommand(command) ||
            !fgLifetime.attach(command))
            return false;
        fgCommand = command;
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
        if (stopped && command == fgCommand)
            fgLifetime.detachLive(command);
        for (auto& producer : producers)
            if (producer.identity() == command)
                producer.detachLive(command);
    }

    // Includes Close and every applicable state setter, including predication.
    void onStateMutation(ID3D12GraphicsCommandList* command) { recording.onMutation(command); }

    // Called immediately AFTER the verified game's depth transition. Multiple
    // matching candidates between phase-1 evaluations make that batch ambiguous.
    bool captureIdentifiedSurface(ID3D12GraphicsCommandList* command, ID3D12Resource* depth,
                                  D3D12_RESOURCE_STATES state)
    {
        collectDestroyed();
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
        for (UINT i = 0; i < count; ++i)
        {
            if (commands[i] == fgCommand)
            {
                if ((fgQueue && fgQueue != queue) || queue->GetDesc().Type != D3D12_COMMAND_LIST_TYPE_COMPUTE)
                    failed = true;
                else if (!fgQueue)
                {
                    fgQueue = queue;
                    fgQueue->AddRef();
                }
            }
            link.submit(queue, commands[i]);
            usesOutput |= outputRecording && commands[i] == fgCommand;
        }
        if (!pool.afterSubmit(queue, count, commands) || !link.healthy())
            failed = true;
        if (usesOutput)
        {
            if ((fgQueue && fgQueue != queue) || queue->GetDesc().Type != D3D12_COMMAND_LIST_TYPE_COMPUTE)
                failed = true;
            else
            {
                if (FAILED(queue->Signal(completion, ++submitted)))
                    failed = true;
                else if (timing && !timer.submitted(fgCommand, queue, completion, submitted))
                    timing = false;
            }
        }
        return !failed;
    }

    // Only native, successful GPU synchronization calls belong here. Exclude
    // this module's own completion signals; they do not establish input order.
    void onSignal(ID3D12CommandQueue* queue, ID3D12Fence* fence, uint64_t value)
    {
        if (failed || !fence || !link.isProducerQueue(queue))
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
        if (!failed && queue == fgQueue)
            link.wait(queue, fence, value);
    }

    PreparedInputs prepare(ID3D12GraphicsCommandList* command, const Inputs& inputs,
                           const D3D12_RESOURCE_STATES (&states)[3], Controls controls)
    {
        collectDestroyed();
        if (!initialized || stopped || failed || command != fgCommand)
            return {};
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

    void nativeFailure() { pass.invalidateHistory(); }
    bool accepting()
    {
        collectDestroyed();
        return initialized && !stopped && !failed;
    }
    void bypass(unsigned index)
    {
        pass.invalidateHistory();
        if (index == 1)
        {
            candidates = 0;
            batchSnapshot = {};
            pool.retire(newest);
        }
    }
    std::optional<GpuTimer::Sample> pollTiming() { return timing ? timer.poll() : std::optional<GpuTimer::Sample> {}; }
    uint64_t renderedDispatches() const { return pass.renderedDispatches(); }
    ID3D12Resource* selection() const { return pass.selection(); }
    ID3D12Resource* failures() { return pass.failures(); }

    void stop()
    {
        stopped = true;
        pass.invalidateHistory();
        pool.retire(newest);
    }

    bool readyToRelease()
    {
        collectDestroyed();
        if (!stopped || failed || outputRecording || !pool.idle())
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
        pass.releaseAfterGpuDrain();
        pool.releaseAfterGpuDrain();
        timer.releaseAfterGpuDrain();
        for (auto& producer : producers)
            producer.forget();
        fgLifetime.forget();
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
        stopped = true;
    }
};
} // namespace GlassFg
