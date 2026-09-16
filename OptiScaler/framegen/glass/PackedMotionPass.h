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
    // Evaluations of the frame that carry no Streamline token: the provider's
    // own driver-level block reads the parameter table there (GLASS_PARAM_CALL
    // shows MotionVectors/Depth reads only on those). They reuse the batch of
    // the frame they follow; the bound stops a stalled token from extending
    // that reuse indefinitely.
    unsigned untaggedReuse = 0;
    std::uint64_t dispatches = 0;
    bool initialized = false, batchReady = false;
    // A substituted input is only meaningful when the compose that writes it is
    // actually queued for the same frame. Two substitutions in a row without a
    // queued compose means the FG would evaluate a texture nothing wrote, so the
    // swap is withheld until a compose is queued again.
    std::uint64_t substitutedFrames = 0, composedFrames = 0;
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
    // Streamline evaluates the same frame generation inputs once per back-buffer
    // command list. The capture hands a packed frame to the first evaluation
    // only, so the second one has to reuse it: otherwise the FG core receives
    // the corrected motion on one evaluation and the engine's original motion on
    // the other, and the frames generated from the second evaluation keep the
    // artifact.
    PackedMotionFrame reuseFrame {};
    ID3D12Resource* reuseMotion = nullptr;
    ID3D12Resource* reuseDepth = nullptr;
    bool reuseValid = false;

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
        untaggedReuse = 0;
        packed = {};
        command = nullptr;
    }
    PreparedInputs prepare(ID3D12GraphicsCommandList* value, const Inputs& inputs,
                           const PackedMotionFrame& objectFrame, const D3D12_RESOURCE_STATES (&states)[3],
                           Controls controls, GpuTimer* timer, bool allowAnyState = false)
    {
        if (!initialized || !value || !inputs.valid() || !controls.active() ||
            (!allowAnyState &&
             (states[0] != D3D12_RESOURCE_STATE_COPY_DEST || states[2] != D3D12_RESOURCE_STATE_COPY_DEST)))
        {
            invalidateHistory();
            return {};
        }
        // Evaluations without a Streamline frame token. The provider's own
        // driver-level block is the one that reads the parameter table (the
        // parameter trace records MotionVectors/Depth reads only there), and it
        // runs after the tagged evaluations of the frame it belongs to. The
        // composed texture still holds that frame's correction, so the batch is
        // extended instead of dropped. The bound stops a stalled token from
        // extending that reuse indefinitely.
        if (inputs.frame == UINT64_MAX)
        {
            if (!batchReady || untaggedReuse >= 4)
            {
                invalidateHistory();
                return {};
            }
            ++untaggedReuse;
            static std::atomic<unsigned> untagged { 0 };
            if (auto* untaggedLog = gpu.logHandle())
            {
                if (untagged.fetch_add(1, std::memory_order_relaxed) < 4)
                {
                    std::fprintf(untaggedLog, "PACKED_SUBSTITUTE untagged index=%u next=%u frame=%u\n", inputs.index,
                                 nextIndex, packed.frame);
                    std::fflush(untaggedLog);
                }
            }
            nextIndex = inputs.index + 1;
            // These evaluations read the table under the provider's own names
            // (the parameter trace records MotionVectors/Depth there), so the
            // swap has to replace those keys, not the DLSSG.* aliases.
            return PreparedInputs::make(inputs.motion, inputs.depth, gpu.motionOutput(), gpu.depthOutput(),
                                        "MotionVectors", "Depth",
                                        controls.packedLayer ? gpu.motionOutput() : nullptr,
                                        controls.packedLayer ? gpu.selectionOutput() : nullptr);
        }
        if (inputs.index == 1)
        {
            // A new engine frame invalidates the reuse pair; a repeat evaluation
            // of the same frame (same motion/depth identities and frame token)
            // keeps it.
            if (objectFrame && reuseValid && objectFrame.fgFrame != reuseFrame.fgFrame)
                reuseValid = false;
            PackedMotionFrame frame = objectFrame;
            if (!frame && reuseValid && inputs.motion == reuseMotion && inputs.depth == reuseDepth &&
                inputs.frame == reuseFrame.fgFrame)
                frame = reuseFrame;
            invalidateHistory();
            if (!frame || inputs.frame == UINT64_MAX || frame.fgFrame != inputs.frame)
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
            const bool wasPending = pendingValid;
            if (substitutedFrames > composedFrames + 1)
            {
                static std::atomic<unsigned> stalled { 0 };
                if (auto* log = gpu.logHandle())
                {
                    if (stalled.fetch_add(1, std::memory_order_relaxed) < 4)
                    {
                        std::fprintf(log, "PACKED_SUBSTITUTE stalled substituted=%llu composed=%llu\n",
                                     static_cast<unsigned long long>(substitutedFrames),
                                     static_cast<unsigned long long>(composedFrames));
                        std::fflush(log);
                    }
                }
                // Without resynchronising the counters the gate stays closed and
                // the correction never resumes.
                substitutedFrames = composedFrames;
                invalidateHistory();
                return {};
            }
            pendingFrame = frame;
            pendingMotion = inputs.motion;
            pendingDepth = inputs.depth;
            pendingMotionState = states[0];
            pendingDepthState = states[2];
            pendingScaleX = inputs.scaleX;
            pendingScaleY = inputs.scaleY;
            pendingControls = controls;
            pendingTimer = timer;
            pendingValid = true;
            // A frame can be prepared more than once (one evaluation per
            // back-buffer list). Only the first prepare of a frame counts as an
            // unqueued substitution.
            if (!wasPending)
                ++substitutedFrames;
            ++dispatches;
            if (!controls.packedSubstitute)
            {
                // Isolation staging: the new GPU work ran, but the FG inputs
                // stay original. Keeps a reset attributable to one of the two.
                invalidateHistory();
                return {};
            }
            batch = inputs;
            packed = frame;
            command = value;
            nextIndex = 2;
            untaggedReuse = 0;
            batchReady = true;
            reuseFrame = frame;
            reuseMotion = inputs.motion;
            reuseDepth = inputs.depth;
            reuseValid = true;
        }
        else if (batchReady && inputs.sameRenderedFrame(batch))
        {
            // The provider evaluates one rendered frame several times: the
            // generated-frame slots and one evaluation per back-buffer list.
            // Every one of them has to read the corrected texture. The first
            // evaluation of the frame already queued the compose, so a repeat
            // only extends the batch. Invalidating it instead sends the engine's
            // own motion vectors to the provider, and the frames generated from
            // that evaluation keep the artifact (observed as
            // "skipped_reused" climbing while index 3 was evaluated three times
            // per frame).
            static std::atomic<unsigned> repeats { 0 };
            if (auto* log = gpu.logHandle())
            {
                if (repeats.fetch_add(1, std::memory_order_relaxed) < 4)
                {
                    std::fprintf(log, "PACKED_SUBSTITUTE repeat index=%u next=%u\n", inputs.index, nextIndex);
                    std::fflush(log);
                }
            }
            nextIndex = inputs.index + 1;
        }
        else
        {
            invalidateHistory();
            return {};
        }
        return PreparedInputs::make(inputs.motion, inputs.depth, gpu.motionOutput(), gpu.depthOutput(), inputs.motionKey,
                                    inputs.depthKey, controls.packedLayer ? gpu.motionOutput() : nullptr,
                                    controls.packedLayer ? gpu.selectionOutput() : nullptr);
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
    ID3D12Fence* composeFence() const { return gpu.composeFenceHandle(); }
    std::uint64_t composeForced() const { return gpu.composeForced(); }
    // Live debug channel: recompile the compose shader without a restart.
    bool reloadShader(const wchar_t* shader, FILE* log) { return gpu.reload(shader, log); }
    // Submit the deferred compose on the FG queue. Called from the host's
    // pre-submit hook after the packed producer wait.
    bool executePending(ID3D12CommandQueue* queue)
    {
        if (!pendingValid)
        {
            // Bounded attribution: without it "no compose was prepared" and
            // "the compose was rejected" look identical in the log.
            static std::atomic<unsigned> missing { 0 };
            if (auto* log = gpu.logHandle())
            {
                if (missing.fetch_add(1, std::memory_order_relaxed) < 8)
                {
                    std::fprintf(log, "PACKED_COMPOSE_SKIP reason=no_pending\n");
                    std::fflush(log);
                }
            }
            return false;
        }
        auto* timer = pendingTimer;
        pendingTimer = nullptr;
        const auto result = gpu.submitCompose(queue, pendingFrame, pendingMotion, pendingDepth, pendingMotionState,
                                              pendingDepthState, pendingScaleX, pendingScaleY, pendingControls, timer);
        pendingValid = false;
        if (result)
            ++composedFrames;
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
