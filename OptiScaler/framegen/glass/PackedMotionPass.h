#pragma once
#include "GlassFgPass.h"
#include "PackedMotionGpu.h"

namespace GlassFg
{
class PackedMotionPass
{
    PackedMotionGpu gpu;
    Inputs batch {};
    PackedMotionFrame packed {};
    ID3D12GraphicsCommandList* command = nullptr;
    unsigned nextIndex = 0;
    std::uint64_t dispatches = 0;
    bool initialized = false, batchReady = false;
    // Deferred compose: prepared inside the FG call, submitted on our own list
    // right before the batch that carries the FG command.
    PackedMotionFrame pendingFrame {};
    ID3D12Resource* pendingMotion = nullptr;
    ID3D12Resource* pendingDepth = nullptr;
    D3D12_RESOURCE_STATES pendingMotionState = D3D12_RESOURCE_STATE_COMMON;
    D3D12_RESOURCE_STATES pendingDepthState = D3D12_RESOURCE_STATE_COMMON;
    float pendingScaleX = 1.f, pendingScaleY = 1.f;
    Controls pendingControls {};
    bool pendingValid = false;
    // Host timer for the deferred compose sample; never owned here.
    GpuTimer* pendingTimer = nullptr;

  public:
    bool initialize(ID3D12Device* device, const D3D12_RESOURCE_DESC (&descriptions)[3],
                    const wchar_t* shader, FILE* log)
    {
        if (initialized || !device || !shader || !log)
            return false;
        initialized = gpu.initialize(device, descriptions[0], descriptions[2], shader, log);
        return initialized;
    }
    void invalidateHistory()
    {
        batchReady = false;
        nextIndex = 0;
        packed = {};
        command = nullptr;
    }
    PreparedInputs prepare(ID3D12GraphicsCommandList* value, const Inputs& inputs,
                           const PackedMotionFrame& objectFrame, const D3D12_RESOURCE_STATES (&states)[3],
                           Controls controls, GpuTimer* timer)
    {
        if (!initialized || !value || !inputs.valid() || !controls.active() ||
            states[0] != D3D12_RESOURCE_STATE_COPY_DEST || states[2] != D3D12_RESOURCE_STATE_COPY_DEST)
        {
            invalidateHistory();
            return {};
        }
        if (inputs.index == 1)
        {
            invalidateHistory();
            if (!objectFrame || inputs.frame == UINT64_MAX || objectFrame.fgFrame != inputs.frame)
                return {};
            // Staged activation: the copy+swap contract is exercised first.
            // The full-screen packed compute dispatch is the new GPU work that
            // correlated with driver TDRs on 2026-09-14, so it stays disabled
            // until the copy path proves stable and the dispatch is reviewed.
            const bool packedDispatchEnabled = controls.packedDispatch;
            if (!packedDispatchEnabled || !gpu.composeReady())
            {
                // Nothing is recorded on the engine's command list: the compose
                // runs on our own list just before the FG batch is submitted.
                invalidateHistory();
                return {};
            }
            pendingFrame = objectFrame;
            pendingMotion = inputs.motion;
            pendingDepth = inputs.depth;
            pendingMotionState = states[0];
            pendingDepthState = states[2];
            pendingScaleX = inputs.scaleX;
            pendingScaleY = inputs.scaleY;
            pendingControls = controls;
            pendingTimer = timer;
            pendingValid = true;
            ++dispatches;
            if (!controls.packedSubstitute)
            {
                // Isolation staging: the new GPU work ran, but the FG inputs
                // stay original. Keeps a reset attributable to one of the two.
                invalidateHistory();
                return {};
            }
            batch = inputs;
            packed = objectFrame;
            command = value;
            nextIndex = 2;
            batchReady = true;
        }
        else if (batchReady && value == command && inputs.index == nextIndex && inputs.index <= inputs.count &&
                 inputs.sameRenderedFrame(batch))
            ++nextIndex;
        else
        {
            invalidateHistory();
            return {};
        }
        return { inputs.motion, inputs.depth, gpu.motionOutput(), gpu.depthOutput() };
    }
    std::uint64_t renderedDispatches() const { return dispatches; }
    // Session teardown: a compose prepared for a retired FG command must not be
    // submitted afterwards.
    void cancelPending()
    {
        pendingValid = false;
        pendingTimer = nullptr;
    }
    // Release gate used by the host before the session resources are freed.
    bool drained() { return gpu.drained(); }
    bool composeInFlight() const { return gpu.composeInFlight(); }
    std::uint64_t composeSubmitted() const { return gpu.composeSubmitted(); }
    std::uint64_t composeCompleted() const { return gpu.composeCompleted(); }
    std::uint64_t composeForced() const { return gpu.composeForced(); }
    // Live debug channel: recompile the compose shader without a restart.
    bool reloadShader(const wchar_t* shader, FILE* log) { return gpu.reload(shader, log); }
    // Submit the deferred compose on the FG queue. Called from the host's
    // pre-submit hook after the packed producer wait.
    bool executePending(ID3D12CommandQueue* queue)
    {
        if (!pendingValid)
            return false;
        auto* timer = pendingTimer;
        pendingTimer = nullptr;
        const auto result = gpu.submitCompose(queue, pendingFrame, pendingMotion, pendingDepth, pendingMotionState,
                                              pendingDepthState, pendingScaleX, pendingScaleY, pendingControls, timer);
        pendingValid = false;
        return result;
    }
    // Live diagnostics: one-frame motion/depth dump and submit correlation.
    void requestDump() { gpu.requestDump(); }
    bool serviceDump() { return gpu.serviceDump(); }
    void dumpSubmitted(ID3D12CommandQueue* queue) { gpu.dumpSubmitted(queue); }
    ID3D12Resource* selection() const { return gpu.selectionOutput(); }
    void releaseAfterGpuDrain()
    {
        gpu.releaseAfterGpuDrain();
        initialized = false;
        invalidateHistory();
    }
};
} // namespace GlassFg
