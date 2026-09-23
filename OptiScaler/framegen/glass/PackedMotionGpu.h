#include "PackedRecordDump.h"
#pragma once
#include "GlassControls.h"
#include "GlassGpuTimer.h"
#include "GlassHostTiming.h"
#include "GlassTrace.h"
#include "MotionDumpFormat.h"
#include "PackedMotionCapture.h"
#include <d3d12.h>
#include <d3dcompiler.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <string>
#include <vector>

namespace GlassFg
{
// One bounded line when a driver call inside the compose submission blocks long
// enough to be felt as a hitch. The budget is fixed so a stall that repeats
// every frame cannot flood the log, and the line carries the wall clock so it
// can be placed against the Windows event log.
inline void NoteSlowComposePhase(FILE* log, const char* phase, double milliseconds, unsigned frame,
                                 unsigned rows) noexcept
{
    constexpr double Threshold = 50.0;
    if (log == nullptr || milliseconds < Threshold)
        return;
    static std::atomic<unsigned> written { 0 };
    if (written.fetch_add(1, std::memory_order_relaxed) >= 32)
        return;
    std::fprintf(log, "GLASS_STALL phase=%s ms=%.3f frame=%u rows=%u epoch=%.3f\n", phase, milliseconds, frame,
                 rows, EpochSeconds());
    std::fflush(log);
}

// Wall time of one driver call, added to the host's per-window counters. The
// destructor runs on every return path, so a failing call is attributed too.
class ComposePhaseScope
{
    HostTiming* timing = nullptr;
    FILE* log = nullptr;
    const char* phase = nullptr;
    unsigned frame = 0, rows = 0;
    std::chrono::steady_clock::time_point start {};

  public:
    ComposePhaseScope(HostTiming& value, FILE* file, const char* name, unsigned packedFrame,
                      unsigned packedRows) noexcept
        : timing(&value), log(file), phase(name), frame(packedFrame), rows(packedRows),
          start(std::chrono::steady_clock::now())
    {}
    ~ComposePhaseScope()
    {
        const auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
                                 std::chrono::steady_clock::now() - start)
                                 .count();
        const auto microseconds = static_cast<std::uint64_t>(elapsed > 0 ? elapsed : 0);
        timing->add(microseconds);
        NoteSlowComposePhase(log, phase, double(microseconds) / 1000.0, frame, rows);
    }
    ComposePhaseScope(const ComposePhaseScope&) = delete;
    ComposePhaseScope& operator=(const ComposePhaseScope&) = delete;
};

class PackedMotionGpu
{
    ID3D12Device* device = nullptr;
    ID3D12DescriptorHeap* heap = nullptr;
    ID3D12RootSignature* root = nullptr;
    ID3D12PipelineState* pipeline = nullptr;
    ID3D12Resource* motion = nullptr;
    ID3D12Resource* depth = nullptr;
    ID3D12Resource* selection = nullptr;
    // FG-facing copies of motion/depth. They are written only by copy, so the
    // evaluation never consumes a UAV-written resource (see dispatch()).
    ID3D12Resource* motionRead = nullptr;
    ID3D12Resource* depthRead = nullptr;
    // Diagnostic coverage counters written by the compose shader:
    // [dispatched, packed_id, edge, interior].
    ID3D12Resource* counters = nullptr;
    ID3D12Resource* zeroCounters = nullptr;
    // The guide descriptions the outputs were built from. A second consumer
    // hands over the game's own textures, so the compose may only run when they
    // are the same extent and format; a different pair would make the copy box
    // or the format conversion invalid.
    D3D12_RESOURCE_DESC motionDescription {};
    D3D12_RESOURCE_DESC depthDescription {};
    // The counters are copy destination, then a UAV write by the compose shader,
    // then (only on a dump) a copy source. The state has to follow that order on
    // every driver; a UAV access while the resource is still COPY_DEST is a
    // state mismatch and hung the device in the 2026-09-15 sessions.
    D3D12_RESOURCE_STATES counterState = D3D12_RESOURCE_STATE_COMMON;
    // Actual current states of the compose outputs. The dispatch leaves them in
    // UNORDERED_ACCESS, so a hardcoded COPY_DEST before-state produced a
    // transition whose StateBefore did not match the resource, which is
    // undefined behaviour for every dispatch after the first.
    D3D12_RESOURCE_STATES outputMotionState = D3D12_RESOURCE_STATE_COPY_DEST;
    D3D12_RESOURCE_STATES outputDepthState = D3D12_RESOURCE_STATE_COPY_DEST;
    D3D12_RESOURCE_STATES outputSelectionState = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    unsigned width = 0, height = 0, increment = 0;
    // Diagnostic readback. Nothing is allocated or copied until the live
    // channel asks for a dump, so the correction path pays nothing by default.
    // 0 composed motion, 1 composed depth, 2 coverage counters,
    // 3 original motion, 4 original depth (same-frame comparison),
    // 5 packed object records (engine coverage),
    // 6 the frame's HUD-less colour, so the motion the compose wrote can be
    //   checked against the image it claims to describe. Allocated lazily on
    //   the health thread once a colour resource has been observed.
    ID3D12Resource* readback[7] {};
    UINT64 readbackBytes[7] {};
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT readbackFootprint[7] {};
    // Colour input of the frame the next dump will capture. The compose sets it
    // from the frame generation evaluation; the copy and the write use it only
    // while a dump is pending, so the normal path never touches it.
    ID3D12Resource* dumpColorResource = nullptr;
    D3D12_RESOURCE_STATES dumpColorState = D3D12_RESOURCE_STATE_COMMON;
    D3D12_RESOURCE_DESC dumpColorDescription {};
    bool dumpColorKnown = false;
    bool dumpColorCopied = false;
    // Set when the observed colour cannot be copied with the allocation this
    // session already owns (a description change). The dump then serves the
    // motion records alone with one counted line instead of stalling the live
    // channel on a resource that would have to be replaced under an in-flight
    // copy.
    bool dumpColorAbandoned = false;
    unsigned dumpColorFailures = 0;
    // The engine frame the second consumer (the DLSS-NR seam) composed on its
    // own command list. The frame generation substitution of the same frame
    // reuses that pair instead of composing a second time.
    std::uint64_t inlineFrame = 0;
    bool inlineValid = false;
    unsigned packedCovered = 0;
    // Dump-only aggregate of the captured object motion, in the record's own
    // 1/8 px units. The composed texture value has to equal
    // packedPixels / mvecScale, so the captured pixels and the scale used for
    // the division are both needed to check one against the other.
    std::uint64_t packedMotionPixels = 0, packedMotionSumAbsX = 0, packedMotionSumAbsY = 0;
    unsigned packedMotionMaxAbsX = 0, packedMotionMaxAbsY = 0, packedMotionSaturated = 0;
    float lastScaleX = 0.f, lastScaleY = 0.f;
    // Sub-pixel projection jitter the provider declared for the frame whose
    // object motion the compose is writing, plus the value the previous compose
    // saw. The delivery removes their difference (the capture's projection
    // convention) and the dump records the pair for the frame it captured, so
    // the residual offset can be attributed to a specific term offline.
    float previousJitterX = 0.f, previousJitterY = 0.f;
    // False until one compose has established the predecessor pair. The first
    // delivered frame after a load then uses a zero conversion instead of an
    // unpaired offset.
    bool jitterHistoryValid = false;
    float dumpJitterX = 0.f, dumpJitterY = 0.f, dumpPreviousJitterX = 0.f, dumpPreviousJitterY = 0.f;
    unsigned lastJitterMode = 0;
    float lastJitterGain = 1.f;
    // Effective engine-proximity gate radius of the last compose, 0 when the
    // diagnostic gate is off. The dump header reports it next to the skip
    // count so a capture names the threshold that produced it.
    float lastEngineGatePx = 0.f;
    // Diagnostic depth route of the last compose. The dump header reports it
    // with the depth substitution count so a capture names the delivery it
    // measured instead of the one the current request file would select.
    bool lastDepthKeep = false;
    // Debug mode word the last compose handed the shader (see the Constants
    // block below: dump request, read skip, zero motion, gate off, depth keep,
    // stripe A/B). The dump header reports it so a capture names the diagnostic
    // that produced its frame instead of the state the live channel held at
    // some other time.
    unsigned lastDebugMode = 0;
    ID3D12Fence* dumpFence = nullptr;
    // Compose fence value that covers the readback copies of a pending dump.
    std::uint64_t dumpComposeValue = 0;
    // Context the pending dump's readback copies were recorded into.
    unsigned dumpComposeContext = 0;
    // Same-frame copies of the engine's own motion and depth, so one dump holds
    // both sides of the correction and the pixels can be compared directly.
    // On: verified with the compose running on the frame generation queue,
    // where the transition of the engine's own textures is the same one the
    // main copy already performs every frame. The copy happens only while a
    // dump is pending, never on the normal path.
    static constexpr bool kDumpEngineInputs = true;
    UINT64 dumpValue = 0;
    unsigned dumpSerial = 0, dumpFrame = 0;
    bool dumpPending = false;
    // Object-id side of the written dump, reported in dump-<serial>.txt:
    // whether -packrec.bin was written, and the id table the capture wrote
    // beside it (PackedMotionCapture.h; zero when no capture is installed).
    bool dumpRecordsWritten = false, dumpIdsWritten = false;
    PackedMotionDumpIdSummary dumpIdSummary {};
    // Steady-clock stamp of the compose that carries the pending dump. A dump
    // whose compose never completes is abandoned instead of being retried for
    // the rest of the session.
    std::atomic<std::uint64_t> dumpPendingSinceMs { 0 };
    // Health-thread-only attempt counter for the readback allocation. The
    // buffers are over 100MB, so a request that can never be prepared has to
    // give up instead of allocating on every health tick.
    unsigned dumpPrepareAttempts = 0;
    std::atomic<unsigned> dumpRequests { 0 };
    std::filesystem::path dumpFolder;
    FILE* logFile = nullptr;
    // Out-of-band compose. Recorded on our own compute list and submitted on
    // the FG queue before the batch that carries the FG call, so the engine's
    // command list state (descriptor heaps, root signature, PSO) is never
    // modified and the NGX recording sequence stays untouched.
    // One compose submission context per command list type. The frame
    // generation path alternates between a direct and a compute queue on
    // consecutive submissions of the same frame (observed as
    // NATIVE_HOST fg_queue adopt type=2 / type=0 alternating), and releasing the
    // list, allocator and fence on every switch reallocated GPU objects per
    // frame and reset the fence value the frame generation queue was already
    // waiting on. Both contexts stay alive until the packed outputs are freed.
    struct ComposeContext
    {
        ID3D12CommandAllocator* allocator = nullptr;
        ID3D12GraphicsCommandList* list = nullptr;
        ID3D12Fence* fence = nullptr;
        std::uint64_t value = 0;
        bool pending = false;
        std::chrono::steady_clock::time_point submittedAt {};
    };
    static constexpr unsigned kComposeContexts = 2;
    ComposeContext composeContexts[kComposeContexts];
    // Context of the most recent submission: the host waits on its fence.
    unsigned composeActive = 0;
    // Descriptor views of the packed record buffers, keyed by resource so a
    // descriptor is never rewritten while it can still be read by the GPU.
    static constexpr unsigned kPackedViewCount = 4;
    struct PackedView
    {
        ID3D12Resource* resource = nullptr;
    };
    PackedView packedViews[kPackedViewCount] {};
    unsigned packedViewCount = 0;
    // The compose list is submitted without the host completion signal whenever
    // the input swap is inactive, so the session teardown needs its own proof
    // that the list finished before these outputs are freed. Freeing them while
    // the GPU still executes the compose is the 2026-09-15 reset. Each context
    // carries its own submission time; the escape below is counted per context.
    std::uint64_t composeForcedReleases = 0;

    D3D12_CPU_DESCRIPTOR_HANDLE cpu(unsigned index) const
    {
        auto value = heap->GetCPUDescriptorHandleForHeapStart();
        value.ptr += SIZE_T(index) * increment;
        return value;
    }
    // DIRECT and COMPUTE both carry frame generation submissions, so each gets
    // its own compose list.
    static unsigned composeIndex(D3D12_COMMAND_LIST_TYPE type) noexcept
    {
        return type == D3D12_COMMAND_LIST_TYPE_DIRECT ? 0u : 1u;
    }
    D3D12_GPU_DESCRIPTOR_HANDLE gpu(unsigned index) const
    {
        auto value = heap->GetGPUDescriptorHandleForHeapStart();
        value.ptr += UINT64(index) * increment;
        return value;
    }
    static void transition(ID3D12GraphicsCommandList* command, ID3D12Resource* resource,
                           D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after)
    {
        D3D12_RESOURCE_BARRIER barrier {};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition = { resource, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, before, after };
        command->ResourceBarrier(1, &barrier);
    }
    // Returns the descriptor slot holding this raster, creating it once. The
    // packed records are eight bytes per pixel, so the view is sized from the
    // configured extent instead of relying on a bare address.
    unsigned packedView(ID3D12Resource* resource)
    {
        for (unsigned i = 0; i < packedViewCount; ++i)
            if (packedViews[i].resource == resource)
                return i;
        if (!device || !resource || packedViewCount >= kPackedViewCount)
            return 0;
        D3D12_UNORDERED_ACCESS_VIEW_DESC view {};
        view.Format = DXGI_FORMAT_R32_TYPELESS;
        view.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
        view.Buffer.NumElements = static_cast<UINT>(UINT64(width) * height * 2);
        view.Buffer.Flags = D3D12_BUFFER_UAV_FLAG_RAW;
        device->CreateUnorderedAccessView(resource, nullptr, &view, cpu(packedViewCount));
        packedViews[packedViewCount].resource = resource;
        return packedViewCount++;
    }
    bool createTexture(D3D12_RESOURCE_DESC desc, D3D12_RESOURCE_STATES state, ID3D12Resource** output)
    {
        D3D12_HEAP_PROPERTIES properties {};
        properties.Type = D3D12_HEAP_TYPE_DEFAULT;
        desc.Alignment = 0;
        return SUCCEEDED(device->CreateCommittedResource(&properties, D3D12_HEAP_FLAG_NONE, &desc, state, nullptr,
                                                         IID_PPV_ARGS(output)));
    }
    // HLSL compilation is the most expensive CPU step of a session start, and a
    // session is created again whenever the engine rebuilds its frame generation
    // lists (focus regain, swap-chain change, frame generation restart). The
    // compiled code depends only on the shader source, so it is cached
    // process-wide and the per-session cost is the pipeline state object alone.
    static ID3DBlob* sharedShaderCode(const std::vector<char>& source, FILE* log)
    {
        struct Entry
        {
            std::size_t size = 0;
            std::uint64_t hash = 0;
            ID3DBlob* code = nullptr;
        };
        static constexpr unsigned kSharedShaderCacheSize = 4;
        static std::mutex mutex;
        static Entry entries[kSharedShaderCacheSize];
        static unsigned entryCount = 0;
        std::uint64_t hash = 1469598103934665603ull;
        for (const auto value : source)
        {
            hash ^= static_cast<unsigned char>(value);
            hash *= 1099511628211ull;
        }
        std::lock_guard lock(mutex);
        for (unsigned i = 0; i < entryCount; ++i)
            if (entries[i].size == source.size() && entries[i].hash == hash && entries[i].code != nullptr)
            {
                if (log != nullptr)
                {
                    std::fprintf(log, "OBJECT_SHADER cached=1 bytes=%zu\n", source.size());
                    std::fflush(log);
                }
                entries[i].code->AddRef();
                return entries[i].code;
            }
        ID3DBlob* code = nullptr;
        ID3DBlob* errors = nullptr;
        auto result = D3DCompile(source.data(), source.size(), "glass-object-motion.hlsl", nullptr, nullptr,
                                 "ApplyObjectMotion", "cs_5_0",
                                 D3DCOMPILE_OPTIMIZATION_LEVEL3 | D3DCOMPILE_ENABLE_STRICTNESS, 0, &code, &errors);
        if (errors)
        {
            if (log != nullptr)
            {
                std::fprintf(log, "OBJECT_SHADER %s\n", static_cast<const char*>(errors->GetBufferPointer()));
                std::fflush(log);
            }
            errors->Release();
        }
        if (FAILED(result) || code == nullptr)
            return nullptr;
        if (entryCount == kSharedShaderCacheSize)
        {
            // Bounded: only shader revisions reach this cache, so the oldest is
            // dropped rather than letting it grow.
            entries[0].code->Release();
            for (unsigned i = 1; i < entryCount; ++i)
                entries[i - 1] = entries[i];
            --entryCount;
        }
        entries[entryCount++] = { source.size(), hash, code };
        code->AddRef();
        return code;
    }
    // Shader source read shared by the pipeline build and the background warm-up.
    static bool readShaderSource(const wchar_t* path, std::vector<char>& source)
    {
        FILE* file = _wfopen(path, L"rb");
        if (!file)
            return false;
        std::fseek(file, 0, SEEK_END);
        const auto size = std::ftell(file);
        std::rewind(file);
        source.assign(size > 0 ? static_cast<size_t>(size) : 0, 0);
        const bool read = size > 0 && std::fread(source.data(), 1, source.size(), file) == source.size();
        std::fclose(file);
        return read;
    }
    // Compiles the compose shader and swaps the compute PSO. Safe to call again
    // from the live debug channel; the old PSO stays until the new one exists.
    bool compilePipeline(const wchar_t* shader, FILE* log)
    {
        std::vector<char> source;
        if (!readShaderSource(shader, source))
            return false;
        const auto codeStart = std::chrono::steady_clock::now();
        ID3DBlob* code = sharedShaderCode(source, log);
        const auto codeMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - codeStart).count();
        if (code == nullptr)
            return false;
        D3D12_COMPUTE_PIPELINE_STATE_DESC pipelineDescription {};
        pipelineDescription.pRootSignature = root;
        pipelineDescription.CS = { code->GetBufferPointer(), code->GetBufferSize() };
        ID3D12PipelineState* created = nullptr;
        const auto pipelineStart = std::chrono::steady_clock::now();
        const auto result = device->CreateComputePipelineState(&pipelineDescription, IID_PPV_ARGS(&created));
        const auto pipelineMs =
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - pipelineStart).count();
        code->Release();
        if (log != nullptr)
        {
            // A session starts on the engine's render thread when it rebuilds its
            // frame generation lists (focus regain, frame generation restart), so
            // the two halves are reported apart: the code half must be a process
            // cache hit by then and the pipeline half is what remains.
            std::fprintf(log, "OBJECT_SHADER code_ms=%.2f pso_ms=%.2f\n", codeMs, pipelineMs);
            std::fflush(log);
        }
        if (FAILED(result))
            return false;
        if (pipeline)
            pipeline->Release();
        pipeline = created;
        return true;
    }

  public:
    // Second consumer path. The DLSS-NR pass runs on the game's own command
    // list, before the frame generation batch is submitted, so a compose
    // submitted on the queue would execute ahead of the frame's own draw work
    // and read a packed buffer that is still being written. The same dispatch
    // body is recorded on the caller's list instead; the caller proved that the
    // frame's producer submission already precedes this list.
    bool recordInline(ID3D12GraphicsCommandList* command, const PackedMotionFrame& packed,
                      ID3D12Resource* originalMotion, ID3D12Resource* originalDepth,
                      D3D12_RESOURCE_STATES motionState, D3D12_RESOURCE_STATES depthState, float scaleX,
                      float scaleY, float jitterX, float jitterY, Controls controls)
    {
        if (!command || !composeReady())
        {
            noteComposeSkip("inline");
            return false;
        }
        if (!dispatch(command, packed, originalMotion, originalDepth, motionState, depthState, scaleX, scaleY,
                      jitterX, jitterY, controls))
            return false;
        inlineFrame = packed.frame;
        inlineValid = true;
        return true;
    }
    bool inlineComposed(std::uint64_t frame) const { return inlineValid && inlineFrame == frame; }
    void clearInline() { inlineValid = false; }

  private:
  public:
    PackedMotionGpu() = default;
    PackedMotionGpu(const PackedMotionGpu&) = delete;
    PackedMotionGpu& operator=(const PackedMotionGpu&) = delete;

    // Non-render-thread warm-up. The first frame generation session is created
    // from inside the engine's frame generation callback, which runs on the
    // render thread; the HLSL compile there measured 153ms (2026-09-16). The
    // compiled code depends only on the source, so filling the process-wide
    // cache from the host's background thread leaves only the pipeline state
    // object for the session start.
    static bool warmShaderCode(const wchar_t* shader, FILE* log) noexcept
    {
        try
        {
            std::vector<char> source;
            if (!readShaderSource(shader, source))
                return false;
            ID3DBlob* code = sharedShaderCode(source, log);
            if (code == nullptr)
                return false;
            code->Release();
            return true;
        }
        catch (...)
        {
            return false;
        }
    }

    bool initialize(ID3D12Device* value, const D3D12_RESOURCE_DESC& motionDesc, const D3D12_RESOURCE_DESC& depthDesc,
                    const wchar_t* shader, FILE* log)
    {
        if (device || !value || !shader || !log || motionDesc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D ||
            depthDesc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || !motionDesc.Width ||
            motionDesc.Width > 32768 || !motionDesc.Height || motionDesc.Height > 32768 ||
            motionDesc.Width != depthDesc.Width || motionDesc.Height != depthDesc.Height ||
            motionDesc.Format != DXGI_FORMAT_R16G16B16A16_FLOAT ||
            (depthDesc.Format != DXGI_FORMAT_R32_FLOAT && depthDesc.Format != DXGI_FORMAT_R32_TYPELESS))
            return false;
        device = value;
        this->logFile = log;
        this->motionDescription = motionDesc;
        this->depthDescription = depthDesc;
        const auto createStart = std::chrono::steady_clock::now();
        dumpFolder = std::filesystem::path(shader).parent_path();
        width = static_cast<unsigned>(motionDesc.Width);
        height = motionDesc.Height;
        // Size/format evidence for crash attribution: a copy box larger than a
        // bound texture or a format mismatch is the first suspect when a driver
        // reset follows the first full-height frame.
        if (logFile)
        {
            std::fprintf(logFile,
                         "PACKED_GPU extent=%ux%u motion_format=%u depth_format=%u depth_flags=%u\n",
                         width, height, static_cast<unsigned>(motionDescription.Format),
                         static_cast<unsigned>(depthDescription.Format),
                         static_cast<unsigned>(depthDescription.Flags));
            std::fflush(logFile);
        }

        auto outputMotion = motionDescription;
        outputMotion.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        if (!createTexture(outputMotion, D3D12_RESOURCE_STATE_COPY_DEST, &motion))
            return false;
        auto outputDepth = depthDescription;
        outputDepth.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        if (!createTexture(outputDepth, D3D12_RESOURCE_STATE_COPY_DEST, &depth))
            return false;
        // The FG evaluation consumes these two. They are only ever written by a
        // copy, never by the compose UAV: handing UAV-written resources to the
        // evaluation reset the driver on 2026-09-14 23:17, while the copy-only
        // pair was stable in the 22:57 session.
        if (!createTexture(motionDescription, D3D12_RESOURCE_STATE_COPY_DEST, &motionRead) ||
            !createTexture(depthDescription, D3D12_RESOURCE_STATE_COPY_DEST, &depthRead))
            return false;
        auto selected = depthDescription;
        selected.Format = DXGI_FORMAT_R16_FLOAT;
        selected.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        if (!createTexture(selected, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, &selection))
            return false;

        D3D12_HEAP_PROPERTIES counterHeap {};
        counterHeap.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC counterDescription {};
        counterDescription.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        counterDescription.Width = 64;
        counterDescription.Height = 1;
        counterDescription.DepthOrArraySize = 1;
        counterDescription.MipLevels = 1;
        counterDescription.SampleDesc.Count = 1;
        counterDescription.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        counterDescription.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        if (FAILED(device->CreateCommittedResource(&counterHeap, D3D12_HEAP_FLAG_NONE, &counterDescription,
                                                   D3D12_RESOURCE_STATE_COMMON, nullptr,
                                                   IID_PPV_ARGS(&counters))))
            return false;
        counterDescription.Flags = D3D12_RESOURCE_FLAG_NONE;
        D3D12_HEAP_PROPERTIES uploadCounterHeap {};
        uploadCounterHeap.Type = D3D12_HEAP_TYPE_UPLOAD;
        if (FAILED(device->CreateCommittedResource(&uploadCounterHeap, D3D12_HEAP_FLAG_NONE, &counterDescription,
                                                   D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                                   IID_PPV_ARGS(&zeroCounters))))
            return false;
        void* zeros = nullptr;
        if (SUCCEEDED(zeroCounters->Map(0, nullptr, &zeros)) && zeros)
        {
            std::memset(zeros, 0, 64);
            zeroCounters->Unmap(0, nullptr);
        }

        D3D12_DESCRIPTOR_HEAP_DESC heapDescription {};
        heapDescription.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        // 0..3 = packed object records (one view per capture frame slot, never
        // rewritten while an earlier compose may still read it),
        // 4..7 = motion/depth/selection/counters.
        heapDescription.NumDescriptors = 8;
        heapDescription.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        if (FAILED(device->CreateDescriptorHeap(&heapDescription, IID_PPV_ARGS(&heap))))
            return false;
        increment = device->GetDescriptorHandleIncrementSize(heapDescription.Type);
        ID3D12Resource* outputs[] { motion, depth, selection };
        DXGI_FORMAT formats[] { DXGI_FORMAT_R16G16B16A16_FLOAT, DXGI_FORMAT_R32_FLOAT, DXGI_FORMAT_R16_FLOAT };
        for (unsigned i = 0; i < 3; ++i)
        {
            D3D12_UNORDERED_ACCESS_VIEW_DESC view {};
            view.Format = formats[i];
            view.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
            device->CreateUnorderedAccessView(outputs[i], nullptr, &view, cpu(i + kPackedViewCount));
        }
        D3D12_UNORDERED_ACCESS_VIEW_DESC counterView {};
        counterView.Format = DXGI_FORMAT_R32_TYPELESS;
        counterView.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
        counterView.Buffer.NumElements = 16;
        counterView.Buffer.Flags = D3D12_BUFFER_UAV_FLAG_RAW;
        device->CreateUnorderedAccessView(counters, nullptr, &counterView, cpu(3 + kPackedViewCount));

        // The packed object records are bound through a descriptor rather than a
        // root UAV. A root UAV is a bare GPU virtual address with no size; the
        // per-pixel read through it reset the GPU on this driver while the same
        // read pattern inside a small window stayed stable, so the view is
        // created explicitly (with NumElements) for every submit.
        D3D12_DESCRIPTOR_RANGE packedRange {};
        packedRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
        packedRange.BaseShaderRegister = 0;
        packedRange.OffsetInDescriptorsFromTableStart = 0;
        packedRange.NumDescriptors = 1;
        D3D12_DESCRIPTOR_RANGE range {};
        range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
        range.BaseShaderRegister = 1;
        range.NumDescriptors = 4;
        D3D12_ROOT_PARAMETER parameters[3] {};
        parameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        parameters[0].DescriptorTable = { 1, &packedRange };
        parameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        parameters[1].DescriptorTable = { 1, &range };
        parameters[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        parameters[2].Constants = { 0, 0, 14 };
        D3D12_ROOT_SIGNATURE_DESC rootDescription { 3, parameters, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_NONE };
        ID3DBlob* serialized = nullptr;
        ID3DBlob* errors = nullptr;
        auto result = D3D12SerializeRootSignature(&rootDescription, D3D_ROOT_SIGNATURE_VERSION_1, &serialized, &errors);
        if (errors)
        {
            std::fprintf(log, "OBJECT_ROOT %s\n", static_cast<const char*>(errors->GetBufferPointer()));
            errors->Release();
        }
        if (FAILED(result))
            return false;
        result = device->CreateRootSignature(0, serialized->GetBufferPointer(), serialized->GetBufferSize(),
                                             IID_PPV_ARGS(&root));
        serialized->Release();
        if (FAILED(result))
            return false;

        if (!compilePipeline(shader, log))
            return false;
        const auto createMs =
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - createStart).count();
        std::fprintf(log, "OBJECT_MOTION_GPU_READY %ux%u passes=1 edge_samples_max=32 create_ms=%.1f\n", width, height,
                     createMs);
        std::fflush(log);
        return true;
    }

    bool reload(const wchar_t* shader, FILE* log)
    {
        if (!device || !root || !shader || !log)
            return false;
        if (!compilePipeline(shader, log))
            return false;
        std::fprintf(log, "OBJECT_MOTION_GPU_RELOADED %ux%u\n", width, height);
        std::fflush(log);
        return true;
    }

    // Live channel: dump one frame's composed motion and depth as PPM images
    // plus a text sample grid. Diagnostic only: one copy of each texture and a
    // blocking map on the health thread, nothing on the normal path.
    void requestDump() noexcept
    {
        // This runs on the frame generation evaluation thread, so it only
        // counts the request. The readback buffers are allocated by serviceDump
        // on the health thread: the same allocation measured 30.1ms when it ran
        // inside the compose record of the 08:38 session.
        dumpRequests.fetch_add(1, std::memory_order_release);
    }

    static std::uint64_t steadyNowMs() noexcept
    {
        return static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch())
                .count());
    }

    bool readbackReady() const noexcept
    {
        bool ready = readback[0] != nullptr && readback[1] != nullptr && readback[2] != nullptr &&
                     readback[5] != nullptr && dumpFence != nullptr;
        if (kDumpEngineInputs)
            ready = ready && readback[3] != nullptr && readback[4] != nullptr;
        return ready;
    }

    // True while a dump is queued or being written. The frame generation
    // evaluation checks this before publishing its colour input, so the hot
    // path pays one relaxed load and nothing else.
    bool dumpWanted() const noexcept
    {
        return dumpPending || dumpRequests.load(std::memory_order_relaxed) != 0;
    }

    // Object-id table gate for a dump of `frame` (PackedMotionCapture.h). The
    // capture may hold the dump back until it has recorded a whole frame, for a
    // bounded number of composes. Without the hook (offline fixtures, no
    // capture) there is no table and no delay.
    static bool dumpIdsReady(std::uint32_t frame) noexcept
    {
        const auto select = packedMotionDumpSelect.load(std::memory_order_acquire);
        return !select || select(frame);
    }

    void setDumpColor(ID3D12Resource* resource, D3D12_RESOURCE_STATES state) noexcept
    {
        if (!resource)
            return;
        // Back buffers alternate, so the identity changes every frame while the
        // description does not. The readback allocation is keyed on the
        // description, never on the pointer.
        dumpColorResource = resource;
        dumpColorState = state;
        dumpColorDescription = resource->GetDesc();
        dumpColorKnown = true;
    }

    // The colour readback is sized from the resource the frame generation call
    // actually published, so a format or extent change cannot silently corrupt
    // the copy. Allocation happens here, on the health thread, never in a
    // compose record.
    bool prepareColorReadback()
    {
        if (!dumpColorKnown || dumpColorAbandoned)
            return false;
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint {};
        UINT64 bytes = 0;
        device->GetCopyableFootprints(&dumpColorDescription, 0, 1, 0, &footprint, nullptr, nullptr, &bytes);
        if (bytes == 0)
            return false;
        if (readback[6])
        {
            const auto& allocated = readbackFootprint[6].Footprint;
            const bool same = readbackBytes[6] == bytes && allocated.Width == footprint.Footprint.Width &&
                              allocated.Height == footprint.Footprint.Height &&
                              allocated.Format == footprint.Footprint.Format &&
                              allocated.RowPitch == footprint.Footprint.RowPitch;
            if (!same)
            {
                dumpColorAbandoned = true;
                if (logFile)
                {
                    std::fprintf(logFile,
                                 "PACKED_DUMP color skip reason=shape_changed format=%u %ux%u bytes=%llu "
                                 "allocated=%llu\n",
                                 unsigned(dumpColorDescription.Format), unsigned(dumpColorDescription.Width),
                                 unsigned(dumpColorDescription.Height), static_cast<unsigned long long>(bytes),
                                 static_cast<unsigned long long>(readbackBytes[6]));
                    std::fflush(logFile);
                }
            }
            return same;
        }
        D3D12_HEAP_PROPERTIES properties {};
        properties.Type = D3D12_HEAP_TYPE_READBACK;
        D3D12_RESOURCE_DESC bufferDescription {};
        bufferDescription.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        bufferDescription.Width = bytes;
        bufferDescription.Height = 1;
        bufferDescription.DepthOrArraySize = 1;
        bufferDescription.MipLevels = 1;
        bufferDescription.SampleDesc.Count = 1;
        bufferDescription.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        ID3D12Resource* created = nullptr;
        if (FAILED(device->CreateCommittedResource(&properties, D3D12_HEAP_FLAG_NONE, &bufferDescription,
                                                   D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&created))))
            return false;
        readback[6] = created;
        readbackBytes[6] = bytes;
        readbackFootprint[6] = footprint;
        return true;
    }

    void dumpSubmitted(ID3D12CommandQueue* queue) noexcept
    {
        if (!dumpPending || !queue || !dumpFence)
            return;
        if (SUCCEEDED(queue->Signal(dumpFence, ++dumpValue)) && logFile)
        {
            std::fprintf(logFile, "PACKED_DUMP submitted serial=%u frame=%u value=%llu\n", dumpSerial, dumpFrame,
                         static_cast<unsigned long long>(dumpValue));
            std::fflush(logFile);
        }
    }

    bool serviceDump() noexcept
    {
        if (!dumpPending)
        {
            // A counted request is prepared here, on the health thread that owns
            // this call, and only then becomes consumable by the next compose
            // record. The render thread never creates a committed resource.
            // The colour buffer is part of the dump when the frame generation
            // evaluation has named one, so the request waits for it here rather
            // than producing a frame whose motion cannot be checked against the
            // image it describes.
            const bool colorReady = !dumpColorKnown || dumpColorAbandoned || readback[6] != nullptr;
            if (dumpRequests.load(std::memory_order_acquire) != 0 && (!readbackReady() || !colorReady))
            {
                if (dumpPrepareAttempts < 4)
                {
                    ++dumpPrepareAttempts;
                    if (!readbackReady())
                        prepareReadback();
                    if (!colorReady)
                    {
                        if (!prepareColorReadback())
                            ++dumpColorFailures;
                        if (dumpColorFailures >= 4)
                        {
                            // Bounded: the colour cannot be copied in this
                            // session, so the dump is served with the motion,
                            // depth and packed records and the reason is left
                            // in the log. Nothing else waits for it.
                            dumpColorAbandoned = true;
                            if (logFile)
                            {
                                std::fprintf(logFile, "PACKED_DUMP color disabled reason=unavailable attempts=%u\n",
                                             dumpColorFailures);
                                std::fflush(logFile);
                            }
                        }
                    }
                    if (readbackReady() && (!dumpColorKnown || dumpColorAbandoned || readback[6]))
                        dumpPrepareAttempts = 0;
                }
                else
                {
                    // The request cannot be served in this session. Drop it so
                    // the live channel stops waiting and the frame path stops
                    // sizing its copies for a dump that will never be read.
                    dumpRequests.store(0, std::memory_order_release);
                    dumpPrepareAttempts = 0;
                    static std::atomic<unsigned> unavailable { 0 };
                    if (logFile && unavailable.fetch_add(1, std::memory_order_relaxed) < 4)
                    {
                        std::fprintf(logFile, "PACKED_DUMP aborted reason=readback_unavailable\n");
                        std::fflush(logFile);
                    }
                }
            }
            return false;
        }
        // A pending dump that no fence ever reports must not stay pending
        // forever: the live channel would wait for a frame it will never read,
        // and every later compose would keep recording its readback copies.
        // 2026-09-16: a dump prepared on an entry that retired before this call
        // saw it produced exactly that silent stall, so the deadline is checked
        // before any fence reasoning.
        {
            const auto since = dumpPendingSinceMs.load(std::memory_order_acquire);
            if (since != 0 && steadyNowMs() - since > 15000)
            {
                static std::atomic<unsigned> stale { 0 };
                if (logFile && stale.fetch_add(1, std::memory_order_relaxed) < 8)
                {
                    std::fprintf(logFile, "PACKED_DUMP aborted reason=stale serial=%u frame=%u age_ms=%llu\n",
                                 dumpSerial, dumpFrame,
                                 static_cast<unsigned long long>(steadyNowMs() - since));
                    std::fflush(logFile);
                }
                dumpPending = false;
                dumpComposeValue = 0;
                dumpPendingSinceMs.store(0, std::memory_order_release);
                return false;
            }
        }
        if (!readback[0] || !readback[1])
            return false;
        // The readback copies are recorded into the compose list, so the
        // completion proof is the compose fence of the context that carried
        // them; the frame generation queue's own signal does not cover them.
        auto& dumpContext = composeContexts[dumpComposeContext < kComposeContexts ? dumpComposeContext : 0];
        ID3D12Fence* completion = dumpContext.fence != nullptr ? dumpContext.fence : dumpFence;
        if (completion == nullptr)
            return false;
        const auto completed = completion->GetCompletedValue();
        const bool composeDone = completed != UINT64_MAX && (dumpComposeValue == 0 || completed >= dumpComposeValue);
        // The copies are executed by the frame generation queue, which signals
        // dumpFence after them on every present. That signal is an independent
        // completion proof, and it is the one that survives a compose context
        // that was reset or replaced between the recording and this call.
        const bool queueDone = dumpFence != nullptr && dumpValue != 0 &&
                               dumpFence->GetCompletedValue() >= dumpValue;
        if (!composeDone && !queueDone)
        {
            // Abandon a dump whose compose never completes. Keeping it pending
            // records another copy set into every later compose and leaves the
            // live channel waiting for a frame it will never read.
            const auto since = dumpPendingSinceMs.load(std::memory_order_acquire);
            if (since != 0 && steadyNowMs() - since > 10000)
            {
                static std::atomic<unsigned> abandoned { 0 };
                if (logFile && abandoned.fetch_add(1, std::memory_order_relaxed) < 8)
                {
                    std::fprintf(logFile,
                                 "PACKED_DUMP aborted reason=compose_timeout serial=%u frame=%u completed=%llu "
                                 "need=%llu\n",
                                 dumpSerial, dumpFrame, static_cast<unsigned long long>(completed),
                                 static_cast<unsigned long long>(dumpComposeValue));
                    std::fflush(logFile);
                }
                dumpPending = false;
                dumpComposeValue = 0;
                dumpPendingSinceMs.store(0, std::memory_order_release);
                return false;
            }
            // Bounded attribution: separates "our GPU work never completed" from
            // "the health thread never reached this call".
            static std::atomic<unsigned> waits { 0 };
            if (logFile && waits.fetch_add(1, std::memory_order_relaxed) < 6)
            {
                std::fprintf(logFile, "PACKED_DUMP waiting serial=%u frame=%u completed=%llu need=%llu\n", dumpSerial,
                             dumpFrame, static_cast<unsigned long long>(completed),
                             static_cast<unsigned long long>(dumpComposeValue));
                std::fflush(logFile);
            }
            return false;
        }
        const bool color = dumpColorCopied && readback[6] != nullptr;
        void* data[7] {};
        for (unsigned i = 0; i < (color ? 7u : 6u); ++i)
        {
            if (!readback[i])
                continue;
            const auto mapped = readback[i]->Map(0, nullptr, &data[i]);
            if (FAILED(mapped) || !data[i])
            {
                // Silent before 2026-09-16: a failed map cleared the pending
                // dump with no line, so the live channel timed out with no
                // reason attached to the frame it had asked for.
                static std::atomic<unsigned> mapFailures { 0 };
                if (logFile && mapFailures.fetch_add(1, std::memory_order_relaxed) < 8)
                {
                    std::fprintf(logFile, "PACKED_DUMP aborted reason=map_failed index=%u hr=0x%08lx serial=%u frame=%u\n",
                                 i, static_cast<unsigned long>(mapped), dumpSerial, dumpFrame);
                    std::fflush(logFile);
                }
                for (unsigned j = 0; j < i; ++j)
                    if (readback[j])
                        readback[j]->Unmap(0, nullptr);
                dumpPending = false;
                dumpComposeValue = 0;
                dumpPendingSinceMs.store(0, std::memory_order_release);
                return false;
            }
        }
        const auto folder = dumpFolder.empty() ? std::filesystem::path(L"Glass") : dumpFolder;
        std::error_code error;
        std::filesystem::create_directories(folder, error);
        const auto suffix = std::to_wstring(dumpSerial);
        const auto base = (folder / L"dump").wstring() + L"-" + suffix;
        writeMotion(base + L"-mv.ppm", static_cast<const std::byte*>(data[0]));
        writeDepth(base + L"-depth.ppm", static_cast<const std::byte*>(data[1]));
        // Full-precision copy of the same two motion textures. The PPM above
        // cannot separate a corrected pixel from an uncorrected one when the
        // correction is smaller than its 1/512 quantization step.
        const auto motionStride = readbackFootprint[0].Footprint.RowPitch;
        WriteMotion32F((base + L"-mv.f32").c_str(), readbackRow(static_cast<const std::byte*>(data[0]), 0, 0),
                       width, height, motionStride, dumpSerial, dumpFrame);
        dumpRecordsWritten = dumpIdsWritten = false;
        dumpIdSummary = {};
        if (kDumpEngineInputs)
        {
            writeMotion(base + L"-original-mv.ppm", static_cast<const std::byte*>(data[3]));
            writeDepth(base + L"-original-depth.ppm", static_cast<const std::byte*>(data[4]));
            writePacked(base + L"-packed.ppm", static_cast<const std::byte*>(data[5]));
            const bool rawWritten = Glass::WritePackedRecord64((base + L"-packrec.bin").c_str(),
                static_cast<const std::byte*>(data[5]), static_cast<std::size_t>(readbackBytes[5]),
                width, height, dumpSerial, dumpFrame);
            if (logFile)
                std::fprintf(logFile, "PACKED_RAW %s serial=%u frame=%u path=%ls-packrec.bin\n",
                    rawWritten ? "written" : "failed", dumpSerial, dumpFrame, base.c_str());
            dumpRecordsWritten = rawWritten;
            // The boundary-id -> pipeline table of the same frame, so the object
            // ids in -packrec.bin can be grouped by pipeline offline.
            if (const auto writeIds = packedMotionDumpWrite.load(std::memory_order_acquire))
            {
                dumpIdsWritten = writeIds((base + L"-pipelines.txt").c_str(), dumpSerial, dumpFrame, &dumpIdSummary);
                if (logFile)
                    std::fprintf(logFile,
                                 "PACKED_IDS %s serial=%u frame=%u records=%u ids=%u complete=%u deferred=%u "
                                 "path=%ls-pipelines.txt\n",
                                 dumpIdsWritten ? "written" : "failed", dumpSerial, dumpFrame, dumpIdSummary.records,
                                 dumpIdSummary.ids, dumpIdSummary.complete ? 1u : 0u, dumpIdSummary.deferred,
                                 base.c_str());
            }
            WriteMotion32F((base + L"-original-mv.f32").c_str(),
                           readbackRow(static_cast<const std::byte*>(data[3]), 0, 0), width, height, motionStride,
                           dumpSerial, dumpFrame);
        }
        writeSamples(base + L".txt", static_cast<const std::byte*>(data[0]), static_cast<const std::byte*>(data[1]),
                     static_cast<const std::byte*>(data[2]), static_cast<const std::byte*>(data[3]),
                     static_cast<const std::byte*>(data[4]));
        if (color)
        {
            // The colour the generated frames have to match. Written as a PPM
            // so the motion images and the scene can be compared without a
            // decoder for the game's own container.
            writeColor(base + L"-color.ppm", static_cast<const std::byte*>(data[6]));
        }
        for (unsigned i = 0; i < 7; ++i)
            if (readback[i])
                readback[i]->Unmap(0, nullptr);
        if (logFile)
        {
            std::fprintf(logFile, "PACKED_DUMP written serial=%u frame=%u color=%u format=%u %ux%u path=%ls\n",
                         dumpSerial, dumpFrame, color ? 1u : 0u, color ? unsigned(dumpColorDescription.Format) : 0u,
                         color ? unsigned(dumpColorDescription.Width) : 0u,
                         color ? unsigned(dumpColorDescription.Height) : 0u, base.c_str());
            std::fflush(logFile);
        }
        dumpColorCopied = false;
        dumpPending = false;
        dumpComposeValue = 0;
        dumpPendingSinceMs.store(0, std::memory_order_release);
        return true;
    }

  public:
    // Diagnostics: the session log the packed pass reports into.
    FILE* logHandle() const { return logFile; }
    bool composeReady()
    {
        bool ready = true;
        for (auto& context : composeContexts)
        {
            if (!context.pending)
                continue;
            if (!context.fence)
            {
                ready = false;
                continue;
            }
            const auto completed = context.fence->GetCompletedValue();
            if (completed == UINT64_MAX)
            {
                ready = false;
                continue;
            }
            if (completed >= context.value)
                context.pending = false;
            else
                ready = false;
        }
        return ready;
    }
    // Diagnostics: raw fence state for the live channel and the offline fixture.
    std::uint64_t composeCompleted() const
    {
        const auto& context = composeContexts[composeActive];
        return context.fence ? context.fence->GetCompletedValue() : 0;
    }
    std::uint64_t composeSubmitted() const { return composeContexts[composeActive].value; }
    bool composeInFlight() const
    {
        for (const auto& context : composeContexts)
            if (context.pending)
                return true;
        return false;
    }
    // The host submits the compose on the queue that owns the packed raster and
    // then makes the frame generation queue wait on this fence, so the packed
    // records are never read across queues.
    ID3D12Fence* composeFenceHandle() const { return composeContexts[composeActive].fence; }
    std::uint64_t composeForced() const { return composeForcedReleases; }
    // Teardown gate: true only when no compose list of this object can still be
    // executing on the GPU. Elapsed time is not completion evidence. Missing
    // signals and device-removal sentinels retain ownership until the host
    // performs explicit device teardown; ordinary retirement must keep waiting.
    bool drained()
    {
        bool complete = true;
        for (auto& context : composeContexts)
        {
            if (!context.pending)
                continue;
            if (context.fence)
            {
                const auto completed = context.fence->GetCompletedValue();
                if (completed != UINT64_MAX && completed >= context.value)
                {
                    context.pending = false;
                    continue;
                }
            }
            complete = false;
        }
        return complete;
    }

    // Records the input copies, the compose dispatch and the optional engine
    // write-back on our own compute list, then submits it on the FG queue. The
    // caller performs the producer wait on that queue before this call, so the
    // engine's command list state and the NGX recording sequence stay clean.
    // Bounded attribution for a compose that could not be queued: the FG then
    // keeps an input nothing wrote, and the reason is otherwise invisible.
    void noteComposeSkip(const char* reason) noexcept
    {
        static std::atomic<unsigned> logged { 0 };
        if (logFile != nullptr && logged.fetch_add(1, std::memory_order_relaxed) < 12)
        {
            std::fprintf(logFile, "PACKED_COMPOSE_SKIP reason=%s width=%u height=%u\n", reason, width, height);
            std::fflush(logFile);
        }
    }

    // Frame generation call phase of the compose being submitted. Diagnostic
    // only: the values are printed next to the jitter pair so one delivered term
    // can be attributed to one generated-frame slot. The default describes an
    // evaluation with no provider frame token, which is what the offline tests
    // and the single-consumer paths drive.
    struct ComposeMark
    {
        unsigned multiFrameIndex = 0, multiFrameCount = 0;
        std::uint64_t fgFrame = UINT64_MAX;
    };

    bool submitCompose(ID3D12CommandQueue* queue, const PackedMotionFrame& packed, ID3D12Resource* originalMotion,
                       ID3D12Resource* originalDepth, D3D12_RESOURCE_STATES motionState,
                       D3D12_RESOURCE_STATES depthState, float scaleX, float scaleY, float jitterX, float jitterY,
                       Controls controls, GpuTimer* timer, ComposeMark mark = {})
    {
        if (!queue)
        {
            noteComposeSkip("queue");
            return false;
        }
        const auto type = queue->GetDesc().Type;
        if (type != D3D12_COMMAND_LIST_TYPE_COMPUTE && type != D3D12_COMMAND_LIST_TYPE_DIRECT)
        {
            noteComposeSkip("queue_type");
            return false;
        }
        if (!composeReady())
        {
            noteComposeSkip("pending");
            return false;
        }
        if (!ensureCompose(type))
        {
            noteComposeSkip("ensure");
            return false;
        }
        const auto index = composeIndex(type);
        auto& context = composeContexts[index];
        {
            const auto rows = (std::min)(height, (std::max)(1u, controls.packedRows));
            ComposePhaseScope phase(ComposeResetTiming(), logFile, "reset", packed.frame, rows);
            if (FAILED(context.allocator->Reset()))
            {
                noteComposeSkip("allocator_reset");
                return false;
            }
            if (FAILED(context.list->Reset(context.allocator, nullptr)))
            {
                noteComposeSkip("list_reset");
                return false;
            }
        }
        // Sparse GPU timing on the host's existing timer. The compose list
        // follows the frame-generation queue type, which is direct for
        // Streamline's DLSS-G evaluation; the timer accepts both. A full
        // eight-slot window skips the sample instead of waiting.
        const auto timing =
            timer && controls.measureGpuTime ? timer->begin(context.list) : GpuTimer::Ticket {};
        const auto rows = (std::min)(height, (std::max)(1u, controls.packedRows));
        {
            ComposePhaseScope phase(ComposeRecordTiming(), logFile, "record", packed.frame, rows);
            const bool dispatched =
                dispatch(context.list, packed, originalMotion, originalDepth, motionState, depthState, scaleX, scaleY,
                         jitterX, jitterY, controls, mark);
            // FG-only policy: the game's motion and depth textures are also read
            // by DLSS Super Resolution, Ray Reconstruction and the ray traced
            // passes, so this path never writes them. The correction reaches the
            // generator only through the DLSS-G parameter substitution.
            const bool recorded = dispatched;
            if (timing)
                timer->end(context.list, timing);
            if (!recorded)
                return false;
            if (FAILED(context.list->Close()))
            {
                noteComposeSkip("close");
                return false;
            }
        }
        ID3D12CommandList* lists[] { context.list };
        {
            ComposePhaseScope phase(ComposeExecuteTiming(), logFile, "execute", packed.frame, rows);
            queue->ExecuteCommandLists(1, lists);
        }
        context.submittedAt = std::chrono::steady_clock::now();
        const auto value = ++context.value;
        // Pending is set before the signal on purpose: a list that was already
        // submitted must never be reclaimed by a later Reset while its
        // completion is unknown.
        context.pending = true;
        {
            ComposePhaseScope phase(ComposeSignalTiming(), logFile, "signal", packed.frame, rows);
            if (FAILED(queue->Signal(context.fence, value)))
                return false;
        }
        composeActive = index;
        if (dumpPending)
        {
            dumpComposeValue = value;
            dumpComposeContext = index;
        }
        if (timing)
            timer->submitted(context.list, queue, context.fence, context.value);
        if (controls.trace && logFile && TraceWanted())
        {
            std::fprintf(logFile, "TRACE_COMPOSE queue=%p frame=%u rows=%u writeback=0\n", queue, packed.frame,
                         (std::min)(height, (std::max)(1u, controls.packedRows)));
            std::fflush(logFile);
        }
        return true;
    }

  private:
    bool ensureCompose(D3D12_COMMAND_LIST_TYPE type)
    {
        auto& context = composeContexts[composeIndex(type)];
        if (context.list)
            return true;
        // Each context is created once and kept for the life of the packed
        // outputs: the frame generation path switches list type per submission,
        // so a type mismatch is not a reason to drop the other context's list,
        // allocator or fence (the frame generation queue may still be waiting on
        // that fence).
        if (!device ||
            FAILED(device->CreateCommandAllocator(type, IID_PPV_ARGS(&context.allocator))) ||
            FAILED(device->CreateCommandList(0, type, context.allocator, nullptr,
                                             IID_PPV_ARGS(&context.list))) ||
            FAILED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&context.fence))))
            return false;
        context.list->Close();
        return true;
    }

    bool prepareReadback()
    {
        if (readbackReady())
            return true;
        ID3D12Resource* targets[] { motion, depth };
        for (unsigned i = 0; i < 2; ++i)
        {
            if (readback[i])
                continue;
            auto description = targets[i]->GetDesc();
            D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint {};
            UINT64 bytes = 0;
            device->GetCopyableFootprints(&description, 0, 1, 0, &footprint, nullptr, nullptr, &bytes);
            D3D12_HEAP_PROPERTIES properties {};
            properties.Type = D3D12_HEAP_TYPE_READBACK;
            D3D12_RESOURCE_DESC bufferDescription {};
            bufferDescription.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
            bufferDescription.Width = bytes;
            bufferDescription.Height = 1;
            bufferDescription.DepthOrArraySize = 1;
            bufferDescription.MipLevels = 1;
            bufferDescription.SampleDesc.Count = 1;
            bufferDescription.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
            ID3D12Resource* created = nullptr;
            if (FAILED(device->CreateCommittedResource(&properties, D3D12_HEAP_FLAG_NONE, &bufferDescription,
                                                       D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&created))))
                return false;
            readback[i] = created;
            readbackBytes[i] = bytes;
            readbackFootprint[i] = footprint;
        }
        if (!dumpFence && FAILED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&dumpFence))))
            return false;
        if (!readback[2])
        {
            D3D12_HEAP_PROPERTIES properties {};
            properties.Type = D3D12_HEAP_TYPE_READBACK;
            D3D12_RESOURCE_DESC bufferDescription {};
            bufferDescription.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
            bufferDescription.Width = 64;
            bufferDescription.Height = 1;
            bufferDescription.DepthOrArraySize = 1;
            bufferDescription.MipLevels = 1;
            bufferDescription.SampleDesc.Count = 1;
            bufferDescription.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
            ID3D12Resource* created = nullptr;
            if (FAILED(device->CreateCommittedResource(&properties, D3D12_HEAP_FLAG_NONE, &bufferDescription,
                                                       D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&created))))
                return false;
            readback[2] = created;
        }
        // The original FG inputs share the owned textures' descriptions, so the
        // same footprint and size serve the comparison readbacks. They are
        // allocated only for the offline before/after comparison; the default
        // dump leaves the engine's own textures untouched.
        for (unsigned i = 3; kDumpEngineInputs && i < 5; ++i)
        {
            if (readback[i])
                continue;
            const auto source = i - 3;
            D3D12_HEAP_PROPERTIES properties {};
            properties.Type = D3D12_HEAP_TYPE_READBACK;
            D3D12_RESOURCE_DESC bufferDescription {};
            bufferDescription.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
            bufferDescription.Width = readbackBytes[source];
            bufferDescription.Height = 1;
            bufferDescription.DepthOrArraySize = 1;
            bufferDescription.MipLevels = 1;
            bufferDescription.SampleDesc.Count = 1;
            bufferDescription.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
            ID3D12Resource* created = nullptr;
            if (FAILED(device->CreateCommittedResource(&properties, D3D12_HEAP_FLAG_NONE, &bufferDescription,
                                                       D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&created))))
                return false;
            readback[i] = created;
            readbackFootprint[i] = readbackFootprint[source];
        }
        if (!readback[5])
        {
            D3D12_HEAP_PROPERTIES properties {};
            properties.Type = D3D12_HEAP_TYPE_READBACK;
            D3D12_RESOURCE_DESC bufferDescription {};
            bufferDescription.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
            bufferDescription.Width = UINT64(width) * height * 8;
            bufferDescription.Height = 1;
            bufferDescription.DepthOrArraySize = 1;
            bufferDescription.MipLevels = 1;
            bufferDescription.SampleDesc.Count = 1;
            bufferDescription.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
            ID3D12Resource* created = nullptr;
            if (FAILED(device->CreateCommittedResource(&properties, D3D12_HEAP_FLAG_NONE, &bufferDescription,
                                                       D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&created))))
                return false;
            readback[5] = created;
            readbackBytes[5] = bufferDescription.Width;
        }
        ++dumpSerial;
        return true;
    }

    const std::byte* readbackRow(const std::byte* data, unsigned index, unsigned y) const
    {
        return data + readbackFootprint[index].Offset + UINT64(y) * readbackFootprint[index].Footprint.RowPitch;
    }

    static std::byte toByte(float value)
    {
        return static_cast<std::byte>(std::clamp(value, 0.f, 255.f));
    }

    static float halfToFloat(unsigned short value)
    {
        const auto sign = unsigned(value & 0x8000u) << 16;
        auto exponent = unsigned((value >> 10) & 0x1fu);
        auto mantissa = unsigned(value & 0x3ffu);
        unsigned bits = 0;
        if (!exponent)
        {
            if (!mantissa)
                bits = sign;
            else
            {
                auto adjusted = 127 - 15 + 1;
                while (!(mantissa & 0x400u))
                {
                    mantissa <<= 1;
                    --adjusted;
                }
                bits = sign | (unsigned(adjusted) << 23) | ((mantissa & 0x3ffu) << 13);
            }
        }
        else if (exponent == 31)
            bits = sign | 0x7f800000u | (mantissa << 13);
        else
            bits = sign | ((exponent + 127 - 15) << 23) | (mantissa << 13);
        float result = 0.f;
        std::memcpy(&result, &bits, sizeof(result));
        return result;
    }

    void writeMotion(const std::wstring& path, const std::byte* data) const
    {
        FILE* file = _wfopen(path.c_str(), L"wb");
        if (!file)
            return;
        std::fprintf(file, "P6\n%u %u\n255\n", width, height);
        for (unsigned y = 0; y < height; ++y)
        {
            const auto* row = reinterpret_cast<const unsigned short*>(readbackRow(data, 0, y));
            for (unsigned x = 0; x < width; ++x)
            {
                const auto mx = halfToFloat(row[x * 4 + 0]);
                const auto my = halfToFloat(row[x * 4 + 1]);
                const std::byte pixel[3] { toByte(128.f + mx * 512.f), toByte(128.f + my * 512.f),
                                           toByte(128.f + std::sqrt(mx * mx + my * my) * 1024.f) };
                std::fwrite(pixel, 1, 3, file);
            }
        }
        std::fclose(file);
    }

    void writeDepth(const std::wstring& path, const std::byte* data) const
    {
        FILE* file = _wfopen(path.c_str(), L"wb");
        if (!file)
            return;
        std::fprintf(file, "P6\n%u %u\n255\n", width, height);
        for (unsigned y = 0; y < height; ++y)
        {
            const auto* row = reinterpret_cast<const float*>(readbackRow(data, 1, y));
            for (unsigned x = 0; x < width; ++x)
            {
                const std::byte pixel[3] { toByte(row[x] * 255.f), toByte(row[x] * 255.f), toByte(row[x] * 255.f) };
                std::fwrite(pixel, 1, 3, file);
            }
        }
        std::fclose(file);
    }

    // The frame generation evaluation's own colour. Its format is the game's,
    // so the writer covers the encodings the Streamline path has published and
    // names the one it could not read instead of writing a file whose channels
    // mean nothing.
    void writeColor(const std::wstring& path, const std::byte* data)
    {
        const auto format = dumpColorDescription.Format;
        WriteColorPpm(path.c_str(), data + readbackFootprint[6].Offset, format, unsigned(dumpColorDescription.Width),
                      unsigned(dumpColorDescription.Height), readbackFootprint[6].Footprint.RowPitch);
        if (!ColorPpmSupported(format) && logFile)
        {
            static std::atomic<unsigned> logged { 0 };
            if (logged.fetch_add(1, std::memory_order_relaxed) < 4)
            {
                std::fprintf(logFile, "PACKED_DUMP color reason=unknown_format format=%u\n", unsigned(format));
                std::fflush(logFile);
            }
        }
    }

    // Engine coverage image: which pixels carry an object record, tinted by
    // object id and brightened by the depth key.
    void writePacked(const std::wstring& path, const std::byte* data)
    {
        FILE* file = _wfopen(path.c_str(), L"wb");
        if (!file)
            return;
        std::fprintf(file, "P6\n%u %u\n255\n", width, height);
        unsigned covered = 0;
        const auto decode = [](std::uint64_t record, unsigned shift)
        {
            const auto raw = static_cast<unsigned>((record >> shift) & 0x7ffu);
            return (raw & 0x400u) ? int(raw) - 2048 : int(raw);
        };
        packedMotionPixels = packedMotionSumAbsX = packedMotionSumAbsY = 0;
        packedMotionMaxAbsX = packedMotionMaxAbsY = packedMotionSaturated = 0;
        for (unsigned y = 0; y < height; ++y)
        {
            const auto* row = reinterpret_cast<const std::uint64_t*>(data + UINT64(y) * width * 8);
            for (unsigned x = 0; x < width; ++x)
            {
                const auto record = row[x];
                const auto id = unsigned(record & 0x7fffu);
                // The top bit of the 18-bit high key is the coverage class the
                // capture set from the material opacity threshold; the low 17
                // bits are the depth key.
                const auto highKey = unsigned((record >> 46) & 0x3ffffu);
                const auto depthKey = highKey & 0x1ffffu;
                std::byte pixel[3] { std::byte(0), std::byte(0), std::byte(0) };
                if (id)
                {
                    ++covered;
                    const int motionX = decode(record, 35), motionY = decode(record, 24);
                    const auto absX = unsigned(motionX < 0 ? -motionX : motionX);
                    const auto absY = unsigned(motionY < 0 ? -motionY : motionY);
                    packedMotionSumAbsX += absX;
                    packedMotionSumAbsY += absY;
                    packedMotionMaxAbsX = (std::max)(packedMotionMaxAbsX, absX);
                    packedMotionMaxAbsY = (std::max)(packedMotionMaxAbsY, absY);
                    if (absX >= 1023 || absY >= 1023)
                        ++packedMotionSaturated;
                    ++packedMotionPixels;
                    const auto hue = unsigned((id * 2654435761u) >> 24) & 0xffu;
                    pixel[0] = static_cast<std::byte>(64 + ((hue * 3) & 0xbf));
                    pixel[1] = static_cast<std::byte>(64 + ((hue * 5) & 0xbf));
                    pixel[2] = static_cast<std::byte>(64 + (depthKey * 255u / 131071u));
                }
                std::fwrite(pixel, 1, 3, file);
            }
        }
        packedCovered = covered;
        std::fclose(file);
    }

    void writeSamples(const std::wstring& path, const std::byte* motionData, const std::byte* depthData,
                      const std::byte* counterData, const std::byte* originalMotionData,
                      const std::byte* originalDepthData) const
    {
        FILE* file = _wfopen(path.c_str(), L"wb");
        if (!file)
            return;
        std::fprintf(file, "serial=%u frame=%u size=%ux%u engine_covered_pixels=%u engine_inputs=%u\n", dumpSerial,
                     dumpFrame, width, height, packedCovered, originalMotionData && originalDepthData ? 1u : 0u);
        // The jitter pair of the dumped frame. Modes 1/2 use the previous one,
        // modes 3/4 the current one, so the residual offset of a dump names the
        // term that has to be removed.
        std::fprintf(file, "jitter=(%.6f,%.6f) previous_jitter=(%.6f,%.6f) mode=%u gain=%.2f\n", dumpJitterX,
                     dumpJitterY, dumpPreviousJitterX, dumpPreviousJitterY, lastJitterMode, lastJitterGain);
        // The colour half of the frame this dump describes. Zero means the
        // motion records stand alone, which is the pre-2026-09-19 shape of
        // every earlier series.
        std::fprintf(file, "color=%u format=%u extent=%ux%u\n", dumpColorCopied ? 1u : 0u,
                     unsigned(dumpColorDescription.Format), unsigned(dumpColorDescription.Width),
                     unsigned(dumpColorDescription.Height));
        // Object-id side of the dump. packrec=1: -packrec.bin holds the exact
        // records. pipelines=1: -pipelines.txt maps their object ids to
        // pipelines (PackedMotionCapture.h). complete=0 means the capture could
        // not record a whole frame for it. deferred counts the composes the table
        // held this dump back (bounded). A dump without this line predates both
        // files.
        std::fprintf(file,
                     "packrec=%u pipelines=%u pipeline_records=%u pipeline_ids=%u pipelines_complete=%u "
                     "pipelines_deferred=%u\n",
                     dumpRecordsWritten ? 1u : 0u, dumpIdsWritten ? 1u : 0u, dumpIdSummary.records, dumpIdSummary.ids,
                     dumpIdSummary.complete ? 1u : 0u, dumpIdSummary.deferred);
        if (counterData)
        {
            const auto* value = reinterpret_cast<const unsigned*>(counterData);
            std::fprintf(file, "dispatched_pixels=%u packed_pixels=%u edge_pixels=%u interior_pixels=%u\n", value[0],
                         value[1], value[2], value[3]);
            // 2026-09-17 contamination audit: the material alpha of every packed
            // pixel and what the compose did with it. The opacity buckets are
            // <0.25, <0.5, <0.75 and >=0.75 of the packed pixels. The apply
            // buckets are not covered, boundary band applied below the interior
            // threshold, boundary take and interior take; their sum is the
            // packed pixel count.
            std::fprintf(file, "opacity_buckets=%u/%u/%u/%u apply_buckets=%u/%u/%u/%u\n", value[4], value[5],
                         value[6], value[7], value[8], value[9], value[10], value[11]);
            // Diagnostic engine-proximity gate: how many already-selected pixels
            // kept the engine's own motion and depth, and the radius that did
            // it. Slot 12 is written only while the gate is on, so a gate-off
            // capture reports zero. The value is stable while the gate is off,
            // which keeps the C22 flip comparison usable.
            std::fprintf(file, "gate_skip=%u gate_px=%.4f\n", value[12], lastEngineGatePx);
            // Diagnostic depth route of the dumped compose: 1 means the motion
            // was substituted while the engine's depth under it stayed, and
            // depth_sub counts the pixels whose depth the compose wrote (slot
            // 13, zero for every frame of a depthkeep capture).
            std::fprintf(file, "depthkeep=%u depth_sub=%u\n", lastDepthKeep ? 1u : 0u, value[13]);
            // Simultaneous stripe A/B of the dumped compose (slot 14, byte 56).
            // stripe_skip counts the pixels the coverage rule had selected and
            // the stripe left on the engine's value: the delivered count of this
            // frame is edge_pixels + interior_pixels - stripe_skip. Zero on every
            // path without the stripe, so an earlier capture keeps its meaning.
            // stripe names bit 5 of the mode word the compose handed the shader;
            // the dump also prints the whole word so a capture can be attributed
            // without the live status snapshot.
            std::fprintf(file, "stripes=%u stripe_skip=%u debug_mode=%u\n",
                         (lastDebugMode & 32u) ? 1u : 0u, value[14], lastDebugMode);
            // Opaque occlusion (slot 15, byte 60): selected pixels whose engine
            // depth is nearer than the object's record; they keep the engine's
            // motion and depth. Diagnostic bit 6 disables the test.
            std::fprintf(file, "occluded=%u\n", value[15]);
        }
        if (packedMotionPixels)
            std::fprintf(file,
                         "captured_motion_pixels=%llu mean_abs_pixels=(%.3f,%.3f) max_abs_pixels=(%.3f,%.3f) "
                         "saturated=%u mvec_scale=(%.6f,%.6f)\n",
                         static_cast<unsigned long long>(packedMotionPixels),
                         double(packedMotionSumAbsX) / (8.0 * double(packedMotionPixels)),
                         double(packedMotionSumAbsY) / (8.0 * double(packedMotionPixels)),
                         double(packedMotionMaxAbsX) / 8.0, double(packedMotionMaxAbsY) / 8.0,
                         packedMotionSaturated, lastScaleX, lastScaleY);
        for (unsigned gy = 0; gy < 9; ++gy)
        {
            for (unsigned gx = 0; gx < 16; ++gx)
            {
                const auto x = (gx * width) / 16, y = (gy * height) / 9;
                const auto* row = reinterpret_cast<const unsigned short*>(readbackRow(motionData, 0, y));
                const auto* depthRow = reinterpret_cast<const float*>(readbackRow(depthData, 1, y));
                if (originalMotionData && originalDepthData)
                {
                    const auto* originalRow =
                        reinterpret_cast<const unsigned short*>(readbackRow(originalMotionData, 3, y));
                    const auto* originalDepthRow =
                        reinterpret_cast<const float*>(readbackRow(originalDepthData, 4, y));
                    std::fprintf(file,
                                 "x=%u y=%u mv=(%.6f,%.6f) original_mv=(%.6f,%.6f) depth=%.6f original_depth=%.6f\n",
                                 x, y, halfToFloat(row[x * 4 + 0]), halfToFloat(row[x * 4 + 1]),
                                 halfToFloat(originalRow[x * 4 + 0]), halfToFloat(originalRow[x * 4 + 1]), depthRow[x],
                                 originalDepthRow[x]);
                }
                else
                    std::fprintf(file, "x=%u y=%u mv=(%.6f,%.6f) depth=%.6f\n", x, y, halfToFloat(row[x * 4 + 0]),
                                 halfToFloat(row[x * 4 + 1]), depthRow[x]);
            }
        }
        std::fclose(file);
    }

  public:
    bool dispatch(ID3D12GraphicsCommandList* command, const PackedMotionFrame& packed,
                  ID3D12Resource* originalMotion, ID3D12Resource* originalDepth,
                  D3D12_RESOURCE_STATES motionState, D3D12_RESOURCE_STATES depthState,
                  float scaleX, float scaleY, float jitterX, float jitterY, Controls controls,
                  ComposeMark mark = {})
    {
        lastScaleX = scaleX;
        lastScaleY = scaleY;
        // Named rejections: a silent false here removes the whole correction
        // while the FG still receives the substituted texture.
        if (!command)
        {
            noteComposeSkip("dispatch_command");
            return false;
        }
        if (!pipeline)
        {
            noteComposeSkip("dispatch_pipeline");
            return false;
        }
        if (!packed)
        {
            noteComposeSkip("dispatch_frame");
            return false;
        }
        if (packed.width != width || packed.height != height)
        {
            noteComposeSkip("dispatch_extent");
            return false;
        }
        if (!originalMotion || !originalDepth)
        {
            noteComposeSkip("dispatch_input");
            return false;
        }
        if (scaleX <= 0 || scaleY <= 0)
        {
            noteComposeSkip("dispatch_scale");
            return false;
        }
        if (motionState != D3D12_RESOURCE_STATE_COPY_SOURCE)
            transition(command, originalMotion, motionState, D3D12_RESOURCE_STATE_COPY_SOURCE);
        if (depthState != D3D12_RESOURCE_STATE_COPY_SOURCE)
            transition(command, originalDepth, depthState, D3D12_RESOURCE_STATE_COPY_SOURCE);
        // The staged row limit controls the new compute work only. Any output
        // that the FG evaluation consumes must hold a complete frame: rows the
        // compute never touches would otherwise keep stale memory, and NGX
        // reading that is wrong by construction (the 22:49 reset ran with 240
        // composed rows and a stale remainder). A pending dump reads the whole
        // frame as well.
        const unsigned copyRows = (dumpRequests.load(std::memory_order_relaxed) || controls.packedSubstitute)
                                      ? height
                                      : (std::min)(height, (std::max)(1u, controls.packedRows));
        {
            const D3D12_BOX box { 0, 0, 0, width, copyRows, 1 };
            D3D12_TEXTURE_COPY_LOCATION source {}, target {};
            source.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            target.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            source.pResource = originalMotion;
            target.pResource = motion;
            command->CopyTextureRegion(&target, 0, 0, 0, &source, &box);
            source.pResource = originalDepth;
            target.pResource = depth;
            command->CopyTextureRegion(&target, 0, 0, 0, &source, &box);
        }
        if (motionState != D3D12_RESOURCE_STATE_COPY_SOURCE)
            transition(command, originalMotion, D3D12_RESOURCE_STATE_COPY_SOURCE, motionState);
        if (depthState != D3D12_RESOURCE_STATE_COPY_SOURCE)
            transition(command, originalDepth, D3D12_RESOURCE_STATE_COPY_SOURCE, depthState);

        if (outputMotionState != D3D12_RESOURCE_STATE_UNORDERED_ACCESS)
        {
            transition(command, motion, outputMotionState, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            outputMotionState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        }
        if (outputDepthState != D3D12_RESOURCE_STATE_UNORDERED_ACCESS)
        {
            transition(command, depth, outputDepthState, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            outputDepthState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        }
        if (outputSelectionState != D3D12_RESOURCE_STATE_UNORDERED_ACCESS)
        {
            transition(command, selection, outputSelectionState, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            outputSelectionState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        }
        struct Constants
        {
            unsigned width, height;
            float scaleX, scaleY;
            unsigned edgeWidth;
            // 0..1. Covered pixels whose material opacity reaches this value
            // take the object motion and depth. Everything else keeps the
            // engine's value.
            float opacityThreshold;
            unsigned debug, jitterMode;
            float jitterX, jitterY;
            float previousJitterX, previousJitterY;
            float jitterGain;
            // Engine-proximity gate radius in pixels (0 with the gate disabled).
            // Bit 4 of the mode word above keeps the engine's depth under a
            // substituted motion (live diagnostic). Keeps the block at 56 bytes.
            float gatePx;
            // The capture endpoint carries the frame-to-frame projection term,
            // so the product default compose is mode 0 (no term) at gain 100 and
            // the jitter mode below is a diagnostic. The engine's own motion is
            // never read or written by this block.
        };
        // One named value for the shader's mode word: the dump header reports
        // the same word, so a capture names the diagnostic it measured.
        const unsigned debugMode = (dumpRequests.load(std::memory_order_relaxed) ? 1u : 0u) |
                                   (controls.packedSkipRead ? 2u : 0u) | (controls.zeroMotion ? 4u : 0u) |
                                   (controls.engineGate ? 0u : 8u) | (DepthKeepEnabled() ? 16u : 0u) |
                                   (StripeProbeEnabled() ? 32u : 0u) |
                                   // Bit 7: frame depth convention for the opaque-occlusion test
                                   // (DLSSG.DepthInverted). The record's own reverse stamp comes from the
                                   // draw's depth comparator and is absent for depth-disabled draws.
                                   (FrameDepthInverted() ? 128u : 0u);
        lastDebugMode = debugMode;
        const Constants constants { width, height, scaleX, scaleY, (std::min)(controls.edgeWidth, 4u),
                       controls.opacityThreshold(), debugMode,
                       (std::min)(controls.jitterMode, 7u), jitterX, jitterY,
                       jitterHistoryValid ? previousJitterX : jitterX,
                       jitterHistoryValid ? previousJitterY : jitterY,
                       float((std::min)(controls.jitterGain, 255u)) / 100.f,
                       controls.engineGatePx() };
        static_assert(sizeof(Constants) == 56);
        const auto packedSlot = packedView(packed.resource);
        command->SetDescriptorHeaps(1, &heap);
        command->SetComputeRootSignature(root);
        command->SetComputeRootDescriptorTable(0, gpu(packedSlot));
        command->SetComputeRootDescriptorTable(1, gpu(kPackedViewCount));
        command->SetComputeRoot32BitConstants(2, 14, &constants, 0);
        command->SetPipelineState(pipeline);
        // The packed raster writes this buffer as a UAV/ROV during the draw, so
        // the compute read needs an explicit UAV barrier. Missing it is the
        // hazard that correlated with the 2026-09-14 driver resets.
        D3D12_RESOURCE_BARRIER packedBarrier {};
        packedBarrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
        packedBarrier.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
        packedBarrier.UAV.pResource = packed.resource;
        command->ResourceBarrier(1, &packedBarrier);
        // Coverage counters are per measured frame: reset, then let the shader
        // count dispatched, packed, edge and interior pixels.
        transition(command, counters, counterState, D3D12_RESOURCE_STATE_COPY_DEST);
        counterState = D3D12_RESOURCE_STATE_COPY_DEST;
        command->CopyBufferRegion(counters, 0, zeroCounters, 0, 64);
        // The shader writes these counters as a UAV (register u4). A copy
        // destination is not a UAV state, so the transition is explicit here
        // rather than left to implicit promotion.
        transition(command, counters, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        counterState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        D3D12_RESOURCE_BARRIER counterBarrier {};
        counterBarrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
        counterBarrier.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
        counterBarrier.UAV.pResource = counters;
        command->ResourceBarrier(1, &counterBarrier);
        // Staged coverage: run the real dispatch over the configured top rows so
        // a pathological cost cannot time out the GPU; the rest of the frame
        // keeps the copied original motion. Raise GlassFG/ComposeRows after a
        // clean run; the INI and settings UI control this without a rebuild.
        const auto rows = (std::min)(height, (std::max)(1u, controls.packedRows));
        // Isolation: the copies and the swap stay, only the compute dispatch is
        // optional, so a reset can be attributed to the swap or to the compute.
        if (controls.packedCompute)
            command->Dispatch((width + 7) / 8, (rows + 7) / 8, 1);
        if (controls.trace && logFile && TraceWanted())
        {
            std::fprintf(logFile, "TRACE_DISPATCH frame=%u rows=%u groups=%u edges=%u compute=%u copy=%u\n",
                         packed.frame, rows, (width + 7) / 8, (std::min)(controls.edgeWidth, 4u),
                         controls.packedCompute ? 1u : 0u, copyRows);
            std::fflush(logFile);
        }
        // A request is honoured only on a drained pipeline: the readback
        // buffers already exist (allocated by the health thread), and no
        // earlier compose is still executing. A request that cannot start
        // stays counted and is picked up by a later compose instead of being
        // recorded into a compose that is already in flight. The capture's
        // object-id table can hold it back as well, by the frame or two that a
        // complete table takes and never more than its own bound (dumpIdsReady).
        if (!dumpPending && dumpRequests.load(std::memory_order_acquire) && readbackReady() && composeReady() &&
            dumpIdsReady(packed.frame))
        {
            transition(command, motion, outputMotionState, D3D12_RESOURCE_STATE_COPY_SOURCE);
            transition(command, depth, outputDepthState, D3D12_RESOURCE_STATE_COPY_SOURCE);
            ID3D12Resource* sources[] { motion, depth };
            for (unsigned i = 0; i < 2; ++i)
            {
                D3D12_TEXTURE_COPY_LOCATION source {}, target {};
                source.pResource = sources[i];
                source.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
                target.pResource = readback[i];
                target.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
                target.PlacedFootprint = readbackFootprint[i];
                command->CopyTextureRegion(&target, 0, 0, 0, &source, nullptr);
            }
            transition(command, motion, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            transition(command, depth, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            outputMotionState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
            outputDepthState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
            transition(command, counters, counterState, D3D12_RESOURCE_STATE_COPY_SOURCE);
            counterState = D3D12_RESOURCE_STATE_COPY_SOURCE;
            command->CopyBufferRegion(readback[2], 0, counters, 0, 64);
            transition(command, counters, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            counterState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
            // Same-frame originals for a direct before/after comparison. The
            // engine's own textures are only read here, and the transition uses
            // the state Streamline reported for the frame generation boundary.
            // Those copies are the ones that can make the compose wait on
            // resources the frame generation path owns, so the switch defaults
            // to off and the composed motion, depth, counters and packed
            // records are dumped on their own.
            if (kDumpEngineInputs)
            {
                if (motionState != D3D12_RESOURCE_STATE_COPY_SOURCE)
                    transition(command, originalMotion, motionState, D3D12_RESOURCE_STATE_COPY_SOURCE);
                if (depthState != D3D12_RESOURCE_STATE_COPY_SOURCE)
                    transition(command, originalDepth, depthState, D3D12_RESOURCE_STATE_COPY_SOURCE);
                ID3D12Resource* originals[] { originalMotion, originalDepth };
                for (unsigned i = 0; i < 2; ++i)
                {
                    D3D12_TEXTURE_COPY_LOCATION source {}, target {};
                    source.pResource = originals[i];
                    source.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
                    target.pResource = readback[3 + i];
                    target.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
                    target.PlacedFootprint = readbackFootprint[3 + i];
                    command->CopyTextureRegion(&target, 0, 0, 0, &source, nullptr);
                }
                if (motionState != D3D12_RESOURCE_STATE_COPY_SOURCE)
                    transition(command, originalMotion, D3D12_RESOURCE_STATE_COPY_SOURCE, motionState);
                if (depthState != D3D12_RESOURCE_STATE_COPY_SOURCE)
                    transition(command, originalDepth, D3D12_RESOURCE_STATE_COPY_SOURCE, depthState);
            }
            // The frame's colour, so the motion the compose wrote can be read
            // against the image it describes. The resource and its state come
            // from the frame generation evaluation itself, and the copy only
            // happens while a dump has been requested.
            dumpColorCopied = dumpColorKnown && !dumpColorAbandoned && readback[6] != nullptr &&
                              dumpColorResource != nullptr;
            if (dumpColorCopied)
            {
                if (dumpColorState != D3D12_RESOURCE_STATE_COPY_SOURCE)
                    transition(command, dumpColorResource, dumpColorState, D3D12_RESOURCE_STATE_COPY_SOURCE);
                D3D12_TEXTURE_COPY_LOCATION source {}, target {};
                source.pResource = dumpColorResource;
                source.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
                target.pResource = readback[6];
                target.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
                target.PlacedFootprint = readbackFootprint[6];
                command->CopyTextureRegion(&target, 0, 0, 0, &source, nullptr);
                if (dumpColorState != D3D12_RESOURCE_STATE_COPY_SOURCE)
                    transition(command, dumpColorResource, D3D12_RESOURCE_STATE_COPY_SOURCE, dumpColorState);
            }
            // Engine coverage: the packed object records themselves.
            transition(command, packed.resource, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                       D3D12_RESOURCE_STATE_COPY_SOURCE);
            command->CopyBufferRegion(readback[5], 0, packed.resource, 0, readbackBytes[5]);
            transition(command, packed.resource, D3D12_RESOURCE_STATE_COPY_SOURCE,
                       D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            dumpFrame = packed.frame;
            // The jitter pair belongs to the captured frame: the offline
            // residual has to be attributable to one of the two terms.
            dumpJitterX = jitterX;
            dumpJitterY = jitterY;
            dumpPreviousJitterX = previousJitterX;
            dumpPreviousJitterY = previousJitterY;
            dumpRequests.fetch_sub(1, std::memory_order_relaxed);
            dumpPending = true;
            dumpPendingSinceMs.store(steadyNowMs(), std::memory_order_release);
        }
        // The compose wrote the outputs as UAVs. Flush those writes explicitly
        // before the state transitions back: a transition out of
        // UNORDERED_ACCESS is not a UAV barrier on every driver, and this
        // hazard class already correlated with the 2026-09-14 driver resets
        // (see the packed-input barrier above).
        for (auto* resource : { motion, depth, selection })
            if (resource)
            {
                D3D12_RESOURCE_BARRIER outputBarrier {};
                outputBarrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
                outputBarrier.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
                outputBarrier.UAV.pResource = resource;
                command->ResourceBarrier(1, &outputBarrier);
            }
        transition(command, motion, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
        transition(command, depth, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
        {
            const D3D12_BOX box { 0, 0, 0, width, height, 1 };
            D3D12_TEXTURE_COPY_LOCATION source {}, target {};
            source.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            target.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            source.pResource = motion;
            target.pResource = motionRead;
            command->CopyTextureRegion(&target, 0, 0, 0, &source, &box);
            source.pResource = depth;
            target.pResource = depthRead;
            command->CopyTextureRegion(&target, 0, 0, 0, &source, &box);
        }
        transition(command, motion, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
        transition(command, depth, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
        transition(command, selection, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                   D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        // Diagnostic: a dump can only be read back every 36-37 frames, so its
        // header cannot say which (jitter, previous) pair produced a captured
        // vector. One line per compose carries that pair for the frame number the
        // dump header reports, which is what separates a wrong frame offset from
        // a wrong jitter term. Flushed every 32 lines: the line has to be
        // readable while the field is still running, but one flush per compose
        // would be the dominant cost of the path being measured.
        if (controls.jitterLog && logFile)
        {
            static std::atomic<unsigned> jitterLines { 0 };
            std::fprintf(logFile, "PACKED_JITTER frame=%u j=%.6f,%.6f p=%.6f,%.6f mode=%u gain=%.3f\n", packed.frame,
                         jitterX, jitterY, jitterHistoryValid ? previousJitterX : jitterX,
                         jitterHistoryValid ? previousJitterY : jitterY, (std::min)(controls.jitterMode, 7u),
                         float(std::min(controls.jitterGain, 255u)) / 100.f);
            // The call phase of the same compose. A separate line on purpose:
            // the frame reader above is parsed with an end-anchored expression
            // (.exploratory/scratch/jitterlog_fit_20260918.py:70), so its format
            // stays byte-identical and the new fields join on `frame`.
            std::fprintf(logFile, "PACKED_COMPOSE_MARK frame=%u mfidx=%u mfcount=%u fgframe=%llu\n", packed.frame,
                         mark.multiFrameIndex, mark.multiFrameCount,
                         static_cast<unsigned long long>(mark.fgFrame));
            if ((jitterLines.fetch_add(1, std::memory_order_relaxed) & 31u) == 31u)
                std::fflush(logFile);
        }
        // The next compose sees this frame as its predecessor, which is the
        // frame whose jitter a material-side motion computation misses when it
        // does not carry the previous projection's sub-pixel offset.
        previousJitterX = jitterX;
        previousJitterY = jitterY;
        jitterHistoryValid = true;
        lastJitterMode = (std::min)(controls.jitterMode, 7u);
        lastJitterGain = float(std::min(controls.jitterGain, 255u)) / 100.f;
        lastEngineGatePx = controls.engineGate ? controls.engineGatePx() : 0.f;
        lastDepthKeep = DepthKeepEnabled();
        return true;
    }

    ID3D12Resource* motionOutput() const { return motionRead; }
    ID3D12Resource* depthOutput() const { return depthRead; }
    ID3D12Resource* selectionOutput() const { return selection; }
    bool acceptsGuides(const D3D12_RESOURCE_DESC& motionValue, const D3D12_RESOURCE_DESC& depthValue) const
    {
        return device != nullptr && motionValue.Width == motionDescription.Width &&
               motionValue.Height == motionDescription.Height && motionValue.Format == motionDescription.Format &&
               depthValue.Width == depthDescription.Width && depthValue.Height == depthDescription.Height &&
               depthValue.Format == depthDescription.Format;
    }
    void releaseAfterGpuDrain()
    {
        for (auto** resource : { &motion, &depth, &selection, &motionRead, &depthRead })
        {
            if (*resource)
                (*resource)->Release();
            *resource = nullptr;
        }
        for (auto** resource :
             { &readback[0], &readback[1], &readback[2], &readback[3], &readback[4], &readback[5], &counters,
               &zeroCounters })
        {
            if (*resource)
                (*resource)->Release();
            *resource = nullptr;
        }
        if (dumpFence) dumpFence->Release();
        dumpFence = nullptr;
        dumpPending = false;
        dumpPendingSinceMs.store(0, std::memory_order_release);
        dumpRequests.store(0, std::memory_order_relaxed);
        dumpPrepareAttempts = 0;
        for (auto& context : composeContexts)
        {
            if (context.allocator) context.allocator->Release();
            if (context.list) context.list->Release();
            if (context.fence) context.fence->Release();
            context = {};
        }
        composeActive = 0;
        dumpComposeContext = 0;
        // The resources are gone; the next allocation starts in the states the
        // textures are created with.
        counterState = D3D12_RESOURCE_STATE_COMMON;
        outputMotionState = D3D12_RESOURCE_STATE_COPY_DEST;
        outputDepthState = D3D12_RESOURCE_STATE_COPY_DEST;
        outputSelectionState = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
        for (auto& view : packedViews)
            view.resource = nullptr;
        packedViewCount = 0;
        if (pipeline) pipeline->Release();
        if (root) root->Release();
        if (heap) heap->Release();
        pipeline = nullptr; root = nullptr; heap = nullptr; device = nullptr;
    }
};
} // namespace GlassFg
