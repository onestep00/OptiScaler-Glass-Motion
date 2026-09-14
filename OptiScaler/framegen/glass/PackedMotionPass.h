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
            const auto ticket = timer && controls.measureGpuTime && dispatches % 30 == 0 ? timer->begin(value)
                                                                                         : GpuTimer::Ticket {};
            // Staged activation: the copy+swap contract is exercised first.
            // The full-screen packed compute dispatch is the new GPU work that
            // correlated with driver TDRs on 2026-09-14, so it stays disabled
            // until the copy path proves stable and the dispatch is reviewed.
            const bool packedDispatchEnabled = controls.packedDispatch;
            if (packedDispatchEnabled &&
                !gpu.dispatch(value, objectFrame, inputs.motion, inputs.depth, states[0], states[2],
                              inputs.scaleX, inputs.scaleY, controls))
                return {};
            if (timer && ticket)
                timer->end(value, ticket);
            ++dispatches;
            batch = inputs;
            packed = objectFrame;
            command = value;
            nextIndex = 2;
            batchReady = true;
            if (!packedDispatchEnabled)
            {
                // No dispatch means the owned outputs are unwritten, so never
                // substitute them; this stage only validates the boundary
                // bookkeeping and frame recycling with zero new GPU work.
                invalidateHistory();
                return {};
            }
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
    // Live debug channel: recompile the compose shader without a restart.
    bool reloadShader(const wchar_t* shader, FILE* log) { return gpu.reload(shader, log); }
    ID3D12Resource* selection() const { return gpu.selectionOutput(); }
    void releaseAfterGpuDrain()
    {
        gpu.releaseAfterGpuDrain();
        initialized = false;
        invalidateHistory();
    }
};
} // namespace GlassFg
