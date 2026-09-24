// Packed capture contracts: the second consumer's frame selection, and draw
// admission of the real capture (PackedMotionCapture.cpp) under two recording
// threads, on an independent device with owned fixture draws.
#include "../PackedMotionCapture.cpp"
#include <algorithm>
#include <atomic>
#include <barrier>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace
{
struct Frame
{
    std::uint32_t number;
    std::uint64_t producerValue;
    bool clearSubmitted;
    const void* consumerCommand;
};
// The capture contract calls require from its recording threads as well.
std::atomic<unsigned> checks { 0 };
void require(bool result, const char* message)
{
    ++checks;
    if (!result)
        throw std::runtime_error(message);
}
} // namespace

// The capture's engine and command-observer seams, answered from owned data.
namespace GlassFg
{
namespace
{
GeometryDrawCaptureOwner* fixtureOwner = nullptr;
constexpr std::uint32_t FixtureWidth = 64, FixtureHeight = 32;
const GeometryRasterState fixtureRaster = []
{
    GeometryRasterState value;
    value.viewport = { 0, 0, float(FixtureWidth), float(FixtureHeight), 0, 1 };
    value.scissor = { 0, 0, LONG(FixtureWidth), LONG(FixtureHeight) };
    value.viewportKnown = value.scissorKnown = value.targetsKnown = true;
    value.targetCount = 1;
    return value;
}();
// Owner histories the stale MotionMatrix rule reads, by proxy. A proxy without
// an entry is unreadable and keeps the root graft, as every draw of the
// concurrent contract does. Written only while no recording thread runs.
struct FixtureHistory
{
    std::uint64_t proxy = 0;
    CyberpunkMotionHistory history;
};
std::array<FixtureHistory, 16> fixtureHistories {};
} // namespace
const GeometryRasterState* ReadGeometryRasterState(ID3D12GraphicsCommandList*) noexcept { return &fixtureRaster; }
std::uint64_t ReadGeometryRecordingEpoch(ID3D12GraphicsCommandList*) noexcept { return 1; }
CyberpunkMeshShape ReadCyberpunkMeshShape(const GeometryDrawView&) noexcept
{
    CyberpunkMeshShape shape;
    shape.chunkAddress = 0x4000;
    shape.vertices = 24;
    shape.indices = 36;
    shape.streams = 1;
    shape.indexType = 1;
    shape.vertexFactory = 3;
    return shape;
}
Controls ReadControls() { return {}; }
bool RegisterGeometryDrawCapture(GeometryDrawCaptureOwner* owner) noexcept
{
    if (!owner || fixtureOwner)
        return false;
    fixtureOwner = owner;
    return true;
}
bool UnregisterGeometryDrawCapture(GeometryDrawCaptureOwner* owner) noexcept
{
    if (!owner || owner != fixtureOwner)
        return false;
    fixtureOwner = nullptr;
    return true;
}
const char* GeometryGraftKindName(GeometryGraftKind) noexcept { return "pending"; }
bool ReadCyberpunkMotionHistory(std::uint64_t proxy, CyberpunkMotionHistory& out) noexcept
{
    for (const auto& entry : fixtureHistories)
        if (entry.proxy && entry.proxy == proxy)
        {
            out = entry.history;
            return true;
        }
    out = {};
    return false;
}
CyberpunkMotionHistoryAdmission ReadCyberpunkMotionHistoryAdmission() noexcept { return {}; }
bool ReadCyberpunkMotionSample(std::uint64_t, std::uint32_t, CyberpunkMotionSample& out) noexcept
{
    out = {};
    return false;
}
} // namespace GlassFg

namespace
{
using namespace GlassFg;
using Microsoft::WRL::ComPtr;

// Draws of this chunk hold their identity resolution until the other recording
// thread resolves one as well (bounded). Two draws can only both admit if
// neither thread holds the capture mutex while it resolves.
constexpr std::uint32_t RendezvousChunk = 5;
std::atomic<unsigned> rendezvousArrivals { 0 };
std::atomic<bool> rendezvousMet { false };

// Thread-safe fixture provider: the owner identity of the span, the element's
// ordinal as source index of an array span.
bool fixtureResolve(const void*, PackedMotionIdentityScratch&, ID3D12GraphicsCommandList*, const GeometryDrawView& draw,
                    const CyberpunkMeshShape& shape, const GeometryPipelineEntry& pipeline, std::uint32_t spanIndex,
                    std::uint32_t ordinal, VertexHistoryKey& key) noexcept
{
    key = {};
    if (draw.chunk == RendezvousChunk)
    {
        rendezvousArrivals.fetch_add(1);
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (rendezvousArrivals.load() < 2 && std::chrono::steady_clock::now() < deadline)
            std::this_thread::yield();
        if (rendezvousArrivals.load() >= 2)
            rendezvousMet = true;
    }
    const auto& span = draw.objects[spanIndex];
    const auto& owner = span.parent ? span.parent : span.identity;
    key.object = owner;
    key.view = 1;
    key.pipeline = pipeline.identity;
    key.topology = 1;
    key.chunk = draw.chunk;
    key.vertexFactory = shape.vertexFactory;
    if (span.count != 1)
    {
        key.arrayGeneration = owner.generation;
        key.sourceIndex = ordinal;
    }
    return true;
}
void fixtureFlush(const void*, PackedMotionIdentityScratch&) noexcept {}

void hr(HRESULT value, const char* message)
{
    if (FAILED(value))
        throw std::runtime_error(message);
}

struct Admitted
{
    std::uint32_t frame, mappingBase, instances;
    D3D12_GPU_VIRTUAL_ADDRESS material;
};

// One recorded draw through the owner, as the draw hook issues it.
bool drawOnce(GeometryDrawCaptureOwner& owner, ID3D12GraphicsCommandList* command, std::uint32_t frame,
              std::uint32_t chunk, std::span<const GeometryBatchSpan> spans, std::uint32_t instances,
              const std::shared_ptr<const GeometryPipelineEntry>& pipeline, std::vector<Admitted>* admitted)
{
    static const GraphicsRootBindings bindings;
    const GeometryDrawView draw { spans, 0x7000, frame, chunk, 48, 0, instances };
    const GeometryIndexedArguments args { 36, instances, 0, 0, 0 };
    GeometryPreparedDraw prepared;
    if (!owner.prepare(command, draw, args, pipeline, bindings, prepared))
        return false;
    require(prepared.bindable() && prepared.history.instances == instances && prepared.history.frame == frame,
            "Admitted draw is not bindable");
    owner.finish(command, true);
    if (admitted)
        admitted->push_back({ frame, prepared.history.mappingBase, instances, prepared.material });
    return true;
}

GeometryBatchSpan single(std::uint64_t proxy, std::uint32_t slot)
{
    GeometryBatchSpan span;
    span.identity = { proxy, proxy + 0x100, slot, 1 };
    span.count = 1;
    return span;
}

// Reads upload memory by GPU virtual address: a compute copy through a root
// SRV into a readback buffer. These are the words the grafted shaders read
// from the capture's mapping and material constant buffers.
class UploadReader
{
    static constexpr UINT MaxWords = 256;
    ComPtr<ID3D12RootSignature> root;
    ComPtr<ID3D12PipelineState> copy;
    ComPtr<ID3D12CommandQueue> queue;
    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12GraphicsCommandList> list;
    ComPtr<ID3D12Resource> target, readback;
    ComPtr<ID3D12Fence> fence;
    std::uint64_t submitted = 0;

  public:
    explicit UploadReader(ID3D12Device* device)
    {
        D3D12_ROOT_PARAMETER parameters[3] {};
        parameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
        parameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
        parameters[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        parameters[2].Constants = { 0, 0, 1 };
        for (auto& parameter : parameters)
            parameter.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        const D3D12_ROOT_SIGNATURE_DESC desc { 3, parameters, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_NONE };
        ComPtr<ID3DBlob> serialized, errors, code;
        hr(D3D12SerializeRootSignature(&desc, D3D_ROOT_SIGNATURE_VERSION_1, &serialized, &errors), "reader root blob");
        hr(device->CreateRootSignature(0, serialized->GetBufferPointer(), serialized->GetBufferSize(),
                                       IID_PPV_ARGS(&root)),
           "reader root signature");
        static constexpr char shader[] = R"(
ByteAddressBuffer Source : register(t0);
RWByteAddressBuffer Target : register(u0);
cbuffer Constants : register(b0) { uint Words; };
[numthreads(64, 1, 1)] void main(uint3 id : SV_DispatchThreadID)
{
    if (id.x < Words)
        Target.Store(id.x * 4, Source.Load(id.x * 4));
})";
        hr(D3DCompile(shader, sizeof(shader) - 1, "reader.hlsl", nullptr, nullptr, "main", "cs_5_0", 0, 0, &code,
                      &errors),
           "reader shader");
        D3D12_COMPUTE_PIPELINE_STATE_DESC state {};
        state.pRootSignature = root.Get();
        state.CS = { code->GetBufferPointer(), code->GetBufferSize() };
        hr(device->CreateComputePipelineState(&state, IID_PPV_ARGS(&copy)), "reader pipeline");
        D3D12_COMMAND_QUEUE_DESC queueDesc {};
        queueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        hr(device->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&queue)), "reader queue");
        hr(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)),
           "reader allocator");
        hr(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr,
                                     IID_PPV_ARGS(&list)),
           "reader list");
        hr(list->Close(), "reader list close");
        target = buffer(device, MaxWords * 4, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
                        D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        readback = buffer(device, MaxWords * 4, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_FLAG_NONE,
                          D3D12_RESOURCE_STATE_COPY_DEST);
        hr(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)), "reader fence");
    }
    std::vector<std::uint32_t> read(D3D12_GPU_VIRTUAL_ADDRESS address, UINT words)
    {
        require(address && words && words <= MaxWords, "Upload read range");
        hr(allocator->Reset(), "reader allocator reset");
        hr(list->Reset(allocator.Get(), copy.Get()), "reader list reset");
        list->SetComputeRootSignature(root.Get());
        list->SetComputeRootShaderResourceView(0, address);
        list->SetComputeRootUnorderedAccessView(1, target->GetGPUVirtualAddress());
        list->SetComputeRoot32BitConstant(2, words, 0);
        list->Dispatch((words + 63) / 64, 1, 1);
        D3D12_RESOURCE_BARRIER barrier {};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition = { target.Get(), D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
                               D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE };
        list->ResourceBarrier(1, &barrier);
        list->CopyBufferRegion(readback.Get(), 0, target.Get(), 0, UINT64(words) * 4);
        std::swap(barrier.Transition.StateBefore, barrier.Transition.StateAfter);
        list->ResourceBarrier(1, &barrier);
        hr(list->Close(), "reader list close");
        ID3D12CommandList* lists[] { list.Get() };
        queue->ExecuteCommandLists(1, lists);
        hr(queue->Signal(fence.Get(), ++submitted), "reader signal");
        hr(fence->SetEventOnCompletion(submitted, nullptr), "reader wait");
        void* bytes = nullptr;
        const D3D12_RANGE range { 0, SIZE_T(words) * 4 };
        hr(readback->Map(0, &range, &bytes), "reader map");
        std::vector<std::uint32_t> result(words);
        std::memcpy(result.data(), bytes, result.size() * 4);
        const D3D12_RANGE unchanged {};
        readback->Unmap(0, &unchanged);
        return result;
    }
};

void packedCaptureContract()
{
    ComPtr<ID3D12Device> device;
    hr(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&device)), "device");
    D3D12_ROOT_SIGNATURE_DESC rootDesc {};
    ComPtr<ID3DBlob> serialized, errors, code;
    hr(D3D12SerializeRootSignature(&rootDesc, D3D_ROOT_SIGNATURE_VERSION_1, &serialized, &errors), "root blob");
    ComPtr<ID3D12RootSignature> rootSignature;
    hr(device->CreateRootSignature(0, serialized->GetBufferPointer(), serialized->GetBufferSize(),
                                   IID_PPV_ARGS(&rootSignature)),
       "root signature");
    static constexpr char shader[] = "[numthreads(1,1,1)] void main() {}";
    hr(D3DCompile(shader, sizeof(shader) - 1, "fixture.hlsl", nullptr, nullptr, "main", "cs_5_0", 0, 0, &code,
                  &errors),
       "fixture shader");
    D3D12_COMPUTE_PIPELINE_STATE_DESC psoDesc {};
    psoDesc.pRootSignature = rootSignature.Get();
    psoDesc.CS = { code->GetBufferPointer(), code->GetBufferSize() };
    ComPtr<ID3D12PipelineState> pso;
    hr(device->CreateComputePipelineState(&psoDesc, IID_PPV_ARGS(&pso)), "fixture pipeline");
    auto root = std::make_shared<GeometryRoot>();
    root->extended = rootSignature;
    root->layout = GeometryLayout::PerInstance;
    const auto entry = [&](std::uint64_t identity, bool graft)
    {
        auto value = std::make_shared<GeometryPipelineEntry>();
        value->root = root;
        value->packed = pso;
        value->identity = identity;
        value->nativeGraft = graft;
        if (graft)
            value->packedArray = pso;
        return value;
    };
    // Root graft pipeline drawn by both threads, a graft pipeline whose array
    // variant is the vertex history (VertexHistoryFallback), so the ownerless
    // spans its draws carry stay refused, and a vertex-history pipeline for the
    // large draw.
    const auto captured = entry(0x11, true), refused = entry(0x22, true), history = entry(0x33, false);
    refused->packedArray = nullptr;
    refused->packedHistory = pso;

    require(InitializePackedMotionCapture(device.Get(), FixtureWidth, FixtureHeight, nullptr,
                                          { nullptr, fixtureResolve, fixtureFlush }),
            "Capture initialization");
    require(fixtureOwner != nullptr, "Capture owner not registered");
    auto& owner = *fixtureOwner;
    for (auto& stage : Gate().stage)
        stage.store(0);
    GateArm(true);

    // Two engine job threads record one command list each, frame by frame; the
    // frame advances once both lists are closed and reset, as in the engine.
    constexpr unsigned Threads = 2, Frames = 16, DrawsPerFrame = 800, RefusedEvery = 8;
    std::uint64_t fixtureCommands[Threads] {};
    ID3D12GraphicsCommandList* commands[Threads];
    for (unsigned t = 0; t < Threads; ++t)
        commands[t] = reinterpret_cast<ID3D12GraphicsCommandList*>(&fixtureCommands[t]);
    std::atomic<std::uint32_t> frame { 100 };
    std::barrier frameEnd(Threads, [&]() noexcept
    {
        for (auto* command : commands)
            owner.discarded(command);
        frame.fetch_add(1);
    });
    std::vector<Admitted> admitted[Threads];
    std::atomic<unsigned> refusedCaptured { 0 }, refusedRefused { 0 }, rejected { 0 };
    std::string failure[Threads];
    const auto record = [&](unsigned t)
    {
        try
        {
            const GeometryBatchSpan rendezvous[] { single(0x9000 + t * 0x1000, 90 + t) };
            GeometryBatchSpan ownerless;
            ownerless.count = 1;
            for (unsigned f = 0; f < Frames; ++f)
            {
                const auto number = frame.load();
                if (f == 0 && !drawOnce(owner, commands[t], number, RendezvousChunk, rendezvous, 1, captured,
                                        &admitted[t]))
                    rejected.fetch_add(1);
                for (unsigned d = 0; d < DrawsPerFrame; ++d)
                {
                    const GeometryBatchSpan span[] { single(0x10000 + t * 0x1000 + (d % 8) * 0x10, t * 8 + d % 8) };
                    if (!drawOnce(owner, commands[t], number, 1 + d % 4, span, 1, captured, &admitted[t]))
                        rejected.fetch_add(1);
                    if (t == 1 && d % RefusedEvery == 0)
                    {
                        const bool taken =
                            drawOnce(owner, commands[t], number, 2, std::span(&ownerless, 1), 1, refused, nullptr);
                        (taken ? refusedCaptured : refusedRefused).fetch_add(1);
                    }
                }
                frameEnd.arrive_and_wait();
            }
        }
        catch (const std::exception& error)
        {
            failure[t] = error.what();
            frameEnd.arrive_and_drop();
        }
    };
    std::thread workers[Threads] { std::thread(record, 0), std::thread(record, 1) };
    for (auto& worker : workers)
        worker.join();
    for (const auto& text : failure)
        if (!text.empty())
            throw std::runtime_error(text);

    // No draw is refused because another thread held the capture: both
    // rendezvous draws resolved concurrently and every captured draw admitted.
    const auto& gate = Gate();
    const std::uint64_t capturedDraws = Threads * (1 + Frames * DrawsPerFrame);
    const std::uint64_t refusedDraws = Frames * DrawsPerFrame / RefusedEvery;
    require(rendezvousMet, "Identity resolution of two recording threads was serialized by the capture");
    require(rejected == 0, "A captured draw was refused while another thread recorded");
    require(admitted[0].size() + admitted[1].size() == capturedDraws, "Admitted draw count");
    // Concurrent reservations stay disjoint: one mapping range and one
    // constant slot per admitted draw of a frame.
    std::vector<Admitted> all;
    for (const auto& part : admitted)
        all.insert(all.end(), part.begin(), part.end());
    std::sort(all.begin(), all.end(), [](const Admitted& a, const Admitted& b)
              { return a.frame != b.frame ? a.frame < b.frame : a.mappingBase < b.mappingBase; });
    for (std::size_t i = 1; i < all.size(); ++i)
        if (all[i].frame == all[i - 1].frame)
            require(all[i].mappingBase >= all[i - 1].mappingBase + all[i - 1].instances &&
                        all[i].material != all[i - 1].material,
                    "Two draws of one frame share a mapping range or constant slot");

    // Attribution of the refused pipeline: its ownerless spans and the draws
    // they left empty land on its own GEOMETRY_PIPELINE split, the captured
    // pipeline carries no gate at all.
    const auto& capturedCoverage = captured->coverage;
    const auto& refusedCoverage = refused->coverage;
    require(refusedCaptured == 0 && refusedRefused == refusedDraws,
            "Ownerless span admitted on the vertex-history variant");
    require(capturedCoverage.draws.load() == capturedDraws && capturedCoverage.captures.load() == capturedDraws &&
                capturedCoverage.graft.load() == capturedDraws,
            "Captured pipeline coverage");
    for (unsigned g = 0; g < GeometryPipelineEntry::CoverageGateCount; ++g)
        require(capturedCoverage.gates[g].load() == 0, "Gate charged to the captured pipeline");
    require(refusedCoverage.draws.load() == refusedDraws && refusedCoverage.captures.load() == 0,
            "Refused pipeline coverage");
    for (unsigned g = 0; g < GeometryPipelineEntry::CoverageGateCount; ++g)
        require(refusedCoverage.gates[g].load() == (g == GeometryPipelineEntry::GateSpanOwner ||
                                                            g == GeometryPipelineEntry::GateNoElement
                                                        ? refusedDraws
                                                        : 0),
                "Refused pipeline gate split");
    require(gate.stage[GatePrepareSpan].load() == refusedDraws &&
                gate.stage[GatePrepareNoElement].load() == refusedDraws,
            "GEOMETRY_GATE span/noelement split");
    auto status = ReadPackedMotionCaptureStatus();
    require(status.admittedDraws == capturedDraws, "Recorded draw count");
    require(status.unknownIdentity == refusedDraws && status.unknownOwnerSpan == refusedDraws,
            "Identity refusal counters");

    // A draw larger than one element batch: every element admitted into one
    // reservation, and the next draw's range and constant slot follow it.
    for (auto* command : commands)
        owner.discarded(command);
    const auto large = frame.load() + 1;
    GeometryBatchSpan array;
    array.parent = { 0x70000, 0x70100, 7, 1 };
    array.count = 100;
    array.orderKind = 1;
    std::vector<Admitted> ordered;
    require(drawOnce(owner, commands[0], large, 3, std::span(&array, 1), 100, history, &ordered),
            "Large array draw refused");
    const GeometryBatchSpan next[] { single(0x80000, 3) };
    require(drawOnce(owner, commands[0], large, 1, next, 1, captured, &ordered), "Draw after the large draw refused");
    require(ordered[0].mappingBase == 0 && ordered[1].mappingBase == 100 &&
                ordered[1].material == ordered[0].material + 256,
            "Large draw reserved more than one range or constant slot");
    status = ReadPackedMotionCaptureStatus();
    require(status.frameSpanCount == 101, "Large draw did not admit every element");

    // Stale MotionMatrix rule (stalemotion=camera). A single-instance root draw
    // keeps the root graft while the engine's velocity collector can give its
    // owner object velocity: history record state 1, or the motion flag (which
    // routes skinning or a special input). Otherwise it takes the camera-only
    // variant. The weight does not enter the decision. An unreadable owner and
    // stalemotion=off keep the root. The two variants are distinct pipelines.
    ComPtr<ID3D12PipelineState> cameraPso;
    hr(device->CreateComputePipelineState(&psoDesc, IID_PPV_ARGS(&cameraPso)), "fixture camera pipeline");
    const auto stale = entry(0x44, true);
    stale->packedArray = cameraPso;
    struct StaleCase
    {
        std::uint64_t proxy;
        std::uint64_t record;
        std::uint8_t state, weight, flags;
        bool readable, cameraRule, camera;
        const char* name;
    };
    const StaleCase staleCases[] {
        { 0xa0000, 0, 0xff, 255, 0, true, true, true, "no history record kept the root graft" },
        { 0xa1000, 0x5000, 1, 255, 0, true, true, false, "state 1 record left the root graft" },
        { 0xa2000, 0x5000, 0, 255, 0, true, true, true, "state 0 record kept the root graft" },
        { 0xa3000, 0x5000, 2, 255, 0, true, true, true, "aged record kept the root graft" },
        { 0xa4000, 0x5000, 1, 0, 0, true, true, false, "zero-weight state 1 record left the root graft" },
        { 0xa5000, 0x5000, 1, 0, 1, true, true, false, "skinned zero-weight state 1 proxy left the root graft" },
        { 0xa6000, 0, 0xff, 255, 1, true, true, false, "motion flag without a record left the root graft" },
        { 0xa7000, 0x5000, 2, 255, 1, true, true, false, "motion flag on an aged record left the root graft" },
        { 0xa8000, 0, 0xff, 255, 0, false, true, false, "unreadable owner left the root graft" },
        { 0xa9000, 0, 0xff, 255, 0, true, false, false, "stalemotion=off left the root graft" },
    };
    static_assert(std::size(staleCases) <= std::tuple_size_v<decltype(fixtureHistories)>);
    for (unsigned i = 0; i < std::size(staleCases); ++i)
    {
        const auto& value = staleCases[i];
        fixtureHistories[i] = {};
        if (value.readable)
        {
            CyberpunkMotionHistory history;
            history.record = value.record;
            history.state = value.state;
            history.weight = value.weight;
            history.flags = value.flags;
            fixtureHistories[i] = { value.proxy, history };
        }
    }
    const auto staleBefore = ReadGeometryGraft(GraftStaleCameraDraws);
    unsigned cameraDraws = 0;
    for (const auto& value : staleCases)
    {
        SetStaleMotionCamera(value.cameraRule);
        const GeometryBatchSpan span[] { single(value.proxy, 40) };
        const GeometryDrawView draw { span, 0x7000, large, 1, 48, 0, 1 };
        const GeometryIndexedArguments args { 36, 1, 0, 0, 0 };
        static const GraphicsRootBindings bindings;
        GeometryPreparedDraw prepared;
        require(owner.prepare(commands[0], draw, args, stale, bindings, prepared), "Stale-rule draw refused");
        owner.finish(commands[0], true);
        require(prepared.pipeline == (value.camera ? cameraPso.Get() : pso.Get()), value.name);
        cameraDraws += value.camera ? 1u : 0u;
    }
    SetStaleMotionCamera(true);
    require(ReadGeometryGraft(GraftStaleCameraDraws) - staleBefore == cameraDraws, "GRAFT stale_camera count");
    require(stale->coverage.graft.load() == std::size(staleCases) - cameraDraws &&
                stale->coverage.array.load() == cameraDraws,
            "Stale-rule pipeline coverage split");

    // Spans without an engine owner (no identity, no parent), the shape of the
    // particles_generic draws in the 2026-09-24 gate logs. On the camera-only
    // variant each such span takes one draw-local identity: its mapping
    // records are live for the grafted VS and carry a frame-unique object ID
    // for the packed PS, with no history block, and the draw gets the material
    // constants (opacity threshold) of any other draw. A draw whose variant
    // would read history keeps the span_owner refusal; a root graft without a
    // camera variant keeps the array refusal.
    for (auto* command : commands)
        owner.discarded(command);
    const auto ownerlessFrame = large + 1;
    ComPtr<ID3D12PipelineState> historyPso;
    hr(device->CreateComputePipelineState(&psoDesc, IID_PPV_ARGS(&historyPso)), "fixture history pipeline");
    const auto rootCamera = entry(0x55, true), cameraRecord = entry(0x66, true), rootHistory = entry(0x77, true),
               rootOnly = entry(0x88, true), historyOnly = entry(0x99, false);
    rootCamera->packedArray = cameraPso;
    cameraRecord->packed = cameraRecord->packedArray = cameraPso;
    rootHistory->packedArray = nullptr;
    rootHistory->packedHistory = historyPso;
    rootOnly->packedArray = nullptr;
    const auto select = packedMotionDumpSelect.load();
    const auto write = packedMotionDumpWrite.load();
    require(select && write && !select(ownerlessFrame), "Dump id table was not armed");
    const auto ownerlessBefore = ReadGeometryGraft(GraftOwnerlessCameraDraws);
    const auto arrayBefore = ReadGeometryGraft(GraftArrayDraws);
    const auto prepareDraw = [&](std::span<const GeometryBatchSpan> spans, std::uint32_t instances,
                                 const std::shared_ptr<const GeometryPipelineEntry>& pipeline,
                                 GeometryPreparedDraw& prepared)
    {
        static const GraphicsRootBindings bindings;
        const GeometryDrawView draw { spans, 0x7000, ownerlessFrame, 0, 48, 0, instances };
        const GeometryIndexedArguments args { 36, instances, 0, 0, 0 };
        prepared = {};
        if (!owner.prepare(commands[0], draw, args, pipeline, bindings, prepared))
            return false;
        owner.finish(commands[0], true);
        return true;
    };
    GeometryBatchSpan none;
    none.count = 1;
    GeometryBatchSpan pair[] { none, none }, three = none, mixed[] { single(0xc0000, 51), none };
    pair[1].first = mixed[1].first = 1;
    three.count = 3;
    const GeometryBatchSpan objectA[] { single(0xb0000, 50) }, objectB[] { single(0xc0000, 51) };
    GeometryPreparedDraw a, b, c, d, e, refusedDraw;
    require(prepareDraw(objectA, 1, rootCamera, a) && a.pipeline == pso.Get(), "Owned draw left the root graft");
    require(prepareDraw(pair, 2, rootCamera, b) && b.pipeline == cameraPso.Get(),
            "Ownerless draw of a root graft was not captured with the camera variant");
    require(prepareDraw(std::span(&three, 1), 3, cameraRecord, c) && c.pipeline == cameraPso.Get(),
            "Ownerless draw of a camera-only record was not captured");
    require(prepareDraw(mixed, 2, rootCamera, d) && d.pipeline == cameraPso.Get(),
            "Draw with an owned and an ownerless span was not captured with the camera variant");
    require(prepareDraw(objectB, 1, rootCamera, e) && e.pipeline == pso.Get(), "Owned draw left the root graft");
    require(!prepareDraw(std::span(&none, 1), 1, rootHistory, refusedDraw),
            "Ownerless span admitted on the vertex-history array variant");
    require(!prepareDraw(std::span(&none, 1), 1, historyOnly, refusedDraw),
            "Ownerless span admitted on a vertex-history pipeline");
    require(!prepareDraw(std::span(&none, 1), 1, rootOnly, refusedDraw),
            "Ownerless span admitted on a root graft without a camera variant");

    UploadReader reader(device.Get());
    const auto records = [&](const GeometryPreparedDraw& prepared)
    {
        const auto words =
            reader.read(prepared.mapping + UINT64(prepared.history.mappingBase) * sizeof(GeometryInstance),
                        prepared.history.instances * UINT(sizeof(GeometryInstance) / 4));
        std::vector<GeometryInstance> result(prepared.history.instances);
        std::memcpy(result.data(), words.data(), words.size() * 4);
        return result;
    };
    const auto ra = records(a), rb = records(b), rc = records(c), rd = records(d), re = records(e);
    // Live for the grafted VS (nonzero generation), no history block, an
    // object ID inside the packed record's 15-bit field.
    const auto live = [](const GeometryInstance& record)
    {
        return record.generation && !record.historyBase && !record.vertices && record.reserved[0] &&
               record.reserved[0] <= 32767;
    };
    for (const auto* draw : { &rb, &rc, &rd })
        for (const auto& record : *draw)
            require(live(record), "Captured element mapping is not a live history-free record");
    const auto id = [](const GeometryInstance& record) { return record.reserved[0]; };
    require(id(rb[0]) != id(rb[1]) && id(rc[0]) == id(rc[1]) && id(rc[1]) == id(rc[2]),
            "Draw-local identity is not one per ownerless span");
    require(id(rd[0]) == id(re[0]) && id(ra[0]) != id(re[0]), "Object boundary IDs changed next to ownerless draws");
    const std::uint32_t local[] { id(rb[0]), id(rb[1]), id(rc[0]), id(rd[1]) };
    for (std::size_t i = 0; i < std::size(local); ++i)
    {
        require(local[i] != id(ra[0]) && local[i] != id(re[0]), "Draw-local ID equals an object ID");
        for (std::size_t j = i + 1; j < std::size(local); ++j)
            require(local[i] != local[j], "Two ownerless spans share an ID");
    }
    const auto constants = [&](const GeometryPreparedDraw& prepared)
    {
        MaterialCaptureConstants value {};
        const auto words = reader.read(prepared.material, UINT(sizeof(value) / 4));
        std::memcpy(&value, words.data(), sizeof(value));
        return value;
    };
    const auto ownedConstants = constants(a), ownerlessConstants = constants(b);
    require(std::memcmp(&ownedConstants, &ownerlessConstants, sizeof(ownedConstants)) == 0 &&
                ownerlessConstants.opacityThreshold == ReadControls().opacityThreshold(),
            "Ownerless draw material constants differ from an owned draw's");

    require(ReadGeometryGraft(GraftOwnerlessCameraDraws) - ownerlessBefore == 3 &&
                ReadGeometryGraft(GraftArrayDraws) == arrayBefore,
            "GRAFT ownerless_camera count");
    const auto gatesAre = [](const GeometryPipelineEntry& pipeline,
                             std::initializer_list<std::pair<unsigned, std::uint64_t>> expected)
    {
        for (unsigned g = 0; g < GeometryPipelineEntry::CoverageGateCount; ++g)
        {
            std::uint64_t count = 0;
            for (const auto& [gate, value] : expected)
                if (gate == g)
                    count = value;
            if (pipeline.coverage.gates[g].load() != count)
                return false;
        }
        return true;
    };
    require(rootCamera->coverage.captures.load() == 4 && rootCamera->coverage.graft.load() == 2 &&
                rootCamera->coverage.array.load() == 2 &&
                gatesAre(*rootCamera, { { GeometryPipelineEntry::GateOwnerlessCamera, 2 } }),
            "Root graft pipeline ownerless coverage");
    require(cameraRecord->coverage.captures.load() == 1 && cameraRecord->coverage.array.load() == 1 &&
                gatesAre(*cameraRecord, { { GeometryPipelineEntry::GateOwnerlessCamera, 1 } }),
            "Camera-only record ownerless coverage");
    for (const auto* pipeline : { rootHistory.get(), historyOnly.get() })
        require(pipeline->coverage.draws.load() == 1 && pipeline->coverage.captures.load() == 0 &&
                    gatesAre(*pipeline, { { GeometryPipelineEntry::GateSpanOwner, 1 },
                                          { GeometryPipelineEntry::GateNoElement, 1 } }),
                "Vertex-history ownerless refusal split");
    require(rootOnly->coverage.draws.load() == 1 && rootOnly->coverage.captures.load() == 0 &&
                rootOnly->coverage.arrayRejected.load() == 1 && gatesAre(*rootOnly, {}),
            "Root graft without a camera variant ownerless refusal");

    // Dump id table rows, which the coverage report reads: an ownerless
    // element is `ownerless`, the owned element of the same draw keeps the
    // draw's variant.
    require(select(ownerlessFrame), "Dump id table of the ownerless frame was not frozen");
    const auto idsPath = std::filesystem::temp_directory_path() /
                         (L"glass-ownerless-ids-" + std::to_wstring(GetCurrentProcessId()) + L".txt");
    PackedMotionDumpIdSummary summary {};
    require(write(idsPath.c_str(), 1, ownerlessFrame, &summary) && summary.complete, "Dump id table was not written");
    std::map<std::uint32_t, std::set<std::string>> variants;
    {
        std::ifstream file(idsPath);
        std::string line;
        while (std::getline(file, line))
        {
            if (line.empty() || line[0] == '#' || line.rfind("PIPELINES", 0) == 0)
                continue;
            std::istringstream fields(line);
            std::uint32_t row = 0;
            std::uint64_t pipeline = 0;
            std::string vertex, pixel, kind, variant;
            fields >> row >> pipeline >> vertex >> pixel >> kind >> variant;
            variants[row].insert(variant);
        }
    }
    std::filesystem::remove(idsPath);
    for (const auto value : local)
        require(variants[value] == std::set<std::string> { "ownerless" }, "Ownerless element dump variant");
    require(variants[id(ra[0])] == std::set<std::string> { "root" } &&
                variants[id(re[0])] == std::set<std::string> { "array", "root" },
            "Owned element dump variant");

    GateArm(false);
    require(ReleasePackedMotionCapture(), "Capture release");
    std::printf("PACKED_CAPTURE_LOCK_OK threads=%u draws=%llu refused_pipeline=%llu lock_waits=%llu "
                "large_elements=100\n",
                Threads, static_cast<unsigned long long>(capturedDraws), static_cast<unsigned long long>(refusedDraws),
                static_cast<unsigned long long>(gate.stage[GatePrepareLockWait].load()));
    std::printf("PACKED_OWNERLESS_OK captured=3 refused=3 local_ids=%u,%u,%u,%u object_ids=%u,%u threshold=%.2f\n",
                local[0], local[1], local[2], local[3], id(ra[0]), id(re[0]),
                double(ownerlessConstants.opacityThreshold));
}
} // namespace

int main()
{
    try
    {
        using GlassFg::SelectSecondConsumerFrame;
        std::array<Frame, 3> frames {{{100, 10, true, nullptr}, {101, 11, true, nullptr}, {102, 12, true, nullptr}}};
        // Slot order, producer age and a newer completed frame cannot change
        // the requested identity. Exercise every ring permutation.
        do
        {
            auto* selected = SelectSecondConsumerFrame(frames, 101);
            require(selected && selected->number == 101, "Exact current frame was not selected");
        } while (std::next_permutation(frames.begin(), frames.end(),
                                      [](const Frame& a, const Frame& b) { return a.number < b.number; }));
        require(!SelectSecondConsumerFrame(frames, 99), "Future producer selected for missing frame");
        require(!SelectSecondConsumerFrame(frames, 103), "Previous producer selected for missing frame");
        require(!SelectSecondConsumerFrame(frames, 0), "Unknown frame selected newest producer");
        require(!SelectSecondConsumerFrame(frames, UINT32_MAX), "Reserved frame admitted");
        require(!SelectSecondConsumerFrame(frames, UINT64_MAX), "Missing frame admitted");
        require(!SelectSecondConsumerFrame(frames, (std::uint64_t{1} << 32) + 101), "Frame truncated to 32 bits");
        frames[1].producerValue = 0;
        require(!SelectSecondConsumerFrame(frames, 101), "Unsubmitted frame fell back to another frame");
        frames[1].producerValue = 11;
        frames[1].clearSubmitted = false;
        require(!SelectSecondConsumerFrame(frames, 101), "Uncleared frame admitted");
        frames[1].clearSubmitted = true;
        frames[1].consumerCommand = &frames;
        require(!SelectSecondConsumerFrame(frames, 101), "FG-owned frame admitted or replaced");
        frames[1].consumerCommand = nullptr;
        frames[2].number = 101;
        require(!SelectSecondConsumerFrame(frames, 101), "Ambiguous same-frame producers admitted");
        frames[2].number = 102;
        require(SelectSecondConsumerFrame(frames, 101) == &frames[1], "Valid route did not recover");
        std::printf("PACKED_SELECTION_OK checks=%u\n", checks.load());
        packedCaptureContract();
        return 0;
    }
    catch (const std::exception& error)
    {
        std::fprintf(stderr, "%s\n", error.what());
        return 1;
    }
}
