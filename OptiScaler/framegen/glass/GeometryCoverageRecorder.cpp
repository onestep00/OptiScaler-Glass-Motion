#include "pch.h"
#include "GeometryCoverageRecorder.h"
#include "GeometryDrawCapture.h"
#include "GeometryCommands.h"
#include "CyberpunkDraws.h"
#include "DxilVertexHistory.h"
#include <fstream>
#include <mutex>
#include <thread>
#include <chrono>

namespace GlassFg
{
namespace
{
using Microsoft::WRL::ComPtr;
constexpr unsigned SlotCount = 8, MaxInstances = 32;
constexpr unsigned MaxCaptures = 64;
constexpr UINT64 Budget = 256ull * 1024 * 1024;
struct Slot
{
    enum Phase { Empty, Requested, Building, Ready, Reserved, Recording, Recorded, Saved, Failed } phase = Empty;
    std::shared_ptr<const GeometryPipelineEntry> lease;
    ComPtr<ID3D12PipelineState> pipeline;
    ComPtr<ID3D12Resource> bits, readback, map, constants, dummy, zeros;
    ComPtr<ID3D12Fence> fence;
    ComPtr<ID3D12CommandQueue> initialQueue;
    ComPtr<ID3D12CommandAllocator> initialAllocator;
    ComPtr<ID3D12GraphicsCommandList> initialCommand;
    std::array<GeometryDrawIdentity, MaxInstances> identities {};
    UINT width = 0, height = 0, instances = 0, wordsPerObject = 0, frame = 0, chunk = 0;
    UINT64 bytes = 0, charge = 0;
    unsigned captureIndex = 0;
    void* mapped = nullptr;
    void* constantData = nullptr;
    ID3D12GraphicsCommandList* command = nullptr;
    bool submitted = false, discarded = false;
    GeometryRasterState raster;
    CyberpunkMeshShape meshShape;
    GeometryIndexedArguments arguments {};
};
void checked(HRESULT result)
{
    if (FAILED(result))
        throw std::runtime_error("D3D12 coverage resource failure");
}
void transition(ID3D12GraphicsCommandList* command, ID3D12Resource* resource,
                D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after)
{
    D3D12_RESOURCE_BARRIER barrier {};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition = { resource, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, before, after };
    command->ResourceBarrier(1, &barrier);
}
struct Recorder final : GeometryDrawCaptureOwner
{
    ComPtr<ID3D12Device> device;
    std::filesystem::path compiler, output;
    std::mutex mutex;
    std::array<Slot, SlotCount> slots;
    struct Selection
    {
        const GeometryPipelineEntry* pipeline = nullptr;
        UINT width = 0, height = 0, instances = 0;
    };
    std::array<Selection, MaxCaptures> selections {};
    unsigned selectionCount = 0;
    UINT64 allocated = 0;
    std::atomic<bool> accepting = true;
    UINT64 firstRequest = 0;
    HANDLE startEvent = nullptr, stopEvent = nullptr;
    std::filesystem::path baseOutput;
    Recorder(ID3D12Device* d, std::filesystem::path c, std::filesystem::path o)
        : device(d), compiler(std::move(c)), output(std::move(o)), baseOutput(output)
    {
        const auto prefix = L"Local\\OptiScaler.Glass.Capture." + std::to_wstring(GetCurrentProcessId());
        startEvent = CreateEventW(nullptr, FALSE, FALSE, (prefix + L".Start").c_str());
        stopEvent = CreateEventW(nullptr, FALSE, FALSE, (prefix + L".Stop").c_str());
        if (!startEvent || !stopEvent)
        {
            if (startEvent) CloseHandle(startEvent);
            if (stopEvent) CloseHandle(stopEvent);
            startEvent = stopEvent = nullptr;
            throw std::runtime_error("Coverage request events unavailable");
        }
    }
    ~Recorder()
    {
        if (startEvent) CloseHandle(startEvent);
        if (stopEvent) CloseHandle(stopEvent);
    }
    ComPtr<ID3D12Resource> buffer(UINT64 bytes, D3D12_HEAP_TYPE type, D3D12_RESOURCE_STATES state, bool uav = false)
    {
        D3D12_HEAP_PROPERTIES heap {};
        heap.Type = type;
        D3D12_RESOURCE_DESC desc {};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        desc.Width = bytes;
        desc.Height = 1;
        desc.DepthOrArraySize = desc.MipLevels = 1;
        desc.SampleDesc.Count = 1;
        desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        desc.Flags = uav ? D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS : D3D12_RESOURCE_FLAG_NONE;
        ComPtr<ID3D12Resource> result;
        checked(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, state, nullptr,
                                                IID_PPV_ARGS(&result)));
        return result;
    }
    void build(Slot& slot)
    {
        GeometryCompiler dxc(compiler);
        std::string error;
        if (FAILED(dxc.createCoverage(device.Get(), *slot.lease->root, slot.lease->description, slot.pipeline, error)))
            throw std::runtime_error(error);
        slot.bits = buffer(slot.bytes, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_COPY_DEST, true);
        slot.readback = buffer(slot.bytes, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST);
        slot.zeros = buffer(slot.bytes, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
        slot.map = buffer(MaxInstances * sizeof(GeometryInstance), D3D12_HEAP_TYPE_UPLOAD,
                          D3D12_RESOURCE_STATE_GENERIC_READ);
        slot.constants = buffer(256, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
        // Coverage mappings have zero vertices: neither previous nor current
        // history is accessed. Distinct legal bindings still satisfy the root.
        slot.dummy = buffer(64, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, true);
        D3D12_RANGE noRead { 0, 0 };
        void* zeroData = nullptr;
        checked(slot.zeros->Map(0, &noRead, &zeroData));
        memset(zeroData, 0, SIZE_T(slot.bytes));
        slot.zeros->Unmap(0, nullptr);
        checked(slot.map->Map(0, &noRead, &slot.mapped));
        checked(slot.constants->Map(0, &noRead, &slot.constantData));
        auto& queue = slot.initialQueue;
        auto& allocator = slot.initialAllocator;
        auto& command = slot.initialCommand;
        D3D12_COMMAND_QUEUE_DESC qd {};
        checked(device->CreateCommandQueue(&qd, IID_PPV_ARGS(&queue)));
        checked(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)));
        checked(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr,
                                         IID_PPV_ARGS(&command)));
        checked(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&slot.fence)));
        command->CopyBufferRegion(slot.bits.Get(), 0, slot.zeros.Get(), 0, slot.bytes);
        transition(command.Get(), slot.bits.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        checked(command->Close());
        ID3D12CommandList* list[] { command.Get() };
        queue->ExecuteCommandLists(1, list);
        checked(queue->Signal(slot.fence.Get(), 1));
        // Worker-only initialization. Never wait in a game draw/submission.
        while (slot.fence->GetCompletedValue() < 1)
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        checked(device->GetDeviceRemovedReason());
        slot.zeros.Reset();
    }
    bool prepare(ID3D12GraphicsCommandList* command, const GeometryDrawView& draw,
                 const GeometryIndexedArguments& args, const std::shared_ptr<const GeometryPipelineEntry>& lease,
                 const GraphicsRootBindings&, GeometryPreparedDraw& prepared) noexcept override
    {
        if (!accepting.load(std::memory_order_relaxed))
            return false;
        const auto* raster = ReadGeometryRasterState(command);
        if (!raster || !raster->usable() || !raster->depth.ptr || !args.instances || args.instances > MaxInstances ||
            raster->viewport.TopLeftX != 0 || raster->viewport.TopLeftY != 0 ||
            raster->viewport.Width > 32768 || raster->viewport.Height > 32768 ||
            raster->viewport.Width < 1 || raster->viewport.Height < 1)
            return false;
        const auto width = UINT(raster->viewport.Width), height = UINT(raster->viewport.Height);
        if (float(width) != raster->viewport.Width || float(height) != raster->viewport.Height)
            return false;
        std::unique_lock lock(mutex, std::try_to_lock);
        if (!lock)
            return false;
        if (!accepting.load(std::memory_order_relaxed))
            return false;
        for (auto& slot : slots)
            if (slot.phase == Slot::Ready && slot.lease == lease && slot.width == width && slot.height == height &&
                slot.instances == args.instances)
            {
                auto* mapping = static_cast<GeometryInstance*>(slot.mapped);
                memset(mapping, 0, MaxInstances * sizeof(GeometryInstance));
                bool any = false;
                for (const auto& object : draw.objects)
                {
                    if (!object.identity || object.count != 1 || object.first >= args.instances)
                        continue;
                    const UINT i = object.first, status = i * slot.wordsPerObject;
                    mapping[i] = { 0, 0, 0, object.identity.generation, 0, 0, width, height,
                                   (status + 1) * 32, width, UINT(slot.bytes * 8), status };
                    if (!mapping[i].validCoverage(slot.bytes / 4))
                        return false;
                    slot.identities[i] = object.identity;
                    any = true;
                }
                if (!any)
                    return false;
                slot.command = command;
                slot.frame = draw.frame;
                slot.chunk = draw.chunk;
                slot.raster = *raster;
                // Only for the selected diagnostic draw, not every observed
                // draw. Copy borrowed engine data before leaving its scope.
                slot.meshShape = ReadCyberpunkMeshShape(draw);
                slot.arguments = args;
                const MaterialCaptureConstants constants { 0, 0, 1.f / width, 1.f / height, 0, 0, draw.frame, 0,
                                                           0, 0, width, height, 0, width, 1, 0 };
                memcpy(slot.constantData, &constants, sizeof(constants));
                prepared = { slot.pipeline.Get(), { 0, args.instances, 0, 0, args.instances, 0, draw.frame, 0 },
                    slot.map->GetGPUVirtualAddress(), slot.dummy->GetGPUVirtualAddress() + 32,
                    slot.constants->GetGPUVirtualAddress(), slot.bits->GetGPUVirtualAddress(),
                    slot.map->GetGPUVirtualAddress() };
                slot.phase = Slot::Reserved;
                return true;
            }
        // A single pending request for this shape/pipeline is enough. Building
        // happens off-thread; the requesting draw retains its original path.
        for (const auto& slot : slots)
            if (slot.phase != Slot::Empty && slot.lease == lease && slot.width == width && slot.height == height &&
                slot.instances == args.instances)
                return false;
        // Sampling only: remember already requested shapes while completed
        // slots retire. Addresses never authorize object identity or history.
        for (unsigned i = 0; i < selectionCount; ++i)
            if (selections[i].pipeline == lease.get() && selections[i].width == width &&
                selections[i].height == height && selections[i].instances == args.instances)
                return false;
        if (selectionCount == MaxCaptures)
            return false;
        for (auto& slot : slots)
            if (slot.phase == Slot::Empty)
            {
                const UINT words = UINT(1 + (UINT64(width) * height + 31) / 32);
                const UINT64 bytes = UINT64(words) * args.instances * 4;
                const UINT64 charge = ((bytes + 65535) & ~UINT64(65535)) * 3 + 1024 * 1024;
                if (allocated + charge > Budget || bytes * 8 > UINT32_MAX)
                    return false;
                allocated += charge;
                slot.charge = charge;
                slot.captureIndex = selectionCount;
                selections[selectionCount++] = { lease.get(), width, height, args.instances };
                if (!firstRequest)
                    firstRequest = GetTickCount64();
                slot.lease = lease;
                slot.width = width;
                slot.height = height;
                slot.instances = args.instances;
                slot.wordsPerObject = words;
                slot.bytes = bytes;
                slot.phase = Slot::Requested;
                break;
            }
        return false;
    }
    void finish(ID3D12GraphicsCommandList* command, bool recorded) noexcept override
    {
        Slot* selected = nullptr;
        {
            std::lock_guard lock(mutex);
            for (auto& slot : slots)
                if (slot.phase == Slot::Reserved && slot.command == command)
                {
                    slot.phase = recorded ? Slot::Recording : Slot::Failed;
                    selected = recorded ? &slot : nullptr;
                    break;
                }
        }
        if (!selected)
            return;
        // No owner lock across D3D12: the host's submission observer can hold
        // its own lock before calling submitted(), which takes this mutex.
        transition(command, selected->bits.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                   D3D12_RESOURCE_STATE_COPY_SOURCE);
        command->CopyBufferRegion(selected->readback.Get(), 0, selected->bits.Get(), 0, selected->bytes);
        transition(command, selected->bits.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE,
                   D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        std::lock_guard lock(mutex);
        selected->phase = Slot::Recorded;
    }
    void submitted(ID3D12CommandQueue* queue, UINT count, ID3D12CommandList* const* lists) noexcept override
    {
        std::lock_guard lock(mutex);
        for (auto& slot : slots)
            if (slot.phase == Slot::Recorded && !slot.discarded)
                for (UINT i = 0; i < count; ++i)
                    if (lists[i] == slot.command)
                    {
                        if (slot.submitted || FAILED(queue->Signal(slot.fence.Get(), 2)))
                            slot.phase = Slot::Failed;
                        slot.submitted = true;
                    }
    }
    void discarded(ID3D12GraphicsCommandList* command) noexcept override
    {
        std::lock_guard lock(mutex);
        for (auto& slot : slots)
            if (slot.command == command && slot.phase == Slot::Recorded)
                slot.discarded = true;
    }
    void save(Slot& slot)
    {
        void* data = nullptr;
        D3D12_RANGE range { 0, SIZE_T(slot.bytes) };
        checked(slot.readback->Map(0, &range, &data));
        const auto stem = output / ("objects-" + std::to_string(slot.captureIndex));
        std::ofstream binary(stem.string() + ".bin", std::ios::binary);
        binary.write(static_cast<const char*>(data), std::streamsize(slot.bytes));
        const bool okay = bool(binary);
        D3D12_RANGE noWrite { 0, 0 };
        slot.readback->Unmap(0, &noWrite);
        if (!okay)
            throw std::runtime_error("Coverage file write failed");
        std::ofstream meta(stem.string() + ".csv");
        meta << "frame,chunk,width,height,instance,proxy,mesh,slot,generation,status_word,first_bit,motion_produced\n";
        for (UINT i = 0; i < slot.instances; ++i)
        {
            const auto& id = slot.identities[i];
            meta << slot.frame << ',' << slot.chunk << ',' << slot.width << ',' << slot.height << ',' << i << ','
                 << id.proxy << ',' << id.mesh << ',' << id.slot << ',' << id.generation << ','
                 << i * slot.wordsPerObject << ',' << (i * slot.wordsPerObject + 1) * 32 << ",0\n";
        }
        if (!meta)
            throw std::runtime_error("Coverage metadata write failed");
        meta.close();
        binary.close();
        std::ofstream draw(stem.string() + ".draw");
        const auto& r = slot.raster;
        const auto& m = slot.meshShape;
        draw << "engine_mesh_shape=" << bool(m) << "\nvertices=" << m.vertices
             << "\nindices=" << m.indices << "\nchunk_address=" << m.chunkAddress
             << "\nvertex_buffer=" << m.vertexBuffer << "\nindex_buffer=" << m.indexBuffer
             << "\nindex_offset=" << m.indexOffset << "\nindex_type=" << unsigned(m.indexType)
             << "\nvertex_factory=" << unsigned(m.vertexFactory) << "\nstreams=" << m.streams;
        for (unsigned i = 0; i < m.streamOffsets.size(); ++i)
            draw << "\nstream_offset_" << i << '=' << m.streamOffsets[i];
        draw << "\ndraw_indices=" << slot.arguments.indices << "\ndraw_instances=" << slot.arguments.instances
             << "\nstart_index=" << slot.arguments.startIndex << "\nbase_vertex=" << slot.arguments.baseVertex
             << "\nstart_instance=" << slot.arguments.startInstance
             << "\nviewport=" << r.viewport.TopLeftX << ',' << r.viewport.TopLeftY << ','
             << r.viewport.Width << ',' << r.viewport.Height << ',' << r.viewport.MinDepth << ',' << r.viewport.MaxDepth
             << "\nscissor=" << r.scissor.left << ',' << r.scissor.top << ',' << r.scissor.right << ',' << r.scissor.bottom
             << "\ndsv_handle=" << r.depth.ptr << "\nrtv_count=" << r.targetCount;
        for (unsigned i = 0; i < r.targetCount; ++i)
            draw << "\nrtv_handle_" << i << '=' << r.targets[i].ptr;
        draw << "\nview_identity_proven=0\ntopology_history_proven=0\nmotion_produced=0\n";
        draw.close();
        if (!draw || !meta || !binary)
            throw std::runtime_error("Coverage provenance file write failed");
        std::ofstream done(stem.string() + ".done");
        done << "coverage_only=1 motion_produced=0 gpu_complete=1 recording_discarded=1\n";
    }
    void runSession()
    {
        const auto began = GetTickCount64();
        UINT64 stopped = 0;
        for (;;)
        {
            if (WaitForSingleObject(startEvent, 0) == WAIT_OBJECT_0)
                std::ofstream(output / "request-busy.txt") << "capture_active=1\n";
            if (!stopped && (WaitForSingleObject(stopEvent, 0) == WAIT_OBJECT_0 ||
                             GetTickCount64() - began > 30000))
                stopped = GetTickCount64();
            // Never release COM resources while holding the owner mutex.
            std::array<Slot, SlotCount> unused;
            {
                std::lock_guard lock(mutex);
                if (stopped)
                {
                    accepting.store(false, std::memory_order_relaxed);
                    bool outstanding = false;
                    for (unsigned i = 0; i < slots.size(); ++i)
                    {
                        auto& slot = slots[i];
                        if (slot.phase == Slot::Requested || slot.phase == Slot::Ready)
                        {
                            allocated -= slot.charge;
                            unused[i] = std::move(slot);
                            slot = Slot {};
                        }
                        outstanding |= slot.phase == Slot::Building || slot.phase == Slot::Reserved ||
                                       slot.phase == Slot::Recording || slot.phase == Slot::Recorded;
                    }
                    // Failure to observe completion/discard never licenses a
                    // readback or release. End diagnostics but retain resources.
                    if (!outstanding || GetTickCount64() - stopped > 120000)
                        return;
                }
            }
            for (unsigned i = 0; i < slots.size(); ++i)
            {
                bool buildNow = false, saveNow = false;
                {
                    std::lock_guard lock(mutex);
                    auto& slot = slots[i];
                    if (slot.phase == Slot::Requested)
                    {
                        slot.phase = Slot::Building;
                        buildNow = true;
                    }
                    if (slot.phase == Slot::Recorded && slot.submitted && slot.discarded)
                    {
                        const auto completed = slot.fence->GetCompletedValue();
                        if (completed == UINT64_MAX)
                            slot.phase = Slot::Failed;
                        else if (completed >= 2)
                        {
                            slot.phase = Slot::Saved;
                            saveNow = true;
                        }
                    }
                }
                if (buildNow || saveNow)
                    try
                    {
                        if (buildNow)
                            build(slots[i]);
                        else
                            save(slots[i]);
                        if (buildNow)
                        {
                            std::lock_guard lock(mutex);
                            slots[i].phase = Slot::Ready;
                        }
                        else
                        {
                            // save was admitted only after submission, GPU
                            // completion and recording discard. Release outside
                            // our mutex: COM destruction can enter host hooks.
                            Slot retired;
                            {
                                std::lock_guard lock(mutex);
                                allocated -= slots[i].charge;
                                retired = std::move(slots[i]);
                                slots[i] = Slot {};
                            }
                        }
                    }
                    catch (const std::exception& error)
                    {
                        std::lock_guard lock(mutex);
                        slots[i].phase = Slot::Failed;
                        std::ofstream log(output / "errors.txt", std::ios::app);
                        log << i << ": " << error.what() << '\n';
                    }
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    }
    void run() noexcept
    {
        try
        {
            unsigned request = 1;
            for (;;)
            {
                std::ofstream(output / "active.txt") << "request=" << request << '\n';
                runSession();
                std::ofstream(output / "idle.txt") << "request=" << request << " accepting=0\n";
                // Idle diagnostics perform no polling, GPU work or file reads.
                for (;;)
                {
                    if (WaitForSingleObject(startEvent, INFINITE) != WAIT_OBJECT_0)
                        return;
                    bool pending = false;
                    {
                        std::lock_guard lock(mutex);
                        for (const auto& slot : slots)
                            pending |= slot.phase == Slot::Building || slot.phase == Slot::Reserved ||
                                       slot.phase == Slot::Recording || slot.phase == Slot::Recorded;
                    }
                    if (pending)
                    {
                        std::ofstream(output / "request-blocked.txt") << "previous_recording_unresolved=1\n";
                        continue;
                    }
                    const auto next = baseOutput / ("request-" + std::to_string(request + 1));
                    if (!std::filesystem::create_directory(next))
                    {
                        std::ofstream(output / "request-blocked.txt") << "output_already_exists=1\n";
                        continue;
                    }
                    output = next;
                    ++request;
                    ResetEvent(stopEvent);
                    {
                        std::lock_guard lock(mutex);
                        selectionCount = 0;
                        firstRequest = 0;
                        accepting.store(true, std::memory_order_relaxed);
                    }
                    break;
                }
            }
        }
        catch (...)
        {
            accepting.store(false, std::memory_order_relaxed);
        }
    }
};
} // namespace
void StartGeometryCoverageRecorder(ID3D12Device* device, const std::filesystem::path& compiler,
                                   const std::filesystem::path& request) noexcept
{
    try
    {
        if (!device || !std::filesystem::is_regular_file(request))
            return;
        std::ifstream input(request);
        std::string path;
        std::getline(input, path);
        std::filesystem::path output(path);
        if (!input || !output.is_absolute())
            return;
        std::filesystem::create_directories(output);
        auto* recorder = new Recorder(device, compiler, output);
        if (!RegisterGeometryDrawCapture(recorder))
        {
            delete recorder;
            return;
        }
        std::thread([recorder] { recorder->run(); }).detach();
    }
    catch (...) {}
}
} // namespace GlassFg
