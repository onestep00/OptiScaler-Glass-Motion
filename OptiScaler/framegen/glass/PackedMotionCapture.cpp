#include "pch.h"
#include "PackedMotionCapture.h"
#include "DxilVertexHistory.h"
#include "CyberpunkDraws.h"
#include "GeometryCommands.h"
#include "GeometryDrawCapture.h"
#include "GeometryHealth.h"
#include "MotionFramePair.h"
#include "PackedMotionMappings.h"
#include <d3dcompiler.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <limits>
#include <mutex>
#include <thread>

namespace GlassFg
{
namespace
{
using Microsoft::WRL::ComPtr;
constexpr unsigned FrameCount = 3, RecordingCount = 64, FgCommandCount = 4;
constexpr unsigned MappingCapacity = 16384, ConstantCapacity = 4096;
// Vertex-history arena. 4096 pages x 128 vertices was regularly exhausted in
// live scenes (a single large mesh asks for a contiguous power-of-two block),
// so the arena was doubled once. The live bar scene still fills it: the module
// reported arena_full=361,289 failed reservations with arena_used 8,192/8,192
// pages and ~2,500 live histories at 3.3 pages each, and every failed
// reservation is a surface whose motion vector stays at the engine's value.
// The arena is therefore doubled again. The GPU cost is 2 x HistoryCapacity x
// 32 bytes (128 MB total) and the backing store is a power-of-two buddy
// allocator, so the page count and the vertex capacity have to move together.
constexpr unsigned HistoryPages = 16384, HistoryPageVertices = 128;
constexpr unsigned HistoryCapacity = HistoryPages * HistoryPageVertices;
static_assert(HistoryCapacity == (1u << 21), "history capacity must stay a power of two");
// The draw batch's frame field is a render-context tick that stays zero in some
// configurations; a zero frame number made the capture reject every draw and the
// frame generation side then had no candidate at all. Fall back to the engine
// render frame the command observer tracks, which is the same counter the
// correction uses, and finally to a private monotone counter.
std::uint32_t resolvedDrawFrame(const GeometryDrawView& draw)
{
    if (draw.frame != 0)
        return draw.frame;
    static std::atomic<std::uint32_t> fallback { 0 };
    const auto commands = GetGeometryCommandStats().lastFrame;
    if (commands != 0)
        return commands;
    return fallback.fetch_add(1, std::memory_order_relaxed) + 1;
}

void checked(HRESULT value, const char* message)
{
    if (FAILED(value))
        throw std::runtime_error(message);
}

ComPtr<ID3D12Resource> buffer(ID3D12Device* device, UINT64 bytes, D3D12_HEAP_TYPE heapType,
                              D3D12_RESOURCE_FLAGS flags = D3D12_RESOURCE_FLAG_NONE,
                              D3D12_RESOURCE_STATES initial = D3D12_RESOURCE_STATE_COMMON)
{
    D3D12_HEAP_PROPERTIES heap {};
    heap.Type = heapType;
    D3D12_RESOURCE_DESC desc {};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Width = bytes;
    desc.Height = 1;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    desc.Flags = flags;
    ComPtr<ID3D12Resource> result;
    const auto state = heapType == D3D12_HEAP_TYPE_UPLOAD ? D3D12_RESOURCE_STATE_GENERIC_READ : initial;
    checked(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, state,
                                             nullptr, IID_PPV_ARGS(&result)), "Packed buffer creation failed");
    return result;
}

// Same live owner, independent of the volatile per-update ticket. The provider
// supplies the registration lifetime in the key generation instead.
bool sameOwner(const GeometryDrawIdentity& a, const GeometryDrawIdentity& b)
{
    return a.proxy == b.proxy && a.mesh == b.mesh && a.slot == b.slot;
}

class Capture final : public GeometryDrawCaptureOwner
{
    struct Recording { ID3D12GraphicsCommandList* command = nullptr; std::uint64_t epoch = 0; };
    struct Frame
    {
        std::uint32_t number = 0, mappingUsed = 0, constantsUsed = 0;
        GeometryInstance* mappings = nullptr;
        std::byte* constants = nullptr;
        ComPtr<ID3D12Resource> mapping, constantBuffer, capture;
        ComPtr<ID3D12CommandAllocator> clearAllocator;
        ComPtr<ID3D12GraphicsCommandList> clearCommand;
        std::array<Recording, RecordingCount> recordings {};
        std::array<std::shared_ptr<const GeometryPipelineEntry>, ConstantCapacity> pipelines;
        ID3D12GraphicsCommandList* consumerCommand = nullptr;
        ComPtr<ID3D12CommandQueue> producerQueue, consumerQueue, orderedQueue;
        ComPtr<ID3D12Fence> syncFence;
        std::uint64_t producerValue = 0, consumerValue = 0, syncValue = 0;
        // Monotonic submission sequence, used to correlate a producer frame with
        // the FG command that was submitted after it.
        std::uint64_t submitOrder = 0;
        // Submission batch index (one per ExecuteCommandLists call). Items in
        // the same batch keep the same value, so a frame recorded on the same
        // command list as the FG call is still admitted.
        std::uint64_t submitBatch = 0;
        bool clearSubmitted = false, syncPending = false;
    };
    struct FgCommand
    {
        ID3D12GraphicsCommandList* command = nullptr;
        ComPtr<ID3D12CommandQueue> queue;
        std::uint64_t submitOrder = 0;
        std::uint64_t submitBatch = 0;
    };
    // Engine-owned Signal/Wait correlation. The orderedQueue condition must not
    // depend on this capture's private fence, which nothing in the engine waits on.
    struct ObservedSignal
    {
        ComPtr<ID3D12Fence> fence;
        ComPtr<ID3D12CommandQueue> queue;
        std::uint64_t value = 0, order = 0;
    };

    ComPtr<ID3D12Device> device;
    ComPtr<ID3D12RootSignature> clearRoot;
    ComPtr<ID3D12PipelineState> clearPipeline;
    ComPtr<ID3D12Resource> history[2];
    ComPtr<ID3D12CommandAllocator> historyClearAllocator;
    ComPtr<ID3D12GraphicsCommandList> historyClearCommand;
    ComPtr<ID3D12CommandQueue> historyQueue;
    ComPtr<ID3D12Fence> producerFence, consumerFence;
    std::array<Frame, FrameCount> frames;
    PackedMotionMappings<4096, 4, HistoryPages, HistoryPageVertices> objectMappings;
    PackedMotionIdentityProvider identitySource;
    std::array<FgCommand, FgCommandCount> fgCommands;
    std::array<ObservedSignal, 256> observedSignals {};
    unsigned nextObservedSignal = 0;
    std::uint64_t submitSequence = 0;
    std::uint64_t submitBatchSequence = 0;
    // Diagnostic only: module log and the last frame that produced a replay
    // trace line, so the line appears once per captured frame.
    FILE* log = nullptr;
    std::uint32_t lastReplayFrame = 0;
    mutable std::mutex mutex;
    PackedMotionCaptureStatus counters;
    std::array<PackedMotionCaptureStatus::ChunkCount, 16> unknownChunks {}, topologyChunks {}, missingChunks {};
    std::array<PackedMotionCaptureStatus::ChunkCount, 16> overflowChunks {};
    // Bounded family table for the packed capture. A four-probe hash window
    // keeps the per-span cost at a few comparisons; a family that cannot be
    // placed is counted, never silently merged into another mesh.
    struct SpanFamily
    {
        std::uint32_t chunk = 0, mesh = 0, vertices = 0;
        std::uint64_t count = 0;
    };
    static constexpr unsigned SpanFamilyCount = 256, SpanFamilyProbes = 8;
    std::array<SpanFamily, SpanFamilyCount> spanFamilies {};
    std::uint64_t spanFamilyEvictions = 0;
    std::uint64_t frameSpanCount = 0;
    std::uint32_t configuredWidth = 0, configuredHeight = 0;
    std::uint64_t nextProducer = 0, nextConsumer = 0;
    MotionFramePair framePair;
    bool failed = false, historyClearSubmitted = false;

    static void noteChunk(std::array<PackedMotionCaptureStatus::ChunkCount, 16>& list,
                          std::uint32_t chunk) noexcept
    {
        if (!chunk) return;
        auto* smallest = &list[0];
        for (auto& entry : list)
        {
            if (entry.chunk == chunk)
            {
                ++entry.count;
                return;
            }
            if (entry.count < smallest->count)
                smallest = &entry;
        }
        smallest->chunk = chunk;
        smallest->count = 1;
    }

    void noteSpanFamily(std::uint32_t chunk, std::uint32_t mesh, std::uint32_t vertices) noexcept
    {
        if (!chunk || !mesh) return;
        const auto base = unsigned(((std::uint64_t(mesh) * 0x9E3779B97F4A7C15ull) ^
                                    (std::uint64_t(chunk) << 7)) & (SpanFamilyCount - 1));
        SpanFamily* free = nullptr;
        for (unsigned i = 0; i < SpanFamilyProbes; ++i)
        {
            auto& entry = spanFamilies[(base + i) & (SpanFamilyCount - 1)];
            if (entry.count)
            {
                if (entry.chunk == chunk && entry.mesh == mesh)
                {
                    ++entry.count;
                    return;
                }
                continue;
            }
            if (!free) free = &entry;
        }
        if (!free)
        {
            ++spanFamilyEvictions;
            return;
        }
        *free = { chunk, mesh, vertices, 1 };
    }

    bool completed(ID3D12Fence* fence, std::uint64_t value)
    {
        if (!value) return true;
        const auto done = fence->GetCompletedValue();
        if (done == std::numeric_limits<std::uint64_t>::max()) failed = true;
        return done != std::numeric_limits<std::uint64_t>::max() && done >= value;
    }
    bool reusable(Frame& frame)
    {
        if (!frame.number) return true;
        for (const auto& recording : frame.recordings)
            if (recording.command) return false;
        // A frame whose draws were captured is the candidate the frame generation
        // evaluation consumes. Recycling it as soon as the draw recordings were
        // discarded left the FG side with no candidate at all
        // (acquire_no_candidate with every slot empty). Hold it until the FG call
        // consumes it, with a bounded margin so the pool cannot be pinned.
        if (frame.producerValue && !frame.consumerCommand)
        {
            const auto newest = newestFrameNumber();
            if (newest == 0 || frame.number + 4 >= newest)
                return false;
        }
        return !frame.consumerCommand && completed(producerFence.Get(), frame.producerValue) &&
               completed(consumerFence.Get(), frame.consumerValue);
    }
    std::uint32_t newestFrameNumber() const
    {
        std::uint32_t newest = 0;
        for (const auto& value : frames)
            if (value.number > newest)
                newest = value.number;
        return newest;
    }
    bool beginMappings(std::uint32_t number)
    {
        if (objectMappings.frame() == number) return true;
        if (!number || number < objectMappings.frame()) return false;
        auto completedThrough = number - 1;
        // Every owned recording remains in these slots until both recording
        // discard and GPU completion. Gaps with no owned work need no retirement.
        for (auto& value : frames)
            if (value.number && value.number <= completedThrough && !reusable(value))
                completedThrough = value.number - 1;
        if (failed || !objectMappings.beginFrame(number, completedThrough)) return false;
        // The family table describes the frame that is being captured now; a
        // cumulative table would be dominated by earlier scenes and would hide
        // the object the current camera actually looks at.
        spanFamilies = {};
        spanFamilyEvictions = 0;
        frameSpanCount = 0;
        return true;
    }
    Frame* frame(std::uint32_t number)
    {
        for (auto& value : frames)
            if (value.number == number) return &value;
        for (auto& value : frames)
            if (reusable(value))
            {
                value.number = number;
                for (unsigned i = 0; i < value.constantsUsed; ++i) value.pipelines[i].reset();
                value.mappingUsed = value.constantsUsed = 0;
                value.recordings = {};
                value.consumerCommand = nullptr;
                value.producerQueue.Reset(); value.consumerQueue.Reset(); value.orderedQueue.Reset();
                value.syncFence.Reset();
                value.producerValue = value.consumerValue = value.syncValue = 0;
                value.clearSubmitted = value.syncPending = false;
                return &value;
            }
        // Stale-slot reclaim. A recorded render list that is never reset again,
        // or an FG list that was replaced without a destroyed notification,
        // otherwise pins its slot forever and the three-slot pool drains. Both
        // fences completing means nothing can still be reading or writing that
        // capture, so dropping the stale bookkeeping cannot race in-flight work.
        for (auto& value : frames)
        {
            if (!value.number || value.number >= number) continue;
            if (!completed(producerFence.Get(), value.producerValue) ||
                !completed(consumerFence.Get(), value.consumerValue))
                continue;
            ++counters.slotReclaimed;
            value.number = number;
            for (unsigned i = 0; i < value.constantsUsed; ++i) value.pipelines[i].reset();
            value.mappingUsed = value.constantsUsed = 0;
            value.recordings = {};
            value.consumerCommand = nullptr;
            value.producerQueue.Reset(); value.consumerQueue.Reset(); value.orderedQueue.Reset();
            value.syncFence.Reset();
            value.producerValue = value.consumerValue = value.syncValue = 0;
            value.clearSubmitted = value.syncPending = false;
            return &value;
        }
        ++counters.slotBusy;
        return nullptr;
    }
    static bool contains(const Frame& frame, ID3D12CommandList* command)
    {
        for (const auto& value : frame.recordings)
            if (value.command == command) return true;
        return false;
    }
    bool record(Frame& value, ID3D12GraphicsCommandList* command, std::uint64_t epoch)
    {
        for (const auto& entry : value.recordings)
            if (entry.command == command) return entry.epoch == epoch;
        for (const auto& other : frames)
            if (&other != &value && contains(other, command)) return false;
        for (auto& entry : value.recordings)
            if (!entry.command)
            {
                entry = { command, epoch };
                return true;
            }
        return false;
    }
    void compileClear()
    {
        static constexpr char shader[] = R"(
RWByteAddressBuffer Target : register(u0);
cbuffer Constants : register(b0) { uint Words; uint GroupsX; };
[numthreads(256,1,1)] void Clear(uint3 id : SV_DispatchThreadID)
{
    uint index = (id.y * GroupsX * 256) + id.x;
    if (index < Words) Target.Store(index * 4, 0);
})";
        ComPtr<ID3DBlob> code, errors;
        checked(D3DCompile(shader, sizeof(shader) - 1, "packed-clear.hlsl", nullptr, nullptr, "Clear", "cs_5_0",
                           D3DCOMPILE_OPTIMIZATION_LEVEL3 | D3DCOMPILE_ENABLE_STRICTNESS, 0, &code, &errors),
                "Packed clear shader compilation failed");
        D3D12_ROOT_PARAMETER parameters[2] {};
        parameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
        parameters[0].Descriptor = { 0, 0 };
        parameters[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        parameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        parameters[1].Constants = { 0, 0, 2 };
        parameters[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        D3D12_ROOT_SIGNATURE_DESC rootDesc { 2, parameters, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_NONE };
        ComPtr<ID3DBlob> serialized;
        checked(D3D12SerializeRootSignature(&rootDesc, D3D_ROOT_SIGNATURE_VERSION_1, &serialized, &errors),
                "Packed clear root serialization failed");
        checked(device->CreateRootSignature(0, serialized->GetBufferPointer(), serialized->GetBufferSize(),
                                            IID_PPV_ARGS(&clearRoot)), "Packed clear root creation failed");
        D3D12_COMPUTE_PIPELINE_STATE_DESC desc {};
        desc.pRootSignature = clearRoot.Get();
        desc.CS = { code->GetBufferPointer(), code->GetBufferSize() };
        checked(device->CreateComputePipelineState(&desc, IID_PPV_ARGS(&clearPipeline)),
                "Packed clear pipeline creation failed");
    }
    void recordClear(ID3D12Resource* target, UINT64 words, ComPtr<ID3D12CommandAllocator>& allocator,
                     ComPtr<ID3D12GraphicsCommandList>& command)
    {
        checked(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)),
                "Packed clear allocator creation failed");
        checked(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), clearPipeline.Get(),
                                          IID_PPV_ARGS(&command)), "Packed clear command creation failed");
        const UINT64 groups = (words + 255) / 256;
        const UINT groupsX = UINT((std::min)(groups, UINT64(65535)));
        const UINT groupsY = UINT((groups + groupsX - 1) / groupsX);
        if (!groupsX || groupsY > 65535) throw std::runtime_error("Packed clear dispatch exceeds D3D12 limits");
        const UINT values[] { UINT(words), groupsX };
        command->SetComputeRootSignature(clearRoot.Get());
        command->SetComputeRootUnorderedAccessView(0, target->GetGPUVirtualAddress());
        command->SetComputeRoot32BitConstants(1, 2, values, 0);
        command->Dispatch(groupsX, groupsY, 1);
        D3D12_RESOURCE_BARRIER barrier {};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
        barrier.UAV.pResource = target;
        command->ResourceBarrier(1, &barrier);
        checked(command->Close(), "Packed clear command close failed");
    }
    void recordHistoryClear()
    {
        checked(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&historyClearAllocator)),
                "History clear allocator creation failed");
        checked(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, historyClearAllocator.Get(),
                                          clearPipeline.Get(), IID_PPV_ARGS(&historyClearCommand)),
                "History clear command creation failed");
        // 16 Mi words needs 65,536 thread groups, which is one past the
        // 65,535 limit of a single dimension, so the clear is dispatched in
        // two dimensions exactly like the packed capture clear.
        const UINT words = HistoryCapacity * 8;
        const UINT64 groups = (UINT64(words) + 255) / 256;
        const UINT groupsX = UINT((std::min)(groups, UINT64(65535)));
        const UINT groupsY = UINT((groups + groupsX - 1) / groupsX);
        if (!groupsX || groupsY > 65535)
            throw std::runtime_error("History clear dispatch exceeds D3D12 limits");
        for (auto& target : history)
        {
            const UINT values[] { words, groupsX };
            historyClearCommand->SetComputeRootSignature(clearRoot.Get());
            historyClearCommand->SetComputeRootUnorderedAccessView(0, target->GetGPUVirtualAddress());
            historyClearCommand->SetComputeRoot32BitConstants(1, 2, values, 0);
            historyClearCommand->Dispatch(groupsX, groupsY, 1);
            D3D12_RESOURCE_BARRIER barrier {};
            barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
            barrier.UAV.pResource = target.Get();
            historyClearCommand->ResourceBarrier(1, &barrier);
        }
        checked(historyClearCommand->Close(), "History clear command close failed");
    }

  public:
    Capture(ID3D12Device* value, std::uint32_t width, std::uint32_t height, PackedMotionIdentityProvider source,
            FILE* logFile = nullptr)
        : device(value), identitySource(source)
    {
        if (!identitySource) throw std::invalid_argument("Verified motion identity source unavailable");
        if (!value || !width || !height || width > 32768 || height > 32768 ||
            std::uint64_t(width) * height > UINT32_MAX / 8)
            throw std::invalid_argument("Invalid packed capture extent");
        configuredWidth = width; configuredHeight = height;
        log = logFile;
        compileClear();
        history[0] = buffer(device.Get(), UINT64(HistoryCapacity) * 32, D3D12_HEAP_TYPE_DEFAULT,
                            D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
        history[1] = buffer(device.Get(), UINT64(HistoryCapacity) * 32, D3D12_HEAP_TYPE_DEFAULT,
                            D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
        checked(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&producerFence)),
                "Packed producer fence creation failed");
        checked(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&consumerFence)),
                "Packed consumer fence creation failed");
        const UINT64 pixels = UINT64(width) * height;
        for (auto& frame : frames)
        {
            frame.mapping = buffer(device.Get(), UINT64(MappingCapacity) * sizeof(GeometryInstance),
                                   D3D12_HEAP_TYPE_UPLOAD);
            frame.constantBuffer = buffer(device.Get(), UINT64(ConstantCapacity) * 256, D3D12_HEAP_TYPE_UPLOAD);
            // The compose reads this raster as a UAV on the frame generation
            // queue while the capture writes it on the graphics queue. The
            // simultaneous-access flag would legalise that overlap, but this
            // driver rejects the flag for every combination
            // (D3D12CreateDevice probe: E_INVALIDARG), so the overlap has to be
            // removed by submitting the compose on the producer queue instead.
            frame.capture = buffer(device.Get(), pixels * 8, D3D12_HEAP_TYPE_DEFAULT,
                                   D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
                                   D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            D3D12_RANGE noRead { 0, 0 };
            checked(frame.mapping->Map(0, &noRead, reinterpret_cast<void**>(&frame.mappings)),
                    "Packed mapping map failed");
            checked(frame.constantBuffer->Map(0, &noRead, reinterpret_cast<void**>(&frame.constants)),
                    "Packed constants map failed");
            recordClear(frame.capture.Get(), pixels * 2, frame.clearAllocator, frame.clearCommand);
        }
        recordHistoryClear();
        counters.initialized = counters.healthy = true;
        counters.width = width; counters.height = height;
    }

    bool prepare(ID3D12GraphicsCommandList* command, const GeometryDrawView& draw,
                 const GeometryIndexedArguments& args, const std::shared_ptr<const GeometryPipelineEntry>& pipeline,
                 const GraphicsRootBindings&, GeometryPreparedDraw& prepared) noexcept override
    {
        std::unique_lock lock(mutex, std::try_to_lock);
        if (!lock) return false; // Counters share this lock too.
        const auto frameNumber = resolvedDrawFrame(draw);
        if (failed || !command || frameNumber == 0 || !args.instances || args.instances > MappingCapacity ||
            !pipeline || !pipeline->packed || !pipeline->root || !pipeline->root->extended)
        {
            if (pipeline && !pipeline->packed)
            {
                ++counters.missingPipeline;
                noteChunk(missingChunks, draw.chunk);
            }
            return false;
        }
        const auto* raster = ReadGeometryRasterState(command);
        const auto shape = ReadCyberpunkMeshShape(draw);
        if (!raster || !raster->usable())
        {
            ++counters.topologyRejected; ++counters.rasterRejected; noteChunk(topologyChunks, draw.chunk);
            return false;
        }
        if (!shape || !shape.vertices || shape.vertices > HistoryCapacity)
        {
            ++counters.topologyRejected; ++counters.shapeRejected; noteChunk(topologyChunks, draw.chunk);
            return false;
        }
        // The engine reports the vertex count as 64 bits; every draw that passes
        // the capacity check above is known to fit, so the narrowing is exact.
        const auto vertices = std::uint32_t(shape.vertices);
        if (!std::isfinite(raster->viewport.TopLeftX) || !std::isfinite(raster->viewport.TopLeftY) ||
            !std::isfinite(raster->viewport.Width) || !std::isfinite(raster->viewport.Height) ||
            raster->viewport.Width <= 0 || raster->viewport.Height <= 0 || raster->viewport.TopLeftX < 0 ||
            raster->viewport.TopLeftY < 0 || raster->viewport.TopLeftX + raster->viewport.Width > configuredWidth ||
            raster->viewport.TopLeftY + raster->viewport.Height > configuredHeight)
        {
            ++counters.topologyRejected; ++counters.viewportRejected; noteChunk(topologyChunks, draw.chunk);
            return false;
        }
        auto* frameSlot = frame(frameNumber);
        if (!frameSlot) return false;
        if (!beginMappings(frameNumber)) { ++counters.orderingRejected; return false; }
        if (frameSlot->mappingUsed > MappingCapacity - args.instances || frameSlot->constantsUsed == ConstantCapacity)
        {
            ++counters.mappingOverflow;
            return false;
        }
        const auto epoch = ReadGeometryRecordingEpoch(command);
        if (!epoch || !record(*frameSlot, command, epoch))
        {
            ++counters.orderingRejected;
            return false;
        }
        const auto mappingBase = frameSlot->mappingUsed;
        auto* mapping = frameSlot->mappings + mappingBase;
        std::memset(mapping, 0, args.instances * sizeof(GeometryInstance));
        bool any = false;
        for (unsigned spanIndex = 0; spanIndex < draw.objects.size(); ++spanIndex)
        {
            const auto& span = draw.objects[spanIndex];
            const auto& owner = span.parent ? span.parent : span.identity;
            if (!owner || !span.count || std::uint64_t(span.first) + span.count > args.instances)
            {
                if (span.count)
                {
                    ++counters.unknownIdentity; ++counters.unknownOwnerSpan;
                    noteChunk(unknownChunks, draw.chunk);
                }
                continue;
            }
            for (unsigned ordinal = 0; ordinal < span.count; ++ordinal)
            {
                VertexHistoryKey key;
                if (!identitySource.resolve(identitySource.context, command, draw, shape, *pipeline,
                    spanIndex, ordinal, key) || !key || !key.object.generation)
                {
                    ++counters.unknownIdentity; ++counters.unknownResolve;
                    noteChunk(unknownChunks, draw.chunk); continue;
                }
                if (!sameOwner(key.object, owner))
                {
                    ++counters.unknownIdentity; ++counters.unknownOwnerMismatch;
                    noteChunk(unknownChunks, draw.chunk); continue;
                }
                if (key.chunk != draw.chunk || key.vertexFactory != shape.vertexFactory ||
                    key.pipeline != pipeline->identity)
                {
                    ++counters.unknownIdentity; ++counters.unknownFieldMismatch;
                    noteChunk(unknownChunks, draw.chunk); continue;
                }
                if ((!span.identity || span.count != 1) && !key.arrayGeneration)
                {
                    ++counters.unknownIdentity; ++counters.unknownNoArrayGeneration;
                    noteChunk(unknownChunks, draw.chunk); continue;
                }
                const auto allocation = objectMappings.acquire(key, vertices, frameNumber);
                if (!allocation)
                {
                    ++counters.historyOverflow;
                    noteChunk(overflowChunks, draw.chunk);
                    continue;
                }
                // The family table is a bounded diagnostic key, not an identity
                // check (the hash still mixes the full mesh address), so the low
                // 32 bits printed in the status line are enough here.
                noteSpanFamily(draw.chunk, std::uint32_t(key.object.mesh), vertices);
                ++frameSpanCount;
                auto& item = mapping[span.first + ordinal];
                item.historyBase = allocation.history.base;
                item.vertices = allocation.history.vertices;
                item.vertexOrigin = 0;
                item.generation = allocation.history.generation;
                item.left = item.top = 0;
                item.width = configuredWidth; item.height = configuredHeight;
                item.pixelBase = 1; item.stride = configuredWidth;
                item.pixelCapacity = configuredWidth * configuredHeight + 1;
                item.statusIndex = 0;
                item.reserved[0] = allocation.boundaryId;
                any = true;
            }
        }
        if (!any) return false;
        const auto constantIndex = frameSlot->constantsUsed++;
        frameSlot->pipelines[constantIndex] = pipeline;
        const auto depthFunction = pipeline->description.DepthStencilState.DepthFunc;
        const bool reverse = depthFunction == D3D12_COMPARISON_FUNC_GREATER ||
                             depthFunction == D3D12_COMPARISON_FUNC_GREATER_EQUAL;
        const MaterialCaptureConstants constants {
            raster->viewport.TopLeftX, raster->viewport.TopLeftY,
            1.f / raster->viewport.Width, 1.f / raster->viewport.Height,
            0, 0, frameNumber, reverse ? 1u : 0u,
            0, 0, configuredWidth, configuredHeight,
            0, configuredWidth, configuredWidth * configuredHeight, 0
        };
        std::memcpy(frameSlot->constants + constantIndex * 256, &constants, sizeof(constants));
        frameSlot->mappingUsed += args.instances;
        prepared.pipeline = pipeline->packed.Get();
        prepared.history = { mappingBase, MappingCapacity, HistoryCapacity, 0, args.instances, 0,
                             frameNumber, frameNumber - 1 };
        prepared.previous = history[(frameNumber - 1) & 1]->GetGPUVirtualAddress();
        prepared.current = history[frameNumber & 1]->GetGPUVirtualAddress();
        prepared.material = frameSlot->constantBuffer->GetGPUVirtualAddress() + UINT64(constantIndex) * 256;
        prepared.capture = frameSlot->capture->GetGPUVirtualAddress();
        prepared.mapping = frameSlot->mapping->GetGPUVirtualAddress();
        // Crash attribution for the replay path. Sparse on purpose: one line per
        // few hundred frames keeps the log bounded while still proving that the
        // packed raster was drawn after the last load.
        if (log && frameNumber != lastReplayFrame && frameNumber % 300 == 0)
        {
            lastReplayFrame = frameNumber;
            std::fprintf(log, "TRACE_REPLAY frame=%u chunk=%u\n", frameNumber, draw.chunk);
            std::fflush(log);
        }
        return true;
    }

    void finish(ID3D12GraphicsCommandList*, bool recorded) noexcept override
    {
        if (!recorded) return;
        std::lock_guard lock(mutex);
        ++counters.admittedDraws;
        GeometryTelemetry::counts[GeometryCaptureDraws].fetch_add(1, std::memory_order_relaxed);
        GeometryTelemetry::changedMs[GeometryCaptureDraws].store(GetTickCount64(), std::memory_order_relaxed);
    }

    void beforeSubmit(ID3D12CommandQueue* queue, UINT count, ID3D12CommandList* const* lists) noexcept override
    {
        std::lock_guard lock(mutex);
        if (failed || !queue || !lists || !count || queue->GetDesc().Type != D3D12_COMMAND_LIST_TYPE_DIRECT) return;
        for (auto& frame : frames)
        {
            bool selected = false;
            for (UINT i = 0; i < count; ++i) selected |= contains(frame, lists[i]);
            if (!selected || frame.clearSubmitted) continue;
            if (!historyClearSubmitted)
            {
                historyQueue = queue;
                ID3D12CommandList* historyClear[] { historyClearCommand.Get() };
                queue->ExecuteCommandLists(1, historyClear);
                historyClearSubmitted = true;
            }
            else if (historyQueue.Get() != queue)
            {
                failed = true; ++counters.orderingRejected; continue;
            }
            if (frame.producerQueue && frame.producerQueue.Get() != queue)
            {
                failed = true; ++counters.orderingRejected; continue;
            }
            frame.producerQueue = queue;
            ID3D12CommandList* clear[] { frame.clearCommand.Get() };
            queue->ExecuteCommandLists(1, clear);
            frame.clearSubmitted = true;
            ++counters.capturedFrames;
        }
    }

    void submitted(ID3D12CommandQueue* queue, UINT count, ID3D12CommandList* const* lists) noexcept override
    {
        std::lock_guard lock(mutex);
        if (failed || !queue || !lists || !count) return;
        const auto batch = ++submitBatchSequence;
        for (auto& command : fgCommands)
            if (command.command)
                for (UINT i = 0; i < count; ++i)
                    if (lists[i] == command.command && (!command.queue || command.queue.Get() == queue))
                    {
                        command.queue = queue;
                        command.submitOrder = ++submitSequence;
                        command.submitBatch = batch;
                    }
        for (auto& frame : frames)
        {
            bool produced = false, consumed = false;
            for (UINT i = 0; i < count; ++i)
            {
                produced |= contains(frame, lists[i]);
                consumed |= frame.consumerCommand && lists[i] == frame.consumerCommand;
            }
            if (produced)
            {
                if (!frame.clearSubmitted || (frame.producerQueue && frame.producerQueue.Get() != queue) ||
                    nextProducer == UINT64_MAX || FAILED(queue->Signal(producerFence.Get(), ++nextProducer)))
                { failed = true; ++counters.orderingRejected; }
                else
                {
                    frame.producerQueue = queue; frame.producerValue = nextProducer;
                    frame.submitOrder = ++submitSequence;
                    frame.submitBatch = batch;
                    frame.syncPending = true; frame.syncFence.Reset(); frame.orderedQueue.Reset();
                }
            }
            if (consumed)
            {
                if ((frame.consumerQueue && frame.consumerQueue.Get() != queue) || nextConsumer == UINT64_MAX ||
                    FAILED(queue->Signal(consumerFence.Get(), ++nextConsumer)))
                { failed = true; ++counters.orderingRejected; }
                else
                { frame.consumerQueue = queue; frame.consumerValue = nextConsumer; }
            }
        }
    }

    void discarded(ID3D12GraphicsCommandList* command) noexcept override
    {
        discard(command, false);
    }

    void discard(const void* command, bool destroyed) noexcept
    {
        std::lock_guard lock(mutex);
        for (auto& frame : frames)
        {
            for (auto& recording : frame.recordings)
                if (recording.command == command) recording = {};
            if (frame.consumerCommand == command) frame.consumerCommand = nullptr;
        }
        if (destroyed)
            for (auto& fg : fgCommands)
                if (fg.command == command) fg = {};
    }

    void signal(ID3D12CommandQueue* queue, ID3D12Fence* fence, UINT64 value) noexcept override
    {
        std::lock_guard lock(mutex);
        if (failed || !queue || !fence) return;
        // Record engine-owned signals so a later Wait on the same fence/value can
        // establish the producer -> FG queue dependency. Our own fences are
        // private to this capture and nothing in the engine waits on them.
        if (fence != producerFence.Get() && fence != consumerFence.Get())
        {
            auto& record = observedSignals[nextObservedSignal++ % observedSignals.size()];
            record.fence = fence;
            record.queue = queue;
            record.value = value;
            record.order = submitSequence;
        }
        for (auto& frame : frames)
            if (frame.syncPending && frame.producerQueue.Get() == queue)
            {
                frame.syncFence = fence; frame.syncValue = value; frame.syncPending = false;
            }
    }

    void wait(ID3D12CommandQueue* queue, ID3D12Fence* fence, UINT64 value) noexcept override
    {
        std::lock_guard lock(mutex);
        if (failed || !queue || !fence) return;
        for (const auto& record : observedSignals)
            if (record.fence.Get() == fence && record.value == value && record.queue && record.queue.Get() != queue)
                for (auto& frame : frames)
                    if (frame.producerQueue.Get() == record.queue.Get() && frame.submitOrder &&
                        frame.submitOrder <= record.order)
                        frame.orderedQueue = queue;
        for (auto& frame : frames)
            if (frame.syncFence.Get() == fence && frame.syncValue == value)
                frame.orderedQueue = queue;
    }

    PackedMotionFrame acquire(ID3D12GraphicsCommandList* command, std::uint32_t width, std::uint32_t height,
                              std::uint64_t fgFrame, bool reset)
    {
        std::lock_guard lock(mutex);
        if (failed || !command || fgFrame == UINT64_MAX || width != configuredWidth || height != configuredHeight)
        {
            if (fgFrame == UINT64_MAX) ++counters.noFgFrame;
            return {};
        }
        FgCommand* fg = nullptr;
        for (auto& value : fgCommands) if (value.command == command) fg = &value;
        if (!fg)
            for (auto& value : fgCommands) if (!value.command) { value.command = command; fg = &value; break; }
        if (!fg || !fg->queue) { ++counters.noFgQueue; return {}; }
        Frame* selected = nullptr;
        bool multiple = false;
        for (auto& value : frames)
            // A frame is admitted when its producer submission precedes the FG
            // command submission. If an engine Wait established the dependency,
            // the waiting queue must be the FG queue. The private-fence condition
            // used previously could never be satisfied.
            if (value.producerValue && value.clearSubmitted && value.number > framePair.engineFrame() &&
                value.submitBatch && fg->submitBatch && value.submitBatch <= fg->submitBatch &&
                (!value.orderedQueue || value.orderedQueue.Get() == fg->queue.Get()))
            {
                // Several frames can satisfy the order rule (older batches stay
                // eligible until the pair advances). The newest eligible frame
                // is the one this FG call consumed, so pick it instead of
                // rejecting the batch.
                if (selected)
                {
                    multiple = true;
                    if (value.submitBatch < selected->submitBatch ||
                        (value.submitBatch == selected->submitBatch && value.number <= selected->number))
                        continue;
                }
                selected = &value;
            }
        if (!selected)
        {
            // Driver path: Streamline's frame generation block carries no engine
            // frame number, so the ordered lookup above cannot match. Take the
            // newest frame this queue already produced instead; the frames are
            // held until a consumer takes them, so it is the frame under
            // construction for this evaluation.
            for (auto& value : frames)
                if (value.producerValue && value.clearSubmitted && !value.consumerCommand &&
                    (!value.orderedQueue || value.orderedQueue.Get() == fg->queue.Get()) &&
                    (!selected || value.number > selected->number))
                    selected = &value;
        }
        if (!selected)
        {
            ++counters.orderingRejected;
            ++counters.acquireNoCandidate;
            // Bounded attribution: which admission condition the candidate frames
            // failed, so a silent rejection can be interpreted.
            static std::atomic<unsigned> rejected { 0 };
            if (log != nullptr && rejected.fetch_add(1, std::memory_order_relaxed) < 6)
            {
                std::fprintf(log, "PACKED_ACQUIRE reject fgFrame=%llu engineFrame=%llu fgSubmitBatch=%llu\n",
                             static_cast<unsigned long long>(fgFrame),
                             static_cast<unsigned long long>(framePair.engineFrame()),
                             static_cast<unsigned long long>(fg->submitBatch));
                for (const auto& value : frames)
                    std::fprintf(log,
                                 "PACKED_ACQUIRE frame number=%u producer=%llu clear=%u submitBatch=%llu ordered=%u\n",
                                 value.number, static_cast<unsigned long long>(value.producerValue),
                                 value.clearSubmitted ? 1u : 0u, static_cast<unsigned long long>(value.submitBatch),
                                 value.orderedQueue ? 1u : 0u);
                std::fflush(log);
            }
            return {};
        }
        if (multiple)
            ++counters.acquireAmbiguous; // Informational: older eligible frames were present.
        if (selected->consumerCommand && selected->consumerCommand != command)
        { ++counters.orderingRejected; ++counters.acquireConsumerBusy; return {}; }
        if (!framePair.advance(selected->number, fgFrame, reset))
        { ++counters.orderingRejected; ++counters.acquireStalePair; return {}; }
        selected->consumerCommand = command;
        ++counters.fgFrames;
        return { selected->capture.Get(),
                 configuredWidth,
                 configuredHeight,
                 selected->number,
                 fgFrame,
                 producerFence.Get(),
                 selected->producerValue,
                 static_cast<void*>(selected->producerQueue.Get()) };
    }

    PackedMotionCaptureStatus status()
    {
        std::lock_guard lock(mutex);
        auto value = counters;
        value.unknownChunks = unknownChunks;
        value.topologyChunks = topologyChunks;
        value.missingChunks = missingChunks;
        value.healthy = counters.initialized && !failed;
        const auto& history = objectMappings.historyStats();
        value.historyHits = history.hits;
        value.historyInserted = history.inserted;
        value.historyReclaimed = history.reclaimed;
        value.historyRejectedTopology = history.rejectedTopology;
        value.historySetFull = history.setFull;
        value.historyArenaFull = history.arenaFull;
        value.historyArenaReclaimed = history.arenaReclaimed;
        value.historyArenaFullPages = history.arenaFullPages;
        value.historyArenaFullPageCount = history.arenaFullPageCount;
        value.historyLive = objectMappings.liveHistories();
        value.historyArenaPages = objectMappings.arenaPages();
        value.historyArenaUsedPages = objectMappings.arenaUsedPages();
        value.historyArenaLargestFree = objectMappings.arenaLargestFreePages();
        value.historyFrame = objectMappings.historyFrame();
        value.historyRetiredFrame = objectMappings.historyRetiredFrame();
        value.historyPinnedEntries = objectMappings.historyPinnedEntries();
        value.overflowChunks = overflowChunks;
        value.spanFamilyEvictions = spanFamilyEvictions;
        value.frameSpanCount = frameSpanCount;
        value.admittedSpanFamilyCount = 0;
        {
            // Copy the heaviest families only; the full table stays internal so
            // the periodic line and the control response keep a fixed size.
            std::array<SpanFamily, SpanFamilyCount> sorted = spanFamilies;
            std::sort(sorted.begin(), sorted.end(),
                      [](const SpanFamily& a, const SpanFamily& b) { return a.count > b.count; });
            for (const auto& entry : sorted)
            {
                if (!entry.count || value.admittedSpanFamilyCount >= value.admittedSpans.size()) break;
                value.admittedSpans[value.admittedSpanFamilyCount++] =
                    { entry.chunk, entry.mesh, entry.vertices, entry.count };
            }
        }
        return value;
    }

    void resetCounters()
    {
        std::lock_guard lock(mutex);
        const auto initialized = counters.initialized, healthy = counters.healthy;
        const auto width = counters.width, height = counters.height;
        counters = {};
        counters.initialized = initialized;
        counters.healthy = healthy;
        counters.width = width;
        counters.height = height;
        unknownChunks = {};
        topologyChunks = {};
        missingChunks = {};
        overflowChunks = {};
        spanFamilies = {};
        spanFamilyEvictions = 0;
    }
};

std::atomic<Capture*> active = nullptr;
} // namespace

bool InitializePackedMotionCapture(ID3D12Device* device, std::uint32_t width, std::uint32_t height, FILE* log,
                                   PackedMotionIdentityProvider identities) noexcept
{
    try
    {
        if (auto* current = active.load(std::memory_order_acquire))
        {
            const auto status = current->status();
            return status.healthy && status.width == width && status.height == height;
        }
        auto* capture = new Capture(device, width, height, identities, log);
        if (!RegisterGeometryDrawCapture(capture))
        {
            delete capture;
            if (log) { std::fprintf(log, "PACKED_CAPTURE ready=0 reason=capture_owner_busy\n"); std::fflush(log); }
            return false;
        }
        active.store(capture, std::memory_order_release);
        if (log)
        {
            std::fprintf(log, "PACKED_CAPTURE ready=1 width=%u height=%u frames=%u mapping=%u history_vertices=%u\n",
                         width, height, FrameCount, MappingCapacity, HistoryCapacity);
            std::fflush(log);
        }
        return true;
    }
    catch (const std::exception& error)
    {
        if (log) { std::fprintf(log, "PACKED_CAPTURE ready=0 reason=%s\n", error.what()); std::fflush(log); }
        return false;
    }
}

PackedMotionFrame AcquirePackedMotionFrame(ID3D12GraphicsCommandList* command,
                                           std::uint32_t width, std::uint32_t height,
                                           std::uint64_t fgFrame, bool reset) noexcept
{
    try
    {
        auto* capture = active.load(std::memory_order_acquire);
        return capture ? capture->acquire(command, width, height, fgFrame, reset) : PackedMotionFrame {};
    }
    catch (...) { return {}; }
}

bool ReleasePackedMotionCapture() noexcept
{
    try
    {
        auto* capture = active.exchange(nullptr, std::memory_order_acq_rel);
        if (!capture)
            return false;
        UnregisterGeometryDrawCapture(capture);
        delete capture;
        return true;
    }
    catch (...)
    {
        return false;
    }
}

PackedMotionCaptureStatus ReadPackedMotionCaptureStatus() noexcept
{
    try
    {
        auto* capture = active.load(std::memory_order_acquire);
        return capture ? capture->status() : PackedMotionCaptureStatus {};
    }
    catch (...) { return {}; }
}

void ResetPackedMotionCounters() noexcept
{
    try
    {
        if (auto* capture = active.load(std::memory_order_acquire))
            capture->resetCounters();
    }
    catch (...)
    {
    }
}

void DiscardPackedMotionRecording(const void* command, bool destroyed) noexcept
{
    if (auto* capture = active.load(std::memory_order_acquire)) capture->discard(command, destroyed);
}
} // namespace GlassFg
