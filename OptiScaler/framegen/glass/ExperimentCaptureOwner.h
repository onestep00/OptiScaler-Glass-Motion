#pragma once
#include "ExperimentCaptureAbi.h"
#include "ExperimentDrawBridge.h"
#include "ExperimentRecording.h"
#include "GeometryCommands.h"
#include "CommandLifetime.h"
#include <mutex>
#include <optional>
#include <cstring>

namespace GlassFg
{
// Process-resident adapter. Install once, after construction on a control thread.
// Its fixed admission pool is diagnostic infrastructure, not complete world-
// transparency admission. No allocation/compilation/copy of game buffers occurs
// per admitted draw. First observation of a command registers a lifetime token.
class ExperimentCaptureOwner final : public GeometryDrawCaptureOwner
{
    static constexpr unsigned Capacity = 256, FrameCount = 8, QueueCount = 8;
    struct FrameSlot { ExperimentRuntime::Frame module; uint64_t id = 0; unsigned users = 0; };
    struct Tracker { CommandLifetime lifetime; unsigned users = 0; };
    struct Job
    {
        enum Phase { Empty, Reserved, Recorded, Collecting } phase = Empty;
        uint64_t id = 0, epoch = 0;
        unsigned frame = 0, tracker = 0;
        ID3D12GraphicsCommandList* command = nullptr;
        ExperimentPipelineLease pipeline;
        std::optional<ExperimentRecording> lifetime;
        DWORD thread = 0;
        bool recorded = false, discarded = false;
    };
    struct Queue
    {
        Microsoft::WRL::ComPtr<ID3D12CommandQueue> identity;
        Microsoft::WRL::ComPtr<ID3D12Fence> completion;
        UINT64 value = 0;
    };
    ExperimentRuntime& runtime;
    Microsoft::WRL::ComPtr<ID3D12Device> device;
    std::mutex mutex, callbacks;
    std::array<FrameSlot, FrameCount> frames;
    std::array<Tracker, Capacity> trackers;
    std::array<Job, Capacity> jobs;
    std::array<Queue, QueueCount> queues;
    bool accepting = true;
    uint64_t latestFrame = 0, nextJob = 0;
    DWORD controlThread = GetCurrentThreadId();

    int32_t dispatch(Job& job, GlassExperimentCaptureInput& input)
    {
        auto& frame = frames[job.frame];
        const GlassExperimentEvent event { sizeof(event), GlassExperimentCapture, frame.id, 0, 0,
                                           1, sizeof(input), &input };
        return frame.module.dispatch(event);
    }
  public:
    ExperimentCaptureOwner(ExperimentRuntime& value, ID3D12Device* d) : runtime(value), device(d)
    {
        if (!d) throw std::runtime_error("Capture experiment requires a device");
        for (auto& queue : queues)
            if (FAILED(d->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&queue.completion))))
                throw std::runtime_error("Capture completion fence creation failed");
    }
    ~ExperimentCaptureOwner()
    {
        // Registered adapters are process-resident. Explicit teardown without
        // retirement proof retains original pipelines as well as module leases.
        for (auto& job : jobs)
            if (job.phase != Job::Empty && job.pipeline)
                (void)new ExperimentPipelineLease(std::move(job.pipeline));
    }
    bool prepare(ID3D12GraphicsCommandList* command, const GeometryDrawView& draw,
                 const GeometryIndexedArguments& args, const ExperimentPipelineLease& pipeline,
                 const GraphicsRootBindings& bindings, GeometryPreparedDraw& output) noexcept override
    {
        // Keep callback ownership until finish on this same original Draw call.
        // Other render/control callbacks skip instead of blocking this thread.
        std::unique_lock call(callbacks, std::try_to_lock);
        std::unique_lock lock(mutex, std::try_to_lock);
        if (!call || !lock || !accepting || !draw.frame || !pipeline) return false;
        const auto epoch = ReadGeometryRecordingEpoch(command);
        const auto* raster = ReadGeometryRasterState(command);
        if (!epoch || !raster || !raster->usable()) return false;
        unsigned frameIndex = FrameCount, jobIndex = Capacity, trackerIndex = Capacity;
        for (unsigned i = 0; i < FrameCount; ++i)
            if (frames[i].id == draw.frame && frames[i].module) { frameIndex = i; break; }
        if (frameIndex == FrameCount)
            for (unsigned i = 0; i < FrameCount; ++i)
                if (!frames[i].users)
                {
                    frames[i].module = runtime.beginFrame(draw.frame, 1);
                    frames[i].id = draw.frame;
                    frameIndex = i;
                    break;
                }
        if (frameIndex == FrameCount || !frames[frameIndex].module) return false;
        latestFrame = draw.frame;
        for (unsigned i = 0; i < Capacity; ++i)
        {
            if (jobIndex == Capacity && jobs[i].phase == Job::Empty) jobIndex = i;
            if (trackers[i].lifetime.identity() == command && !trackers[i].lifetime.wasDestroyed()) trackerIndex = i;
        }
        if (jobIndex == Capacity || nextJob == UINT64_MAX) return false;
        if (trackerIndex == Capacity)
            for (unsigned i = 0; i < Capacity; ++i)
                if (!trackers[i].users && (!trackers[i].lifetime.identity() || trackers[i].lifetime.wasDestroyed()))
                {
                    trackers[i].lifetime.takeDestroyed();
                    if (!trackers[i].lifetime.attach(command)) return false;
                    trackerIndex = i;
                    break;
                }
        if (trackerIndex == Capacity) return false;
        auto& job = jobs[jobIndex];
        job.id = ++nextJob; job.epoch = epoch; job.command = command;
        job.frame = frameIndex; job.tracker = trackerIndex;
        const auto input = MakeExperimentDrawInput(command, epoch, draw, args, *raster, bindings, pipeline);
        GlassExperimentPreparedCapture prepared {}; prepared.size = sizeof(prepared);
        GlassExperimentCaptureInput request { sizeof(request), GlassCapturePrepare, job.id, epoch,
                                              command, &input, &prepared, 0, 0 };
        if (dispatch(job, request) != 1) return false;
        job.phase = Job::Reserved; job.thread = GetCurrentThreadId();
        job.pipeline = pipeline; job.discarded = job.recorded = false;
        ++frames[frameIndex].users; ++trackers[trackerIndex].users;
        job.lifetime.emplace(frames[frameIndex].module.retain(), command, epoch);
        output = {};
        if (prepared.size == sizeof(prepared) && !prepared.reserved)
        {
            output.pipeline = static_cast<ID3D12PipelineState*>(prepared.pipeline);
            static_assert(sizeof(output.history) == sizeof(prepared.history));
            memcpy(&output.history, prepared.history, sizeof(output.history));
            output.previous = prepared.previous; output.current = prepared.current;
            output.material = prepared.material; output.capture = prepared.capture; output.mapping = prepared.mapping;
        }
        call.release(); // finish adopts this lock; original API keeps the thread.
        return true;
    }
    void finish(ID3D12GraphicsCommandList* command, bool recorded) noexcept override
    {
        Job* selected = nullptr;
        {
            std::lock_guard lock(mutex);
            for (auto& job : jobs)
                if (job.phase == Job::Reserved && job.command == command && job.thread == GetCurrentThreadId())
                { selected = &job; break; }
        }
        if (!selected) return;
        std::lock_guard call(callbacks, std::adopt_lock);
        GlassExperimentCaptureInput input { sizeof(input), GlassCaptureRecorded, selected->id, selected->epoch,
                                            command, nullptr, nullptr, recorded ? 1u : 0u, 0 };
        const auto result = dispatch(*selected, input); // No owner mutex across GPU calls.
        std::lock_guard lock(mutex);
        selected->recorded = recorded;
        if (result < 0) selected->lifetime->submissionUnknown();
        selected->phase = Job::Recorded;
    }
    void submitted(ID3D12CommandQueue* queue, UINT count, ID3D12CommandList* const* commands) noexcept override
    {
        std::lock_guard lock(mutex);
        if (!queue || (count && !commands))
        {
            for (auto& job : jobs)
                if (job.phase == Job::Recorded && !job.discarded) job.lifetime->submissionUnknown();
            return;
        }
        std::array<Job*, Capacity> selected {}; unsigned used = 0;
        for (auto& job : jobs)
            if (job.phase == Job::Recorded && !job.discarded)
                for (UINT i = 0; i < count; ++i)
                    if (commands[i] == job.command) { selected[used++] = &job; break; }
        if (!used) return;
        Queue* completion = nullptr;
        for (auto& candidate : queues) if (candidate.identity.Get() == queue) completion = &candidate;
        if (!completion && queue)
        {
            Microsoft::WRL::ComPtr<ID3D12Device> actual;
            if (SUCCEEDED(queue->GetDevice(IID_PPV_ARGS(&actual))) && actual.Get() == device.Get())
                for (auto& candidate : queues) if (!candidate.identity)
                { candidate.identity = queue; completion = &candidate; break; }
        }
        const bool signaled = completion && completion->value < UINT64_MAX - 1 &&
            SUCCEEDED(queue->Signal(completion->completion.Get(), ++completion->value));
        for (unsigned i = 0; i < used; ++i)
            if (signaled) selected[i]->lifetime->submitted(completion->completion.Get(), completion->value);
            else selected[i]->lifetime->submissionUnknown();
    }
    void discarded(ID3D12GraphicsCommandList* command) noexcept override
    {
        std::lock_guard lock(mutex);
        for (auto& job : jobs)
            if (job.phase == Job::Recorded && job.command == command && !job.discarded)
            { job.lifetime->discard(command, job.epoch); job.discarded = true; }
    }
    void stop()
    {
        if (GetCurrentThreadId() != controlThread) throw std::runtime_error("Capture stop requires control thread");
        std::lock_guard lock(mutex); accepting = false;
    }
    unsigned collect()
    {
        if (GetCurrentThreadId() != controlThread) throw std::runtime_error("Capture collect requires control thread");
        std::unique_lock call(callbacks, std::try_to_lock);
        if (!call) return 0;
        unsigned retired = 0;
        for (auto& job : jobs)
        {
            ExperimentPipelineLease releaseOutsideLock;
            {
                std::lock_guard lock(mutex);
                if (job.phase != Job::Recorded) continue;
                if (!job.discarded && trackers[job.tracker].lifetime.wasDestroyed())
                { job.lifetime->discard(job.command, job.epoch); job.discarded = true; }
                if (!job.lifetime->retire()) continue;
                job.phase = Job::Collecting;
            }
            GlassExperimentCaptureInput input { sizeof(input), GlassCaptureRetired, job.id, job.epoch,
                                                nullptr, nullptr, nullptr, job.recorded ? 1u : 0u, 0 };
            dispatch(job, input);
            {
                std::lock_guard lock(mutex);
                --frames[job.frame].users; --trackers[job.tracker].users;
                releaseOutsideLock = std::move(job.pipeline);
                job.lifetime.reset(); job.phase = Job::Empty; ++retired;
            }
        }
        std::lock_guard lock(mutex);
        for (auto& frame : frames)
            if (!frame.users && (!accepting || frame.id != latestFrame)) { frame.module = {}; frame.id = 0; }
        return retired;
    }
};
} // namespace GlassFg
