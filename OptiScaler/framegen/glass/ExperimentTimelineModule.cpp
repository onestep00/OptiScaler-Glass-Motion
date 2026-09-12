// Replaceable CPU diagnostic. Retains immutable pipeline inputs only; no GPU
// resource retention, readback, injected rendering or inferred object identity.
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <d3d12.h>
#include "ExperimentCensusAbi.h"
#include "ExperimentSourceAbi.h"
#include <array>
#include <atomic>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <stdexcept>

namespace
{
HMODULE timelineModule = nullptr;
struct TimelineRow
{
    uint64_t sequence, frame, tick, recording, pipeline, command, mesh;
    uint32_t chunk, operation, indices, instances, objectFirst, objectCount, objectDeclared, flags;
};
static_assert(sizeof(TimelineRow) == 88 && sizeof(GlassExperimentObject) == 40);
struct TimelineProvenance
{
    uint64_t objectIndex = 0;
    GlassExperimentInstanceSource source;
};
static_assert(sizeof(TimelineProvenance) == 200);
struct TimelinePipeline
{
    uint64_t identity = 0;
    const void* original = nullptr;
    GlassExperimentPipelineAccess access {};
    GlassExperimentPipelineView view {};
    void* token = nullptr;
    bool selected = false;
    ~TimelinePipeline() { if (token) access.release(token); }
};
template<unsigned DrawCapacity = 524288, unsigned ObjectCapacity = 1048576,
         unsigned PipelineCapacity = 4096>
class TimelineRecorder
{
    static_assert((PipelineCapacity & (PipelineCapacity - 1)) == 0);
    std::unique_ptr<TimelineRow[]> rows = std::make_unique<TimelineRow[]>(DrawCapacity);
    std::unique_ptr<GlassExperimentObject[]> objects = std::make_unique<GlassExperimentObject[]>(ObjectCapacity);
    static constexpr unsigned ProvenanceCapacity = (ObjectCapacity + 3) / 4;
    GlassExperimentInstanceQuery instanceQuery = nullptr;
    std::unique_ptr<TimelineProvenance[]> provenance;
    std::atomic<uint64_t> queryAttempts = 0, queryMatches = 0, provenanceCount = 0;
    std::atomic<uint64_t> unmatchedPrevious = 0, unmatchedNext = 0;
    std::array<TimelinePipeline, PipelineCapacity> pipelines;
    std::array<std::atomic<uint64_t>, PipelineCapacity> published {};
    std::mutex pipelineMutex;
    std::atomic<uint64_t> rowCount = 0, objectCount = 0;
    std::atomic<uint64_t> seen = 0, contended = 0, full = 0, missingPipeline = 0, invalidPipeline = 0;
    std::atomic<uint64_t> filtered = 0, pipelineFull = 0;
    std::filesystem::path output;
    std::atomic<bool> stopped = false;

    TimelinePipeline* pipeline(const GlassExperimentDrawInput& draw)
    {
        auto hash = draw.pipelineIdentity ^ (reinterpret_cast<uintptr_t>(draw.originalPipeline) >> 4);
        hash ^= hash >> 33; hash *= 0xff51afd7ed558ccdULL; hash ^= hash >> 33;
        for (unsigned probe = 0; probe < PipelineCapacity; ++probe)
        {
            const auto slot = (static_cast<unsigned>(hash) + probe) & (PipelineCapacity - 1);
            auto& entry = pipelines[slot];
            if (published[slot].load(std::memory_order_acquire))
            {
                if (entry.identity == draw.pipelineIdentity && entry.original == draw.originalPipeline) return &entry;
                continue;
            }
            // Only first observation of a pipeline needs serialization. A
            // published entry is immutable until the host drains callbacks.
            std::unique_lock lock(pipelineMutex, std::try_to_lock);
            if (!lock) { ++contended; return nullptr; }
            if (published[slot].load(std::memory_order_acquire))
            {
                if (entry.identity == draw.pipelineIdentity && entry.original == draw.originalPipeline) return &entry;
                continue;
            }
            if (!draw.pipelineAccess.retain || !draw.pipelineAccess.view || !draw.pipelineAccess.release) return nullptr;
            auto* token = draw.pipelineAccess.retain(draw.pipelineAccess.source);
            if (!token) return nullptr;
            GlassExperimentPipelineView view {}; view.size = sizeof(view);
            if (!draw.pipelineAccess.view(token, &view) || !view.descriptor ||
                view.descriptorBytes != sizeof(D3D12_GRAPHICS_PIPELINE_STATE_DESC))
            { draw.pipelineAccess.release(token); return nullptr; }
            entry.identity = draw.pipelineIdentity; entry.original = draw.originalPipeline;
            entry.access = draw.pipelineAccess; entry.access.source = nullptr;
            entry.view = view; entry.token = token;
            const auto& desc = *static_cast<const D3D12_GRAPHICS_PIPELINE_STATE_DESC*>(view.descriptor);
            entry.selected = desc.BlendState.AlphaToCoverageEnable != 0;
            for (unsigned target = 0; target < desc.NumRenderTargets && target < 8; ++target)
            {
                const auto& blend = desc.BlendState.RenderTarget[desc.BlendState.IndependentBlendEnable ? target : 0];
                const bool replacesRgb = blend.SrcBlend == D3D12_BLEND_ONE && blend.DestBlend == D3D12_BLEND_ZERO &&
                                         blend.BlendOp == D3D12_BLEND_OP_ADD;
                entry.selected |= (blend.RenderTargetWriteMask & 7) && blend.BlendEnable && !blend.LogicOpEnable && !replacesRgb;
            }
            published[slot].store(entry.identity, std::memory_order_release);
            return &entry;
        }
        ++pipelineFull; return nullptr;
    }
    void binary(const char* name, const void* data, size_t bytes) const
    {
        std::ofstream file(output / name, std::ios::binary);
        file.write(static_cast<const char*>(data), static_cast<std::streamsize>(bytes));
        file.close(); if (!file) throw std::runtime_error("Timeline write failed");
    }
  public:
    explicit TimelineRecorder(std::filesystem::path path, GlassExperimentInstanceQuery query = nullptr)
        : instanceQuery(query), output(std::move(path))
    {
        if (!output.is_absolute() || !std::filesystem::create_directory(output))
            throw std::runtime_error("Fresh output directory required");
        if (instanceQuery) provenance = std::make_unique<TimelineProvenance[]>(ProvenanceCapacity);
    }
    int observe(const GlassExperimentEvent& event)
    {
        if (event.kind != GlassExperimentCensus || event.payloadVersion != GLASS_EXPERIMENT_CENSUS_VERSION ||
            event.payloadBytes != sizeof(GlassExperimentCensusInput) || !event.payload) return -1;
        const auto& input = *static_cast<const GlassExperimentCensusInput*>(event.payload);
        if (input.size != sizeof(input) || input.draw.size != sizeof(input.draw)) return -1;
        ++seen;
        const auto& draw = input.draw;
        if (!draw.pipelineIdentity) { ++missingPipeline; return 0; }
        if (stopped) return 0;
        if (rowCount.load(std::memory_order_relaxed) >= DrawCapacity) { ++full; return 0; }
        auto* entry = pipeline(draw);
        if (!entry || entry->view.originalRoot != draw.originalRoot) { ++invalidPipeline; return 0; }
        if (!entry->selected) { ++filtered; return 0; }
        const auto rowIndex = rowCount.fetch_add(1, std::memory_order_relaxed);
        if (rowIndex >= DrawCapacity) { ++full; return 0; }
        auto& row = rows[rowIndex];
        row.sequence = input.sequence; row.frame = event.frame; row.tick = GetTickCount64();
        row.recording = draw.recording; row.pipeline = draw.pipelineIdentity;
        row.command = reinterpret_cast<uintptr_t>(draw.command); row.mesh = draw.mesh;
        row.chunk = draw.chunk; row.operation = input.operation; row.indices = draw.indices;
        row.instances = draw.instances; row.objectDeclared = draw.objectCount;
        const auto objectStart = objectCount.fetch_add(draw.objectCount, std::memory_order_relaxed);
        row.objectFirst = static_cast<unsigned>((std::min)(objectStart, uint64_t(ObjectCapacity)));
        if (draw.objectCount && !draw.objectAt) row.flags |= 1;
        else for (uint32_t i = 0; i < draw.objectCount; ++i)
        {
            if (objectStart + i >= ObjectCapacity) { row.flags |= 4; break; }
            GlassExperimentObject object {};
            if (draw.objectAt(draw.source, i, &object) != 1) { row.flags |= 2; break; }
            objects[objectStart + i] = object; ++row.objectCount;
            if (instanceQuery && event.frame && event.frame <= UINT32_MAX && (object.globalRange || object.count > 1))
            {
                ++queryAttempts;
                GlassExperimentInstanceSource source;
                const auto result = instanceQuery(static_cast<unsigned>(event.frame), draw.mesh,
                                                  object.transformIndex, object.count, &source);
                bool valid = result == 1 && source.size == sizeof(source) && source.version == 1 &&
                    source.proxy && source.mesh == draw.mesh && source.renderer && source.scene &&
                    source.frame == event.frame && source.selectedCount == object.count && !source.reserved &&
                    source.originalCount && source.originalCount <= 65536 && source.selectedCount <= source.originalCount &&
                    source.linear <= 1 && (source.linear ? source.selectedCount == source.originalCount : source.selectedCount <= 64);
                if (valid && !source.linear)
                    for (unsigned element = 0; element < source.selectedCount; ++element)
                        valid &= source.indices[element] < source.originalCount;
                if (valid)
                {
                    ++queryMatches;
                    const auto index = provenanceCount.fetch_add(1, std::memory_order_relaxed);
                    if (index < ProvenanceCapacity) provenance[index] = {objectStart + i, source};
                }
                else
                {
                    // Diagnostic counters only. Neighboring-frame observations
                    // are never accepted as this draw's identity or motion.
                    GlassExperimentInstanceSource neighbor;
                    if (event.frame > 1 && instanceQuery(static_cast<unsigned>(event.frame - 1), draw.mesh,
                        object.transformIndex, object.count, &neighbor) == 1) ++unmatchedPrevious;
                    neighbor = {};
                    if (event.frame < UINT32_MAX && instanceQuery(static_cast<unsigned>(event.frame + 1), draw.mesh,
                        object.transformIndex, object.count, &neighbor) == 1) ++unmatchedNext;
                }
            }
        }
        return 1;
    }
    void save()
    {
        // The experiment ABI calls destroy only after all callbacks drain.
        stopped = true;
        const auto savedRows = (std::min)(rowCount.load(), uint64_t(DrawCapacity));
        const auto savedObjects = (std::min)(objectCount.load(), uint64_t(ObjectCapacity));
        binary("draws.bin", rows.get(), size_t(savedRows) * sizeof(TimelineRow));
        binary("objects.bin", objects.get(), size_t(savedObjects) * sizeof(GlassExperimentObject));
        const auto savedProvenance = (std::min)(provenanceCount.load(), uint64_t(ProvenanceCapacity));
        if (instanceQuery) binary("provenance.bin", provenance.get(), size_t(savedProvenance) * sizeof(TimelineProvenance));
        unsigned retained = 0, selected = 0;
        for (const auto& entry : pipelines)
        {
            if (!entry.token) continue;
            ++retained; if (!entry.selected) continue; ++selected;
            const auto& desc = *static_cast<const D3D12_GRAPHICS_PIPELINE_STATE_DESC*>(entry.view.descriptor);
            const std::string stem = std::to_string(entry.identity);
            for (unsigned stage = 0; stage < 2; ++stage)
            {
                const auto& code = stage ? desc.PS : desc.VS;
                if (code.pShaderBytecode && code.BytecodeLength)
                    binary((stem + (stage ? ".ps.dxbc" : ".vs.dxbc")).c_str(), code.pShaderBytecode, code.BytecodeLength);
            }
        }
        std::ofstream done(output / "timeline.done");
        done << "format=1\nrow_bytes=" << sizeof(TimelineRow) << "\nobject_bytes=" << sizeof(GlassExperimentObject)
             << "\nrows=" << savedRows << "\nobjects=" << savedObjects << "\nseen=" << seen.load()
             << "\nmissing_pipeline=" << missingPipeline.load() << "\ninvalid_pipeline=" << invalidPipeline.load()
             << "\ncontended=" << contended.load() << "\nfull=" << full.load() << "\nfiltered=" << filtered
             << "\npipeline_full=" << pipelineFull << "\npipelines=" << retained << "\nselected_pipelines=" << selected
             << "\nrow_storage_bytes=" << size_t(DrawCapacity) * sizeof(TimelineRow)
             << "\nobject_storage_bytes=" << size_t(ObjectCapacity) * sizeof(GlassExperimentObject)
             << "\nprovenance_row_bytes=" << sizeof(TimelineProvenance) << "\nprovenance_rows=" << savedProvenance
             << "\nquery_attempts=" << queryAttempts.load() << "\nquery_matches=" << queryMatches.load()
             << "\nunmatched_previous_frame=" << unmatchedPrevious.load() << "\nunmatched_next_frame=" << unmatchedNext.load()
             << "\nprovenance_full=" << (provenanceCount.load() - savedProvenance)
             << "\nprovenance_storage_bytes=" << (instanceQuery ? size_t(ProvenanceCapacity) * sizeof(TimelineProvenance) : 0)
             << "\norder=cpu_observation\nframe_zero=unknown\ngpu_buffer_copies=0\nmotion_produced=0\n";
        done.close(); if (!done) throw std::runtime_error("Timeline completion write failed");
    }
};
using RuntimeTimeline = TimelineRecorder<>;
int32_t timelineCreate(const GlassExperimentHost* host, void** context)
{
    if (!context) return -1; *context = nullptr;
    if (!host || host->size != sizeof(*host) || host->abi != GLASS_EXPERIMENT_ABI ||
        !(host->capabilities & GlassExperimentCensus)) return -1;
    try
    {
        wchar_t path[32768]; const auto length = GetModuleFileNameW(timelineModule, path, 32768);
        if (!length || length >= 32768) return -1;
        auto config = std::filesystem::path(path); config.replace_extension(L".config");
        if (std::filesystem::file_size(config) > 32768) return -1;
        std::ifstream file(config); std::string output;
        if (!std::getline(file, output) || file.peek() != std::char_traits<char>::eof()) return -1;
        if (!output.empty() && output.back() == '\r') output.pop_back();
        GlassExperimentInstanceQuery query = nullptr;
        auto providerConfig = std::filesystem::path(path); providerConfig.replace_extension(L".provider");
        if (std::filesystem::exists(providerConfig))
        {
            if (std::filesystem::file_size(providerConfig) > 32768) return -1;
            std::ifstream providerFile(providerConfig); std::string providerPath;
            if (!std::getline(providerFile, providerPath) || providerFile.peek() != std::char_traits<char>::eof()) return -1;
            if (!providerPath.empty() && providerPath.back() == '\r') providerPath.pop_back();
            const std::filesystem::path provider(std::u8string(providerPath.begin(), providerPath.end()));
            if (!provider.is_absolute()) return -1;
            const auto module = GetModuleHandleW(provider.c_str());
            const auto address = module ? GetProcAddress(module, "GlassInstanceSourceQuery") : nullptr;
            HMODULE pinned = nullptr;
            if (!address || !GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
                reinterpret_cast<LPCWSTR>(address), &pinned) || pinned != module) return -1;
            query = reinterpret_cast<GlassExperimentInstanceQuery>(address);
        }
        *context = new RuntimeTimeline(std::filesystem::path(std::u8string(output.begin(), output.end())), query);
        return 0;
    }
    catch (...) { return -1; }
}
int32_t timelineEvent(void* context, const GlassExperimentEvent* event)
{
    try { return context && event ? static_cast<RuntimeTimeline*>(context)->observe(*event) : -1; }
    catch (...) { return -1; }
}
void timelineDestroy(void* context)
{
    std::unique_ptr<RuntimeTimeline> recorder(static_cast<RuntimeTimeline*>(context));
    try { if (recorder) recorder->save(); } catch (...) { /* Missing completion marks failure. */ }
}
const GlassExperimentApi timelineApi {sizeof(timelineApi), GLASS_EXPERIMENT_ABI, GlassExperimentCensus,
                                     timelineCreate, timelineEvent, timelineDestroy};
}
extern "C" __declspec(dllexport) const GlassExperimentApi* GlassExperimentQuery() { return &timelineApi; }
BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH) timelineModule = instance;
    return TRUE;
}
