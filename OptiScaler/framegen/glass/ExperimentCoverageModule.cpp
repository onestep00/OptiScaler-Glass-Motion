// Build as a separate DLL, never into the resident OptiScaler binary.
#include "ExperimentCaptureAbi.h"
#include "ExperimentCensusLog.h"
#include "ExperimentCaptureSelection.h"
#include "GeometryPipeline.h"
#include "DxilVertexHistory.h"
#include <windows.h>
#include <array>
#include <atomic>
#include <condition_variable>
#include <cstring>
#include <fstream>
#include <mutex>
#include <thread>
#include <sstream>
#include <utility>

namespace
{
using Microsoft::WRL::ComPtr;
using namespace GlassFg;
HMODULE moduleIdentity = nullptr;
#ifdef GLASS_CAPTURE_NATIVE_PIXELS
constexpr bool NativePixelDiagnostic = true;
#else
constexpr bool NativePixelDiagnostic = false;
#endif
constexpr unsigned SlotCount = 8, MaxInstances = 64, MaxCaptures = NativePixelDiagnostic ? 8 : 64;
constexpr uint64_t Budget = (NativePixelDiagnostic ? 512ull : 256ull) * 1024 * 1024;
#ifdef GLASS_CAPTURE_VERTEX_COVERAGE
constexpr bool VertexCoverageDiagnostic = true;
#else
constexpr bool VertexCoverageDiagnostic = false;
#endif
#ifdef GLASS_CAPTURE_NATIVE_PAIR
constexpr bool NativePairDiagnostic = true;
static_assert(!VertexCoverageDiagnostic, "Native pair and coverage use different record layouts");
#else
constexpr bool NativePairDiagnostic = false;
#endif
constexpr unsigned VertexRecordBytes = NativePairDiagnostic ? 64 : 32;
#if defined(GLASS_CAPTURE_VERTEX_OUTPUTS) || defined(GLASS_CAPTURE_VERTEX_COVERAGE) || defined(GLASS_CAPTURE_NATIVE_PAIR)
constexpr bool VertexOutputDiagnostic = true;
#else
constexpr bool VertexOutputDiagnostic = false;
#endif
#ifdef GLASS_CAPTURE_DRAW_INSTANCES
constexpr bool DrawInstanceDiagnostic = true;
#else
constexpr bool DrawInstanceDiagnostic = false;
#endif
void check(HRESULT result) { if (FAILED(result)) throw std::runtime_error("Coverage GPU operation failed"); }
void barrier(ID3D12GraphicsCommandList* command, ID3D12Resource* resource,
             D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after)
{
    D3D12_RESOURCE_BARRIER value {}; value.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    value.Transition = { resource, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, before, after };
    command->ResourceBarrier(1, &value);
}
struct Token
{
    GlassExperimentPipelineAccess access {};
    void* value = nullptr;
    Token() = default;
    Token(const Token&) = delete;
    Token& operator=(const Token&) = delete;
    Token(Token&& other) noexcept : access(other.access), value(std::exchange(other.value, nullptr)) {}
    Token& operator=(Token&& other) noexcept
    {
        if (value) access.release(value);
        access = other.access; value = std::exchange(other.value, nullptr); return *this;
    }
    ~Token() { if (value) access.release(value); }
};
struct Slot
{
    enum State { Empty, Requested, Building, Ready, Reserved, Recorded, Retired, Failed } state = Empty;
    Token source;
    GlassExperimentPipelineView view {};
    ComPtr<ID3D12PipelineState> pipeline;
    ComPtr<ID3D12Resource> bits, readback, map, constants, dummy, zeros;
    ComPtr<ID3D12CommandQueue> initQueue;
    ComPtr<ID3D12CommandAllocator> initAllocator;
    ComPtr<ID3D12GraphicsCommandList> initCommand;
    ComPtr<ID3D12Fence> initFence;
    bool initSubmitted = false, initComplete = false;
    void* mapping = nullptr;
    void* constantData = nullptr;
    uint64_t bytes = 0, vertexBytes = 0, charge = 0, job = 0, recording = 0, frame = 0, retiredAtFrame = 0;
    uint64_t requestedMesh = 0;
    unsigned requestedChunk = 0;
    unsigned index = 0, width = 0, height = 0, left = 0, top = 0, instances = 0, words = 0;
    GlassExperimentDrawInput draw {};
    GlassExperimentMesh mesh {};
    std::array<GlassExperimentObject, MaxInstances> objects {};
    std::array<GlassExperimentTarget, 9> targets {};
};
class Coverage
{
    ComPtr<ID3D12Device> device;
    std::filesystem::path compiler, output;
    std::mutex mutex;
    std::condition_variable changed;
    bool stopping = false;
    std::thread worker;
    ExperimentCensusLog census;
    ExperimentCaptureSelection selection;
    VertexClipPair clipPair {};
    uint64_t nativePipeline = 0;
    uint64_t selectionAccepted = 0, selectionRejected = 0;
    std::array<Slot, SlotCount> slots;
    struct Selection { uint64_t pipeline; unsigned width, height, instances; uint64_t mesh; unsigned chunk; };
    std::array<Selection, MaxCaptures> selected {};
    unsigned selections = 0;
    uint64_t allocated = 0;
    uint64_t lastVertexFrame = 0;
    std::atomic<uint64_t> latestObservedFrame { 0 };
    ComPtr<ID3D12Resource> buffer(uint64_t bytes, D3D12_HEAP_TYPE type, D3D12_RESOURCE_STATES state, bool uav = false)
    {
        D3D12_HEAP_PROPERTIES heap {}; heap.Type = type;
        D3D12_RESOURCE_DESC desc {}; desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        desc.Width = bytes; desc.Height = 1; desc.DepthOrArraySize = desc.MipLevels = 1;
        desc.SampleDesc.Count = 1; desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        desc.Flags = uav ? D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS : D3D12_RESOURCE_FLAG_NONE;
        ComPtr<ID3D12Resource> result;
        check(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, state, nullptr, IID_PPV_ARGS(&result)));
        return result;
    }
    void build(Slot& slot)
    {
        auto& view = slot.view; view.size = sizeof(view);
        if (!slot.source.access.view(slot.source.value, &view) || view.layout != 1 ||
            view.descriptorBytes != sizeof(D3D12_GRAPHICS_PIPELINE_STATE_DESC) || !view.descriptor)
            throw std::runtime_error("Original pipeline unavailable");
        GeometryRoot root;
        root.original = static_cast<ID3D12RootSignature*>(view.originalRoot);
        root.extended = static_cast<ID3D12RootSignature*>(view.extendedRoot);
        root.layout = GeometryLayout::PerInstance; root.dwords = view.dwords;
        GeometryCompiler dxc(compiler);
        std::string error;
        const auto& original = *static_cast<const D3D12_GRAPHICS_PIPELINE_STATE_DESC*>(view.descriptor);
        // Recorded Cyberpunk layout. Capturing these words is not view/history admission.
        const VertexConstantPair cameraWords { 0, 1, 848, 51 };
        const NativeClipInputs nativeInputs {clipPair.currentOutput,clipPair.previousOutput,true};
        const auto compiled = NativePixelDiagnostic
            ? dxc.createNativeMotionCapture(device.Get(), root, original, slot.pipeline, error, nativeInputs)
            : VertexCoverageDiagnostic
            ? dxc.createCoverageAudit(device.Get(), root, original, slot.pipeline, error, &cameraWords)
            : NativePairDiagnostic
            ? dxc.createVertexCapture(device.Get(), root, original, slot.pipeline, error, nullptr, &clipPair)
            : VertexOutputDiagnostic
            ? dxc.createVertexCapture(device.Get(), root, original, slot.pipeline, error, &cameraWords)
            : dxc.createCoverageAudit(device.Get(), root, original, slot.pipeline, error);
        if (FAILED(compiled))
            throw std::runtime_error(error);
        slot.bits = buffer(slot.bytes, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_COPY_DEST, true);
        slot.readback = buffer(slot.bytes, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST);
        slot.zeros = buffer(slot.bytes, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
        slot.map = buffer(MaxInstances * sizeof(GeometryInstance), D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
        slot.constants = buffer(256, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
        slot.dummy = buffer(64, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, true);
        D3D12_RANGE noRead { 0, 0 }; void* zeros = nullptr;
        check(slot.zeros->Map(0, &noRead, &zeros)); memset(zeros, 0, SIZE_T(slot.bytes)); slot.zeros->Unmap(0, nullptr);
        check(slot.map->Map(0, &noRead, &slot.mapping));
        check(slot.constants->Map(0, &noRead, &slot.constantData));
        D3D12_COMMAND_QUEUE_DESC qd {};
        check(device->CreateCommandQueue(&qd, IID_PPV_ARGS(&slot.initQueue)));
        check(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&slot.initAllocator)));
        check(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, slot.initAllocator.Get(), nullptr,
                                        IID_PPV_ARGS(&slot.initCommand)));
        check(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&slot.initFence)));
        slot.initCommand->CopyBufferRegion(slot.bits.Get(), 0, slot.zeros.Get(), 0, slot.bytes);
        barrier(slot.initCommand.Get(), slot.bits.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        check(slot.initCommand->Close());
        ID3D12CommandList* lists[] { slot.initCommand.Get() };
        slot.initSubmitted = true;
        slot.initQueue->ExecuteCommandLists(1, lists);
        check(slot.initQueue->Signal(slot.initFence.Get(), 1));
        const auto until = GetTickCount64() + 10000;
        while (slot.initFence->GetCompletedValue() < 1 && GetTickCount64() < until) Sleep(1);
        check(device->GetDeviceRemovedReason());
        if (slot.initFence->GetCompletedValue() < 1) throw std::runtime_error("Private initialization timeout");
        slot.initComplete = true;
        slot.initCommand.Reset(); // Destroy the private completed recording.
        slot.initAllocator.Reset(); slot.initQueue.Reset(); slot.initFence.Reset();
        if constexpr (!VertexOutputDiagnostic)
            slot.zeros.Reset(); // Vertex snapshots retain a zero previous-input SRV.
    }
    void save(Slot& slot)
    {
        const auto stem = output / ("objects-" + std::to_string(slot.index));
        const auto withSuffix = [&](const char* suffix) { auto path = stem; path += suffix; return path; };
        const auto write = [&](const char* suffix, const void* data, size_t bytes)
        {
            std::ofstream file(withSuffix(suffix), std::ios::binary);
            file.write(static_cast<const char*>(data), std::streamsize(bytes)); file.close();
            if (!file) throw std::runtime_error("Capture output write failed");
        };
        void* data = nullptr; D3D12_RANGE range { 0, SIZE_T(slot.bytes) };
        check(slot.readback->Map(0, &range, &data));
        try
        {
            write(".bin", data, size_t(slot.bytes));
            if constexpr (VertexCoverageDiagnostic)
            {
                write(".vertices.bin", data, size_t(slot.vertexBytes));
                write(".coverage.bin", static_cast<const char*>(data) + slot.vertexBytes,
                      size_t(slot.bytes - slot.vertexBytes));
            }
        }
        catch (...) { D3D12_RANGE noWrite { 0, 0 }; slot.readback->Unmap(0, &noWrite); throw; }
        D3D12_RANGE noWrite { 0, 0 }; slot.readback->Unmap(0, &noWrite);
        const auto& pso = *static_cast<const D3D12_GRAPHICS_PIPELINE_STATE_DESC*>(slot.view.descriptor);
        write(".vs.dxil", pso.VS.pShaderBytecode, pso.VS.BytecodeLength);
        write(".ps.dxil", pso.PS.pShaderBytecode, pso.PS.BytecodeLength);
        write(".targets.bin", slot.targets.data(), sizeof(slot.targets));
        std::ofstream targets(withSuffix(".targets.csv"));
        targets << "binding,kind,handle,heap,revision,resource,address,default_descriptor,null_resource,width,height,"
                   "array_or_depth,mips,resource_format,samples,descriptor_bytes";
        for (unsigned word = 0; word < 8; ++word) targets << ",descriptor_word_" << word;
        targets << '\n';
        unsigned observedTargets = 0;
        for (unsigned i = 0; i < slot.targets.size(); ++i)
        {
            const auto& target = slot.targets[i];
            if (target.size != sizeof(target)) continue;
            ++observedTargets;
            D3D12_RESOURCE_DESC allocation {};
            if (target.allocationBytes == sizeof(allocation)) memcpy(&allocation, target.allocation, sizeof(allocation));
            targets << i << ',' << target.kind << ',' << target.handle << ',' << target.heap << ',' << target.revision << ','
                    << target.resource << ',' << target.address << ',' << target.defaultDescriptor << ',' << target.nullResource << ','
                    << allocation.Width << ',' << allocation.Height << ',' << allocation.DepthOrArraySize << ',' << allocation.MipLevels << ','
                    << allocation.Format << ',' << allocation.SampleDesc.Count << ',' << target.descriptorBytes;
            for (unsigned word = 0; word < 8; ++word)
            {
                uint32_t value; memcpy(&value, target.descriptor + 4 * word, 4); targets << ',' << value;
            }
            targets << '\n';
        }
        targets.close(); if (!targets) throw std::runtime_error("Capture target metadata failed");
        if constexpr (NativePixelDiagnostic)
        {
            std::ofstream meta(withSuffix(".draw"));
            meta << "native_pixel_format=2\nrecord_bytes=32\nfirst_pixel=1\nstatus_word=0\ninvocation_counter_byte=16\nframe=" << slot.frame
                 << "\nwidth=" << slot.width << "\nheight=" << slot.height << "\nleft=" << slot.left << "\ntop=" << slot.top
                 << "\nmesh=" << slot.draw.mesh << "\nchunk=" << slot.draw.chunk << "\npipeline=" << slot.view.identity
                 << "\nproxy=" << slot.objects[0].proxy << "\ncurrent_input=" << clipPair.currentOutput
                 << "\nprevious_input=" << clipPair.previousOutput
                 << "\ncoverage=original_native_depth_tested_draw\ntransmission_is_placeholder=1\nfg_substitution=0\n";
            meta.close(); if(!meta) throw std::runtime_error("Native pixel metadata failed");
            std::ofstream done(withSuffix(".done"));
            done << "native_pixel_capture=1\nbytes=" << slot.bytes << "\nframe=" << slot.frame << '\n';
            done.close(); if(!done) throw std::runtime_error("Native pixel completion failed");
            return;
        }
        if constexpr (VertexOutputDiagnostic)
        {
            std::ofstream csv(withSuffix(".csv"));
            csv << "frame,instance,proxy,mesh,slot,generation,vertices,first_record,chunk,pipeline\n";
            for (unsigned i = 0; i < slot.instances; ++i)
            {
                const auto& object = slot.objects[i];
                csv << slot.frame << ',' << i << ',' << object.proxy << ',' << slot.draw.mesh << ','
                    << object.slot << ',' << object.generation << ',' << slot.mesh.vertices << ','
                    << i * slot.mesh.vertices << ',' << slot.draw.chunk << ',' << slot.view.identity << '\n';
            }
            csv.close(); if (!csv) throw std::runtime_error("Vertex metadata write failed");
            std::ofstream meta(withSuffix(".draw"));
            if constexpr (NativePairDiagnostic)
                meta << "vertex_format=3\nrecord_bytes=64\ncurrent_clip_offset=32\nprevious_clip_offset=48\ncurrent_output="
                     << clipPair.currentOutput << "\nprevious_output=" << clipPair.previousOutput;
            else
                meta << "vertex_format=2\nrecord_bytes=32\nextra_word_offset=24\nextra_cb_space=0\nextra_cb_binding=1\nextra_cb_bytes=848\nextra_cb_row=51";
            meta << "\nframe=" << slot.frame
                 << "\nrecording=" << slot.recording << "\nvertices=" << slot.mesh.vertices
                 << "\nretirement_observed_frame=" << slot.retiredAtFrame
                 << "\ninstances=" << slot.instances << "\nmesh=" << slot.draw.mesh
                 << "\npipeline_identity=" << slot.view.identity
                 << "\ndraw_indices=" << slot.draw.indices << "\ndraw_instances=" << slot.draw.instances
                 << "\nstart_index=" << slot.draw.startIndex << "\nbase_vertex=" << slot.draw.baseVertex
                 << "\nstart_instance=" << slot.draw.startInstance
                 << "\nchunk=" << slot.draw.chunk << "\nvertex_buffer=" << slot.mesh.vertexBuffer
                 << "\nindex_buffer=" << slot.mesh.indexBuffer << "\nindex_offset=" << slot.mesh.indexOffset
                 << "\nvertex_factory=" << slot.mesh.vertexFactory << "\nviewport=";
            for (unsigned i = 0; i < 6; ++i) meta << (i ? "," : "") << slot.draw.viewport[i];
            if constexpr (VertexCoverageDiagnostic)
                meta << "\ncoverage_same_draw=1\ncoverage_byte_offset=" << slot.vertexBytes
                     << "\ncoverage_bytes=" << slot.bytes - slot.vertexBytes
                     << "\ncoverage_words_per_instance=" << slot.words
                     << "\nreference_surviving_first_bit=" << (slot.instances * slot.words + 1) * 32
                     << "\nreference_contributing_first_bit=" << ((slot.instances + 1) * slot.words + 1) * 32
                     << "\ncoverage_width=" << slot.width << "\ncoverage_height=" << slot.height;
            meta << "\nview_identity_proven=0\ntopology_history_proven=0\nmotion_produced=0\n";
            meta.close(); if (!meta) throw std::runtime_error("Vertex provenance write failed");
            const std::string done = std::string("vertex_only=") + (VertexCoverageDiagnostic ? "0" : "1") +
                " motion_produced=0 gpu_complete=1 recording_discarded=1\n";
            write(".done", done.data(), done.size());
            return;
        }
        std::ofstream csv(withSuffix(".csv"));
        csv << "frame,chunk,width,height,left,top,instance,proxy,mesh,slot,generation,status_word,first_bit,motion_produced\n";
        for (unsigned i = 0; i < slot.instances; ++i)
        {
            const auto& object = slot.objects[i];
            csv << slot.frame << ',' << slot.draw.chunk << ',' << slot.width << ',' << slot.height << ','
                << slot.left << ',' << slot.top << ',' << i << ',' << object.proxy << ',' << object.mesh << ','
                << object.slot << ',' << object.generation << ',' << i * slot.words << ','
                << (i * slot.words + 1) * 32 << ",0\n";
        }
        csv.close(); if (!csv) throw std::runtime_error("Capture object metadata failed");
        std::ofstream meta(withSuffix(".draw"));
        meta << "coverage_format=2\nmodule_owned=1\ndraw_instance_diagnostic=" << DrawInstanceDiagnostic
             << "\npersistent_instance_identity_proven=0\ntarget_binding_observations=" << observedTargets
             << "\ntarget_observation_point=OMSetRenderTargets\nrecording_epoch=" << slot.recording
             << "\npipeline_identity=" << slot.view.identity << "\nengine_mesh_shape=" << bool(slot.mesh.chunkAddress)
             << "\nvertices=" << slot.mesh.vertices << "\nindices=" << slot.mesh.indices
             << "\nchunk_address=" << slot.mesh.chunkAddress << "\nvertex_buffer=" << slot.mesh.vertexBuffer
             << "\nindex_buffer=" << slot.mesh.indexBuffer << "\nindex_offset=" << slot.mesh.indexOffset
             << "\nindex_type=" << slot.mesh.indexType << "\nvertex_factory=" << slot.mesh.vertexFactory
             << "\nstreams=" << slot.mesh.streams << "\ndraw_indices=" << slot.draw.indices
             << "\ndraw_instances=" << slot.draw.instances << "\nstart_index=" << slot.draw.startIndex
             << "\nbase_vertex=" << slot.draw.baseVertex << "\nstart_instance=" << slot.draw.startInstance;
        for (unsigned i = 0; i < 5; ++i) meta << "\nstream_offset_" << i << '=' << slot.mesh.streamOffsets[i];
        meta << "\nviewport=";
        for (unsigned i = 0; i < 6; ++i) meta << (i ? "," : "") << slot.draw.viewport[i];
        meta << "\nscissor=";
        for (unsigned i = 0; i < 4; ++i) meta << (i ? "," : "") << slot.draw.scissor[i];
        meta << "\ndsv_handle=" << slot.draw.depthTarget << "\nrtv_count=" << slot.draw.renderTargetCount;
        for (unsigned i = 0; i < slot.draw.renderTargetCount; ++i) meta << "\nrtv_handle_" << i << '=' << slot.draw.renderTargets[i];
        meta << "\nreference_surviving_first_bit=" << (slot.instances * slot.words + 1) * 32
             << "\nreference_contributing_first_bit=" << ((slot.instances + 1) * slot.words + 1) * 32
             << "\nreference_stride=" << slot.width << "\nreference_pixels=" << uint64_t(slot.width) * slot.height
             << "\nreference_same_draw=1\nreference_object_mapping=0\nview_identity_proven=0"
                "\ntopology_history_proven=0\nmotion_produced=0\n";
        meta.close(); if (!meta) throw std::runtime_error("Capture provenance failed");
        const char done[] = "coverage_only=1 motion_produced=0 gpu_complete=1 recording_discarded=1 module_owned=1\n";
        write(".done", done, sizeof(done) - 1);
    }
    void run() noexcept
    {
        for (;;)
        {
            Slot* task = nullptr; bool buildNow = false;
            {
                std::unique_lock lock(mutex);
                changed.wait(lock, [&]
                {
                    for (auto& slot : slots) if (slot.state == Slot::Retired || (!stopping && slot.state == Slot::Requested)) return true;
                    return stopping;
                });
                for (auto& slot : slots) if (slot.state == Slot::Retired) { task = &slot; break; }
                if (!task && !stopping)
                    for (auto& slot : slots) if (slot.state == Slot::Requested)
                    { task = &slot; buildNow = true; slot.state = Slot::Building; break; }
                if (!task) return;
            }
            try
            {
                if (buildNow)
                {
                    build(*task);
                    std::lock_guard lock(mutex); task->state = Slot::Ready;
                }
                else
                {
                    save(*task);
                    Slot releaseOutsideLock;
                    { std::lock_guard lock(mutex); allocated -= task->charge; releaseOutsideLock = std::move(*task); *task = Slot {}; }
                }
            }
            catch (const std::exception& error)
            {
                std::ofstream(output / "errors.txt", std::ios::app) << error.what() << '\n';
                std::lock_guard lock(mutex);
                if (task->initSubmitted && !task->initComplete)
                {
                    // Bounded diagnostic quarantine: never free a timed-out
                    // private initialization's possibly in-flight resources.
                    (void)new Slot(std::move(*task)); *task = Slot {};
                }
                task->state = Slot::Failed;
            }
        }
    }
  public:
    Coverage(ID3D12Device* d, std::filesystem::path c, std::filesystem::path o, ExperimentCaptureSelection select,
             VertexClipPair pair, uint64_t selectedPipeline)
        : device(d), compiler(std::move(c)), output(std::move(o)), selection(select), clipPair(pair), nativePipeline(selectedPipeline)
    {
        if (!d || !compiler.is_absolute() || !std::filesystem::is_regular_file(compiler) || !output.is_absolute() ||
            !std::filesystem::create_directory(output)) throw std::runtime_error("Invalid capture module paths");
        worker = std::thread([this] { run(); });
    }
    ~Coverage()
    {
        { std::lock_guard lock(mutex); stopping = true; }
        changed.notify_one(); if (worker.joinable()) worker.join();
        try { census.save(output); }
        catch (const std::exception& error) { std::ofstream(output / "errors.txt", std::ios::app) << error.what() << '\n'; }
        std::ofstream(output / "selection.status") << "enabled=" << selection.enabled << "\nmatched=" << selectionAccepted
            << "\nrejected=" << selectionRejected << "\nobject_motion_produced=0\n";
    }
    int32_t event(const GlassExperimentEvent& event)
    {
        if (event.kind == GlassExperimentCensus) return census.observe(event);
        if (event.kind != GlassExperimentCapture || event.payloadVersion != GLASS_EXPERIMENT_CAPTURE_VERSION ||
            event.payloadBytes != sizeof(GlassExperimentCaptureInput) || !event.payload) return -1;
        const auto& request = *static_cast<const GlassExperimentCaptureInput*>(event.payload);
        if (request.size != sizeof(request)) return -1;
        if (request.stage != GlassCapturePrepare)
        {
            Slot* found = nullptr;
            { std::lock_guard lock(mutex); for (auto& slot : slots) if (slot.job == request.job && slot.job) { found = &slot; break; } }
            if (!found) return -1;
            if (request.stage == GlassCaptureRecorded)
            {
                if (!request.recorded)
                { std::lock_guard lock(mutex); found->state = Slot::Failed; return 0; }
                auto* command = static_cast<ID3D12GraphicsCommandList*>(request.command);
                if (!command) return -1;
                barrier(command, found->bits.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
                command->CopyBufferRegion(found->readback.Get(), 0, found->bits.Get(), 0, found->bytes);
                barrier(command, found->bits.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
                std::lock_guard lock(mutex); found->state = Slot::Recorded; return 0;
            }
            if (request.stage == GlassCaptureRetired)
            {
                std::lock_guard lock(mutex);
                if (found->state == Slot::Recorded)
                {
                    found->retiredAtFrame = latestObservedFrame.load(std::memory_order_relaxed);
                    found->state = Slot::Retired; changed.notify_one();
                }
                return 0;
            }
            return -1;
        }
        const auto* d = request.draw;
        auto observed = latestObservedFrame.load(std::memory_order_relaxed);
        while (event.frame > observed && !latestObservedFrame.compare_exchange_weak(
                   observed, event.frame, std::memory_order_relaxed)) {}
        if (!d || d->size != sizeof(*d) || !request.output || !event.frame || event.frame > UINT32_MAX ||
            !d->rasterKnown || !d->rootReplayable || !d->depthTarget || d->renderTargetCount > 8 ||
            !d->instances || d->instances > MaxInstances || !d->objectAt || !d->meshShape || d->objectCount > 4096 ||
            !d->pipelineIdentity || !d->pipelineAccess.retain || !d->pipelineAccess.view || !d->pipelineAccess.release)
            return 0;
        if (((NativePairDiagnostic || NativePixelDiagnostic) && d->pipelineIdentity != nativePipeline) ||
            (NativePixelDiagnostic && d->instances != 1) || !selection.matches(*d))
        { ++selectionRejected; return 0; }
        ++selectionAccepted;
        for (unsigned i = 0; i < 4; ++i)
            if (!std::isfinite(d->viewport[i]) || d->viewport[i] < 0 || d->viewport[i] > 32768 ||
                float(unsigned(d->viewport[i])) != d->viewport[i]) return 0;
        const unsigned left = unsigned(d->viewport[0]), top = unsigned(d->viewport[1]);
        const unsigned width = unsigned(d->viewport[2]), height = unsigned(d->viewport[3]);
        if (!width || !height || left + width > 32768 || top + height > 32768) return 0;
        GlassExperimentMesh vertexShape {};
        if constexpr (VertexOutputDiagnostic)
            if (!d->meshShape(d->source, &vertexShape) || !vertexShape.vertices ||
                vertexShape.vertices > 65535 || d->baseVertex || d->startIndex) return 0;
        std::unique_lock lock(mutex, std::try_to_lock);
        if (!lock || stopping) return 0;
        if constexpr (VertexOutputDiagnostic)
            if (!selection.meshOnly && lastVertexFrame == event.frame) return 0;
        if (selection.meshOnly)
            for (const auto& slot : slots)
                if (slot.frame == event.frame && slot.draw.mesh == d->mesh && slot.draw.chunk == d->chunk &&
                    slot.draw.pipelineIdentity == d->pipelineIdentity && slot.draw.startInstance == d->startInstance &&
                    slot.draw.instances == d->instances && slot.draw.recording == d->recording) return 0;
        const bool batchPreparing = selection.meshOnly && std::any_of(slots.begin(), slots.end(), [](const Slot& slot)
            { return slot.state == Slot::Requested || slot.state == Slot::Building; });
        for (auto& slot : slots)
            if (!batchPreparing && slot.state == Slot::Ready && slot.view.identity == d->pipelineIdentity && slot.width == width &&
                slot.height == height && slot.instances == d->instances &&
                (!(NativePixelDiagnostic || selection.meshOnly) || (slot.requestedMesh == d->mesh && slot.requestedChunk == d->chunk)) &&
                (!VertexOutputDiagnostic || slot.mesh.vertices == vertexShape.vertices))
            {
                slot.left = left; slot.top = top; slot.objects = {};
                auto* mapping = static_cast<GeometryInstance*>(slot.mapping);
                memset(mapping, 0, MaxInstances * sizeof(GeometryInstance));
                uint64_t seen = 0;
                for (unsigned i = 0; i < d->objectCount; ++i)
                {
                    GlassExperimentObject object {};
                    if (!d->objectAt(d->source, i, &object) || !object.proxy || object.mesh != d->mesh ||
                        !object.generation || object.count != 1 ||
                        object.first >= d->instances) continue;
                    const auto index = object.first, status = index * slot.words;
                    const uint64_t bit = uint64_t(1) << index;
                    if (seen & bit) { slot.objects[index] = {}; mapping[index] = {}; continue; }
                    seen |= bit;
                    slot.objects[index] = object;
                    mapping[index] = { 0, 0, 0, object.generation, left, top, width, height,
                                       (status + 1) * 32, width, unsigned(slot.bytes * 8), status };
                    if constexpr (!VertexOutputDiagnostic && !NativePixelDiagnostic)
                        if (!mapping[index].validCoverage(slot.bytes / 4)) return -1;
                }
                if constexpr (DrawInstanceDiagnostic && !VertexOutputDiagnostic)
                {
                    // Same-draw ordinal isolation only. Never invent an engine
                    // object identity or authorize previous-vertex history.
                    // Generation 1 is the coverage ABI's local enable sentinel.
                    // Engine metadata in slot.objects remains unchanged.
                    for (unsigned index = 0; index < d->instances; ++index)
                    {
                        const unsigned status = index * slot.words;
                        mapping[index] = { 0, 0, 0, 1, left, top, width, height,
                            (status + 1) * 32, width, unsigned(slot.bytes * 8), status };
                        if (!mapping[index].validCoverage(slot.bytes / 4)) return -1;
                    }
                }
                slot.frame = event.frame; slot.job = request.job; slot.recording = request.recording; slot.draw = *d;
                d->meshShape(d->source, &slot.mesh);
                slot.targets = {};
                if (d->targetAt)
                    for (unsigned i = 0; i < slot.targets.size(); ++i)
                    {
                        slot.targets[i].size = sizeof(GlassExperimentTarget);
                        if (d->targetAt(d->targetSource, i, &slot.targets[i]) != 1) slot.targets[i] = {};
                    }
                slot.draw.source = nullptr; slot.draw.objectAt = nullptr; slot.draw.meshShape = nullptr;
                slot.draw.targetSource = nullptr; slot.draw.targetAt = nullptr;
                slot.draw.bindingSource = nullptr; slot.draw.bindingAt = nullptr;
                slot.draw.pipelineAccess = {}; slot.draw.descriptor = nullptr;
                const unsigned reference = (d->instances * slot.words + 1) * 32;
                const MaterialCaptureConstants constants { float(left), float(top), 1.f / width, 1.f / height,
                    0, 0, unsigned(event.frame), 0, left, top, width, height,
                    reference, width, unsigned(slot.bytes * 8), reference + slot.words * 32 };
                memcpy(slot.constantData, &constants, sizeof(constants));
                const InstanceHistoryConstants history { 0, d->instances, 0, 0, d->instances, 0, unsigned(event.frame), 0 };
                auto& prepared = *request.output; prepared = {}; prepared.size = sizeof(prepared);
                prepared.pipeline = slot.pipeline.Get(); memcpy(prepared.history, &history, sizeof(history));
                prepared.previous = slot.map->GetGPUVirtualAddress(); prepared.current = slot.dummy->GetGPUVirtualAddress() + 32;
                prepared.material = slot.constants->GetGPUVirtualAddress(); prepared.capture = slot.bits->GetGPUVirtualAddress();
                prepared.mapping = slot.map->GetGPUVirtualAddress(); slot.state = Slot::Reserved;
                if constexpr (NativePixelDiagnostic)
                {
                    mapping[0] = {0,0,0,1,left,top,width,height,1,width,unsigned(slot.bytes/32),0};
                    auto nativeConstants=constants;
                    nativeConstants.jitterDeltaX=nativeConstants.jitterDeltaY=0;
                    nativeConstants.base=1; nativeConstants.stride=width;
                    nativeConstants.capacity=unsigned(slot.bytes/32);
                    nativeConstants.reserved=0;
                    memcpy(slot.constantData,&nativeConstants,sizeof(nativeConstants));
                }
                if constexpr (VertexOutputDiagnostic)
                {
                    // Frame-local ordinal storage only. Tags never authorize temporal identity.
                    for (unsigned i = 0; i < d->instances; ++i)
                    {
                        mapping[i] = { i * vertexShape.vertices, vertexShape.vertices, 0, 1 };
                        if constexpr (VertexCoverageDiagnostic)
                        {
                            const unsigned status = i * slot.words;
                            mapping[i] = { i * vertexShape.vertices, vertexShape.vertices, 0, 1,
                                left, top, width, height, (status + 1) * 32, width,
                                unsigned((slot.bytes - slot.vertexBytes) * 8), status };
                        }
                    }
                    const InstanceHistoryConstants vertices { 0, d->instances, unsigned(slot.vertexBytes / VertexRecordBytes),
                        0, d->instances, 0, unsigned(event.frame), 0 };
                    memcpy(prepared.history, &vertices, sizeof(vertices));
                    prepared.previous = slot.zeros->GetGPUVirtualAddress();
                    prepared.current = slot.bits->GetGPUVirtualAddress();
                    if constexpr (VertexCoverageDiagnostic)
                    {
                        prepared.capture = slot.bits->GetGPUVirtualAddress() + slot.vertexBytes;
                        auto combined = constants;
                        combined.capacity = unsigned((slot.bytes - slot.vertexBytes) * 8);
                        memcpy(slot.constantData, &combined, sizeof(combined));
                    }
                    lastVertexFrame = event.frame;
                }
                return 1;
            }
        if constexpr (!VertexOutputDiagnostic)
            for (unsigned i = 0; i < selections; ++i)
                if (selected[i].pipeline == d->pipelineIdentity && selected[i].width == width &&
                    selected[i].height == height && selected[i].instances == d->instances &&
                    (!NativePixelDiagnostic || (selected[i].mesh == d->mesh && selected[i].chunk == d->chunk))) return 0;
        if (selections == MaxCaptures) return 0;
        if (selection.meshOnly)
            for (const auto& slot : slots)
                if (slot.state != Slot::Empty && slot.requestedMesh == d->mesh && slot.requestedChunk == d->chunk &&
                    slot.width == width && slot.height == height && slot.instances == d->instances &&
                    selected[slot.index].pipeline == d->pipelineIdentity) return 0;
        for (auto& slot : slots) if (slot.state == Slot::Empty)
        {
            const unsigned words = unsigned(1 + (uint64_t(width) * height + 31) / 32);
            const uint64_t vertexBytes = uint64_t(vertexShape.vertices) * d->instances * VertexRecordBytes;
            const uint64_t coverageBytes = uint64_t(words) * (d->instances + 2) * 4;
            const uint64_t bytes = NativePixelDiagnostic ? (uint64_t(width)*height+1)*32
                : VertexCoverageDiagnostic ? vertexBytes + coverageBytes
                : VertexOutputDiagnostic ? vertexBytes
                : uint64_t(words) * (d->instances + 2) * 4;
            slot.vertexBytes = VertexOutputDiagnostic ? vertexBytes : 0;
            const uint64_t charge = ((bytes + 65535) & ~uint64_t(65535)) * 3 + 1024 * 1024;
            if (allocated + charge > Budget || bytes * 8 > UINT32_MAX) return 0;
            slot.source.access = d->pipelineAccess;
            slot.source.value = slot.source.access.retain(slot.source.access.source);
            slot.source.access.source = nullptr;
            if (!slot.source.value) return 0;
            slot.width = width; slot.height = height; slot.instances = d->instances; slot.words = words;
            slot.requestedMesh=d->mesh; slot.requestedChunk=d->chunk;
            slot.bytes = bytes; slot.charge = charge; allocated += charge; slot.index = selections;
            if constexpr (VertexOutputDiagnostic) slot.mesh = vertexShape;
            selected[selections++] = { d->pipelineIdentity, width, height, d->instances, d->mesh, d->chunk };
            slot.state = Slot::Requested; changed.notify_one();
            if (selection.meshOnly) break; // Reserve space for other chunks in the same frame.
            if constexpr (!VertexOutputDiagnostic) break;
            if (selections == MaxCaptures) break;
        }
        return 0;
    }
};
int32_t create(const GlassExperimentHost* host, void** context)
{
    *context = nullptr;
    try
    {
        wchar_t path[32768]; const auto length = GetModuleFileNameW(moduleIdentity, path, 32768);
        if (!length || length >= 32768) return -1;
        auto config = std::filesystem::path(path); config.replace_extension(L".config");
        if (std::filesystem::file_size(config) > 262144) return -1;
        std::ifstream file(config); std::string compiler, output, select;
        if (!std::getline(file, compiler) || !std::getline(file, output)) return -1;
        if (!compiler.empty() && compiler.back() == '\r') compiler.pop_back();
        if (!output.empty() && output.back() == '\r') output.pop_back();
        if (compiler.starts_with("\xef\xbb\xbf")) compiler.erase(0, 3);
        if (std::getline(file, select))
        {
            if (!select.empty() && select.back() == '\r') select.pop_back();
        }
        VertexClipPair pair {};
        uint64_t selectedPipeline = 0;
        if constexpr (NativePairDiagnostic || NativePixelDiagnostic)
        {
            std::string line, marker, extra;
            if (!std::getline(file, line)) return -1;
            std::istringstream fields(line);
            uint32_t process = 0;
            if (!(fields >> marker >> process >> selectedPipeline >> pair.currentOutput >> pair.previousOutput) ||
                marker != "clip-pair-v1" || process != GetCurrentProcessId() || !selectedPipeline ||
                (fields >> extra) || pair.currentOutput == pair.previousOutput ||
                pair.currentOutput >= 32 || pair.previousOutput >= 32) return -1;
        }
        if (file.peek() != std::char_traits<char>::eof()) return -1;
        *context = new Coverage(static_cast<ID3D12Device*>(host->device),
            std::filesystem::path(std::u8string(compiler.begin(), compiler.end())),
            std::filesystem::path(std::u8string(output.begin(), output.end())),
            ExperimentCaptureSelection::parse(select, GetCurrentProcessId()), pair, selectedPipeline);
        return 0;
    }
    catch (...) { return -1; }
}
int32_t event(void* context, const GlassExperimentEvent* value)
{
    try { return value ? static_cast<Coverage*>(context)->event(*value) : -1; }
    catch (...) { return -1; }
}
void destroy(void* context) { delete static_cast<Coverage*>(context); }
const GlassExperimentApi api { sizeof(api), GLASS_EXPERIMENT_ABI, GlassExperimentCapture | GlassExperimentCensus, create, event, destroy };
}
extern "C" __declspec(dllexport) const GlassExperimentApi* GlassExperimentQuery() { return &api; }
BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH) moduleIdentity = instance;
    return TRUE;
}
