#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <d3d12.h>
#include "ExperimentCensusAbi.h"
#include <array>
#include <atomic>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <condition_variable>
#include <thread>
#include <sstream>

namespace
{
HMODULE moduleIdentity;
struct Pipeline
{
    uint64_t identity = 0;
    const void* original = nullptr;
    GlassExperimentPipelineAccess access {};
    void* token = nullptr;
    GlassExperimentPipelineView view {};
    ~Pipeline() { if (token) access.release(token); }
};
struct Row
{
    uint64_t sequence = 0, frame = 0, recording = 0, pipeline = 0, mesh = 0;
    uint32_t chunk = 0, count = 0, instances = 0, saved = 0;
    std::array<GlassExperimentBinding, 64> bindings {};
};
class Recorder
{
    static constexpr unsigned Capacity = 2048;
    std::unique_ptr<Row[]> rows = std::make_unique<Row[]>(Capacity);
    std::array<Pipeline, 256> pipelines;
    unsigned count = 0, pipelineCount = 0;
    std::mutex mutex;
    std::atomic<uint64_t> dropped = 0;
    std::filesystem::path output;
    uint64_t prepareIdentity = 0, preparedIdentity = 0;
    unsigned prepareIndex = UINT_MAX;
    bool stopping = false, prepareQueued = false;
    int32_t prepareResult = 0;
    std::condition_variable changed;
    std::thread worker;
    void prepare()
    {
        std::unique_lock lock(mutex);
        changed.wait(lock, [&] { return stopping || prepareQueued; });
        if (stopping) return;
        auto& p = pipelines[prepareIndex];
        lock.unlock();
        const auto result = p.view.requestVertexCapture ? p.view.requestVertexCapture(p.token) : 0;
        lock.lock(); prepareResult = result;
    }
  public:
    uint64_t observedMesh = 0;
    explicit Recorder(std::filesystem::path path, uint64_t preparePipeline, uint64_t mesh = 0)
        : output(std::move(path)), prepareIdentity(preparePipeline), observedMesh(mesh)
    {
        if (!output.is_absolute() || !std::filesystem::create_directory(output))
            throw std::runtime_error("Fresh absolute output directory required");
        if (prepareIdentity) worker = std::thread([this] { prepare(); });
    }
    void stop()
    {
        { std::lock_guard lock(mutex); stopping = true; }
        changed.notify_one(); if (worker.joinable()) worker.join();
    }
    ~Recorder() { stop(); }
    int32_t observe(const GlassExperimentEvent& event)
    {
        if (event.kind != GlassExperimentCensus || event.payloadVersion != GLASS_EXPERIMENT_CENSUS_VERSION ||
            event.payloadBytes != sizeof(GlassExperimentCensusInput) || !event.payload) return -1;
        const auto& input = *static_cast<const GlassExperimentCensusInput*>(event.payload);
        const auto& d = input.draw;
        if (observedMesh && d.mesh != observedMesh) return 0;
        if (input.size != sizeof(input) || d.size != sizeof(d) || !d.bindingAt ||
            !d.pipelineIdentity || !d.pipelineAccess.retain || !d.pipelineAccess.view || !d.pipelineAccess.release)
            return 0;
        std::unique_lock lock(mutex, std::try_to_lock);
        if (!lock || count == Capacity) { ++dropped; return 0; }
        unsigned index = 0;
        while (index < pipelineCount && pipelines[index].identity != d.pipelineIdentity) ++index;
        if (index == pipelineCount)
        {
            if (pipelineCount == pipelines.size()) { ++dropped; return 0; }
            auto& p = pipelines[index];
            auto* token = d.pipelineAccess.retain(d.pipelineAccess.source);
            if (!token) return 0;
            GlassExperimentPipelineView view {}; view.size = sizeof(view);
            if (!d.pipelineAccess.view(token, &view) || !view.descriptor ||
                view.descriptorBytes != sizeof(D3D12_GRAPHICS_PIPELINE_STATE_DESC) ||
                view.rootParameterBytes != sizeof(D3D12_ROOT_PARAMETER1) || view.rootParameterCount > 64 ||
                (view.rootParameterCount && !view.rootParameters))
            { d.pipelineAccess.release(token); return 0; }
            p.identity = d.pipelineIdentity; p.original = d.originalPipeline;
            p.access = d.pipelineAccess; p.access.source = nullptr;
            p.token = token; p.view = view; ++pipelineCount;
            if (prepareIdentity == p.identity && !prepareQueued && view.requestVertexCapture)
            { prepareIndex = index; prepareQueued = true; changed.notify_one(); }
        }
        const auto& p = pipelines[index];
        if (p.view.originalRoot != d.originalRoot) return 0;
        if (prepareQueued && p.view.vertexOnlyCapture && p.view.extendedRoot &&
            p.original == pipelines[prepareIndex].original) preparedIdentity = p.identity;
        auto& row = rows[count++];
        row.sequence = input.sequence; row.frame = event.frame; row.recording = d.recording;
        row.pipeline = p.identity; row.mesh = d.mesh; row.chunk = d.chunk;
        row.count = d.indices; row.instances = d.instances;
        for (unsigned slot = 0; slot < p.view.rootParameterCount; ++slot)
        {
            auto& b = row.bindings[slot]; b.size = sizeof(b);
            if (d.bindingAt(d.bindingSource, slot, &b) != 1) b = {};
            else ++row.saved;
        }
        return 1;
    }
    void save() const
    {
        std::ofstream bindings(output / "bindings.csv"), constants(output / "constants.csv"), layouts(output / "layouts.csv"), draws(output / "draws.csv");
        draws << "sequence,frame,recording,pipeline,mesh,chunk,draw_count,instances,bindings_saved\n";
        bindings << "sequence,frame,recording,pipeline,mesh,chunk,draw_count,instances,slot,type,gpu_address,known_constants\n";
        constants << "sequence,slot,word,value\n";
        layouts << "pipeline,slot,type,visibility,range,register,space,count,offset\n";
        for (unsigned i = 0; i < pipelineCount; ++i)
        {
            const auto& p = pipelines[i];
            const auto& d = *static_cast<const D3D12_GRAPHICS_PIPELINE_STATE_DESC*>(p.view.descriptor);
            if (p.view.serializedRoot && p.view.serializedRootBytes)
            {
                std::ofstream file(output / (std::to_string(p.identity) + ".root.bin"), std::ios::binary);
                file.write(static_cast<const char*>(p.view.serializedRoot), p.view.serializedRootBytes);
                file.close(); if (!file) throw std::runtime_error("Root write failed");
            }
            for (unsigned stage = 0; stage < 2; ++stage)
            {
                const auto code = stage ? d.PS : d.VS;
                std::ofstream file(output / (std::to_string(p.identity) + (stage ? ".ps.dxbc" : ".vs.dxbc")), std::ios::binary);
                file.write(static_cast<const char*>(code.pShaderBytecode), code.BytecodeLength);
                file.close(); if (!file) throw std::runtime_error("Shader write failed");
            }
            const auto* params = static_cast<const D3D12_ROOT_PARAMETER1*>(p.view.rootParameters);
            for (unsigned slot = 0; slot < p.view.rootParameterCount; ++slot)
            {
                const auto& v = params[slot];
                const auto prefix = [&] { layouts << p.identity << ',' << slot << ',' << v.ParameterType << ',' << v.ShaderVisibility << ','; };
                if (v.ParameterType == D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE)
                    for (unsigned j = 0; j < v.DescriptorTable.NumDescriptorRanges; ++j)
                    {
                        const auto& r = v.DescriptorTable.pDescriptorRanges[j]; prefix();
                        layouts << r.RangeType << ',' << r.BaseShaderRegister << ',' << r.RegisterSpace << ','
                                << r.NumDescriptors << ',' << r.OffsetInDescriptorsFromTableStart << '\n';
                    }
                else if (v.ParameterType == D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS)
                { prefix(); layouts << "-1," << v.Constants.ShaderRegister << ',' << v.Constants.RegisterSpace << ',' << v.Constants.Num32BitValues << ",0\n"; }
                else
                { prefix(); layouts << "-1," << v.Descriptor.ShaderRegister << ',' << v.Descriptor.RegisterSpace << ",1,0\n"; }
            }
        }
        for (unsigned i = 0; i < count; ++i)
        {
            const auto& r = rows[i];
            draws << r.sequence << ',' << r.frame << ',' << r.recording << ',' << r.pipeline << ',' << r.mesh << ','
                  << r.chunk << ',' << r.count << ',' << r.instances << ',' << r.saved << '\n';
            for (unsigned slot = 0; slot < r.bindings.size(); ++slot)
            {
                const auto& b = r.bindings[slot]; if (!b.size) continue;
                bindings << r.sequence << ',' << r.frame << ',' << r.recording << ',' << r.pipeline << ',' << r.mesh << ','
                         << r.chunk << ',' << r.count << ',' << r.instances << ',' << slot << ',' << b.type << ',' << b.address << ',' << b.knownConstants << '\n';
                for (unsigned word = 0; word < 64; ++word)
                    if (b.knownConstants & (uint64_t {1} << word)) constants << r.sequence << ',' << slot << ',' << word << ',' << b.constants[word] << '\n';
            }
        }
        bindings.close(); constants.close(); layouts.close(); draws.close();
        if (!bindings || !constants || !layouts || !draws) throw std::runtime_error("Binding write failed");
        std::ofstream done(output / "bindings.done");
        done << "format=1\nrows=" << count << "\npipelines=" << pipelineCount << "\ndropped=" << dropped.load()
             << "\nrow_storage_bytes=" << Capacity * sizeof(Row)
             << "\nprepare_selected=" << prepareIdentity << "\nprepare_queued=" << prepareQueued
             << "\nprepare_request_result=" << prepareResult << "\nprepared_pipeline_observed=" << preparedIdentity
             << "\norder=cpu_observation\nframe_zero=unknown\ngpu_buffer_copies=0\nprevious_transform_verified=0\n";
        done.close(); if (!done) throw std::runtime_error("Completion write failed");
    }
};
int32_t create(const GlassExperimentHost* host, void** context)
{
    if (!context) return -1; *context = nullptr;
    if (!host || host->size != sizeof(*host) || host->abi != GLASS_EXPERIMENT_ABI ||
        !(host->capabilities & GlassExperimentCensus)) return -1;
    try
    {
        wchar_t path[32768]; const auto length = GetModuleFileNameW(moduleIdentity, path, 32768);
        if (!length || length >= 32768) return -1;
        auto config = std::filesystem::path(path); config.replace_extension(L".config");
        if (std::filesystem::file_size(config) > 32768) return -1;
        std::ifstream input(config); std::string output;
        if (!std::getline(input, output)) return -1;
        if (!output.empty() && output.back() == '\r') output.pop_back();
        uint64_t prepareIdentity = 0;
        uint64_t observedMesh = 0;
        if (input.peek() != std::char_traits<char>::eof())
        {
            std::string line, format; uint32_t process = 0;
            if (!std::getline(input, line)) return -1;
            std::istringstream selection(line);
            uint64_t identity = 0;
            if (!(selection >> format >> process >> identity) ||
                process != GetCurrentProcessId() || !identity) return -1;
            if (format == "prepare-vertex-v1")
            {
                prepareIdentity = identity;
                selection >> std::ws;
                if (!selection.eof() && (!(selection >> observedMesh) || !observedMesh)) return -1;
            }
            else if (format == "observe-mesh-v1") observedMesh = identity;
            else return -1;
            selection >> std::ws;
            if (!selection.eof() || input.peek() != std::char_traits<char>::eof()) return -1;
        }
        *context = new Recorder(std::filesystem::path(std::u8string(output.begin(), output.end())), prepareIdentity, observedMesh);
        return 0;
    }
    catch (...) { return -1; }
}
int32_t event(void* context, const GlassExperimentEvent* value)
{
    try { return context && value ? static_cast<Recorder*>(context)->observe(*value) : -1; }
    catch (...) { return -1; }
}
void destroy(void* context)
{
    std::unique_ptr<Recorder> recorder(static_cast<Recorder*>(context));
    try { if (recorder) { recorder->stop(); recorder->save(); } } catch (...) { /* No completion marker on failure. */ }
}
const GlassExperimentApi api { sizeof(api), GLASS_EXPERIMENT_ABI, GlassExperimentCensus, create, event, destroy };
}
extern "C" __declspec(dllexport) const GlassExperimentApi* GlassExperimentQuery() { return &api; }
BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH) moduleIdentity = instance;
    return TRUE;
}
