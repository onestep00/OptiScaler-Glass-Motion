// Replaceable CPU diagnostic. Retains immutable pipeline inputs only; no GPU
// resource retention, readback, injected rendering or inferred object identity.
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <d3d12.h>
#include "ExperimentCensusAbi.h"
#include "ExperimentSourceAbi.h"
#include "CyberpunkInstanceSelection.h"
#include "DiagnosticPacketTransform.h"
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
const GlassExperimentApi* timelineEpochApi=nullptr;
void* timelineEpochContext=nullptr;
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
struct TimelineParent { uint64_t objectIndex; GlassExperimentPacketParent source; };
struct TimelinePacketFlags { uint32_t flags=0, valid=0; };
struct TimelinePacketSource
{
    GlassExperimentSourceOwner owner;
    uint32_t originalFirst=UINT32_MAX, valid=0;
};
static_assert(sizeof(TimelinePacketSource)==48);
struct TimelinePacketTransform
{
    uint64_t packetIndex=0;
    uint32_t ordinal=0,valid=0;
    std::array<uint32_t,12> words{};
};
static_assert(sizeof(TimelinePacketTransform)==64);
bool readPacketTransform(uint64_t address,std::array<uint32_t,12>& value) noexcept
{
    __try { memcpy(value.data(),reinterpret_cast<const void*>(address),sizeof(value));return true; }
    __except(EXCEPTION_EXECUTE_HANDLER){return false;}
}
static_assert(sizeof(TimelineParent) == 80);
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
    GlassExperimentInstanceQueryV2 instanceQueryV2 = nullptr;
    GlassExperimentPacketQuery packetQuery = nullptr;
    GlassExperimentPacketQueryV2 packetQueryV2 = nullptr;
    std::unique_ptr<TimelinePacketFlags[]> packetFlags;
    GlassExperimentSourceQuery packetSourceQuery = nullptr;
    std::unique_ptr<TimelinePacketSource[]> packetSources;
    static constexpr uint64_t TransformCapacity=65536;
    std::unique_ptr<TimelinePacketTransform[]> packetTransforms;
    std::atomic<uint64_t> transformCount=0;
    std::unique_ptr<TimelineProvenance[]> provenance;
    std::unique_ptr<GlassExperimentSourceOwner[]> sourceOwners;
    std::unique_ptr<TimelineParent[]> packetParents;
    std::atomic<uint64_t> packetAttempts = 0, packetCount = 0;
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
    explicit TimelineRecorder(std::filesystem::path path, GlassExperimentInstanceQuery query = nullptr,
                              GlassExperimentPacketQuery packet = nullptr, GlassExperimentInstanceQueryV2 queryV2 = nullptr,
                              GlassExperimentPacketQueryV2 packetV2 = nullptr, GlassExperimentSourceQuery ownerQuery = nullptr,
                              bool captureTransforms=false)
        : instanceQuery(query), instanceQueryV2(queryV2), packetQuery(packet), packetQueryV2(packetV2), packetSourceQuery(ownerQuery), output(std::move(path))
    {
        if (!output.is_absolute() || !std::filesystem::create_directory(output))
            throw std::runtime_error("Fresh output directory required");
        if (instanceQuery || instanceQueryV2) provenance = std::make_unique<TimelineProvenance[]>(ProvenanceCapacity);
        if (instanceQueryV2) sourceOwners = std::make_unique<GlassExperimentSourceOwner[]>(ProvenanceCapacity);
        if (packetQuery || packetQueryV2) packetParents = std::make_unique<TimelineParent[]>(ProvenanceCapacity);
        if (packetQueryV2) packetFlags = std::make_unique<TimelinePacketFlags[]>(ProvenanceCapacity);
        if (packetQueryV2 && packetSourceQuery) packetSources=std::make_unique<TimelinePacketSource[]>(ProvenanceCapacity);
        if(captureTransforms&&packetSources)packetTransforms=std::make_unique<TimelinePacketTransform[]>(TransformCapacity);
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
        if(timelineEpochApi)timelineEpochApi->event(timelineEpochContext,&event);
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
            if (packetQuery || packetQueryV2)
            {
                ++packetAttempts;
                GlassExperimentPacketParent parent;
                TimelinePacketFlags flags;
                int result=0;
                if (packetQueryV2)
                {
                    GlassExperimentPacketParentV2 value;
                    result=packetQueryV2(input.callsite,draw.mesh,draw.chunk,draw.instances,i,object.first,object.count,
                        object.transformIndex,object.globalRange,&value);
                    parent=value.parent; flags={value.instanceFlags,value.flagsValid};
                    if(value.size!=sizeof(value)||value.version!=2||flags.valid>1||flags.flags>UINT16_MAX)result=-1;
                }
                else result=packetQuery(input.callsite,draw.mesh,draw.chunk,draw.instances,i,object.first,object.count,
                    object.transformIndex,object.globalRange,&parent);
                if (result == 1 && parent.size == sizeof(parent) &&
                    parent.version == 1 && parent.proxy && parent.mesh == draw.mesh && parent.count == object.count &&
                    parent.first == object.first && parent.transformIndex == object.transformIndex &&
                    parent.globalRange == object.globalRange && !parent.reserved)
                {
                    const auto index = packetCount.fetch_add(1, std::memory_order_relaxed);
                    if (index < ProvenanceCapacity)
                    {
                        packetParents[index] = {objectStart + i, parent};
                        if(packetFlags)packetFlags[index]=flags;
                        if(packetSources && flags.valid==1)
                        {
                            GlassFg::CyberpunkInstanceSelection selection;
                            GlassExperimentSourceOwner owner;
                            if(selection.resolveGlobalPacket(parent.globalRange==1,parent.transformIndex,parent.count,
                                parent.arrayGlobalStart,parent.arrayCount,flags.flags) &&
                               packetSourceQuery(parent.proxy,parent.mesh,parent.arrayCount,&owner)==1 &&
                               owner.size==sizeof(owner)&&owner.version==1&&!owner.reserved&&owner.node&&owner.buffer&&
                               owner.generation&&owner.count==parent.arrayCount&&
                               uint64_t(owner.first)+owner.count <= (uint64_t{1}<<32))
                            {
                                packetSources[index]={owner,selection.linearFirst,1};
                                if(packetTransforms)
                                    for(uint32_t ordinal=0;ordinal<parent.count;++ordinal)
                                    {
                                        const auto slot=transformCount.fetch_add(1,std::memory_order_relaxed);
                                        if(slot>=TransformCapacity)break;
                                        auto& transform=packetTransforms[slot];
                                        transform.packetIndex=index;transform.ordinal=ordinal;
                                        transform.valid=GlassFg::capturePacketTransform(parent,ordinal,transform.words,readPacketTransform);
                                    }
                            }
                        }
                    }
                }
            }
            if ((instanceQuery || instanceQueryV2) && event.frame && event.frame <= UINT32_MAX && (object.globalRange || object.count > 1))
            {
                ++queryAttempts;
                GlassExperimentInstanceSource source;
                GlassExperimentSourceOwner owner;
                int result = 0; bool validOwner = true;
                if (instanceQueryV2)
                {
                    GlassExperimentInstanceSourceV2 extended;
                    result = instanceQueryV2(static_cast<unsigned>(event.frame), draw.mesh,
                                            object.transformIndex, object.count, &extended);
                    source = extended.instance; owner = extended.owner;
                    const bool present = owner.node || owner.buffer || owner.first || owner.count || owner.generation;
                    validOwner = extended.size == sizeof(extended) && extended.version == 2 &&
                        owner.size == sizeof(owner) && owner.version == 1 && !owner.reserved &&
                        (!present || (owner.node && owner.buffer && owner.generation && owner.count == source.originalCount &&
                         std::uint64_t(owner.first) + owner.count <= (std::uint64_t {1} << 32)));
                }
                else result = instanceQuery(static_cast<unsigned>(event.frame), draw.mesh,
                                            object.transformIndex, object.count, &source);
                bool valid = validOwner && result == 1 && source.size == sizeof(source) && source.version == 1 &&
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
                    if (index < ProvenanceCapacity)
                    {
                        provenance[index] = {objectStart + i, source};
                        if (sourceOwners) sourceOwners[index] = owner;
                    }
                }
                else
                {
                    // Diagnostic counters only. Neighboring-frame observations
                    // are never accepted as this draw's identity or motion.
                    GlassExperimentInstanceSource neighbor;
                    if (instanceQuery && event.frame > 1 && instanceQuery(static_cast<unsigned>(event.frame - 1), draw.mesh,
                        object.transformIndex, object.count, &neighbor) == 1) ++unmatchedPrevious;
                    neighbor = {};
                    if (instanceQuery && event.frame < UINT32_MAX && instanceQuery(static_cast<unsigned>(event.frame + 1), draw.mesh,
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
        if (provenance) binary("provenance.bin", provenance.get(), size_t(savedProvenance) * sizeof(TimelineProvenance));
        if (sourceOwners) binary("source-owners.bin", sourceOwners.get(), size_t(savedProvenance) * sizeof(GlassExperimentSourceOwner));
        const auto savedParents = (std::min)(packetCount.load(), uint64_t(ProvenanceCapacity));
        if (packetParents) binary("packet-parents.bin", packetParents.get(), size_t(savedParents) * sizeof(TimelineParent));
        if (packetFlags) binary("packet-flags.bin", packetFlags.get(), size_t(savedParents)*sizeof(TimelinePacketFlags));
        if (packetSources) binary("packet-sources.bin",packetSources.get(),size_t(savedParents)*sizeof(TimelinePacketSource));
        const auto savedTransforms=(std::min)(transformCount.load(),TransformCapacity);
        if(packetTransforms)binary("packet-transforms.bin",packetTransforms.get(),size_t(savedTransforms)*sizeof(TimelinePacketTransform));
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
             << "\nprovenance_storage_bytes=" << (provenance ? size_t(ProvenanceCapacity) * sizeof(TimelineProvenance) : 0)
             << "\nsource_owner_row_bytes=" << sizeof(GlassExperimentSourceOwner)
             << "\nsource_owner_rows=" << (sourceOwners ? savedProvenance : 0)
             << "\nsource_owner_storage_bytes=" << (sourceOwners ? size_t(ProvenanceCapacity) * sizeof(GlassExperimentSourceOwner) : 0)
             << "\nsource_query_version=" << (instanceQueryV2 ? 2 : (instanceQuery ? 1 : 0))
             << "\npacket_parent_rows=" << savedParents << "\npacket_query_attempts=" << packetAttempts.load()
             << "\npacket_parent_full=" << (packetCount.load() - savedParents)
             << "\npacket_storage_bytes=" << (packetParents ? size_t(ProvenanceCapacity) * sizeof(TimelineParent) : 0)
             << "\npacket_query_version=" << (packetQueryV2?2:(packetQuery?1:0))
             << "\npacket_flags_row_bytes=" << sizeof(TimelinePacketFlags)
             << "\npacket_flags_rows=" << (packetFlags?savedParents:0)
             << "\npacket_source_row_bytes=" << sizeof(TimelinePacketSource)
             << "\npacket_source_rows=" << (packetSources?savedParents:0)
             << "\npacket_transform_rows=" << savedTransforms
             << "\npacket_transform_row_bytes=" << sizeof(TimelinePacketTransform)
             << "\npacket_transform_overflow=" << (transformCount.load()-savedTransforms)
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
        GlassExperimentInstanceQueryV2 queryV2 = nullptr;
        GlassExperimentPacketQuery packetQuery = nullptr;
        GlassExperimentPacketQueryV2 packetQueryV2 = nullptr;
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
            const auto sourceAddress = module ? GetProcAddress(module, "GlassInstanceSourceQuery") : nullptr;
            const auto sourceAddressV2 = module ? GetProcAddress(module, "GlassInstanceSourceQueryV2") : nullptr;
            const auto packetAddress = module ? GetProcAddress(module, "GlassPacketParentQuery") : nullptr;
            const auto packetAddressV2 = module ? GetProcAddress(module, "GlassPacketParentQueryV2") : nullptr;
            const auto address = sourceAddressV2 ? sourceAddressV2 : (sourceAddress ? sourceAddress : (packetAddressV2?packetAddressV2:packetAddress));
            HMODULE pinned = nullptr;
            if (!address || !GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
                reinterpret_cast<LPCWSTR>(address), &pinned) || pinned != module) return -1;
            query = reinterpret_cast<GlassExperimentInstanceQuery>(sourceAddress);
            queryV2 = reinterpret_cast<GlassExperimentInstanceQueryV2>(sourceAddressV2);
            packetQuery = reinterpret_cast<GlassExperimentPacketQuery>(packetAddress);
            packetQueryV2 = reinterpret_cast<GlassExperimentPacketQueryV2>(packetAddressV2);
        }
        GlassExperimentSourceQuery ownerQuery=nullptr;
        auto ownerConfig=std::filesystem::path(path);ownerConfig.replace_extension(L".owner-provider");
        if(std::filesystem::exists(ownerConfig))
        {
            if(!packetQueryV2||std::filesystem::file_size(ownerConfig)>32768)return -1;
            std::ifstream ownerFile(ownerConfig);std::string line;
            if(!std::getline(ownerFile,line)||ownerFile.peek()!=std::char_traits<char>::eof())return -1;
            if(!line.empty()&&line.back()=='\r')line.pop_back();
            const std::filesystem::path ownerPath(std::u8string(line.begin(),line.end()));
            if(!ownerPath.is_absolute())return -1;
            const auto ownerModule=GetModuleHandleW(ownerPath.c_str());
            const auto address=ownerModule?GetProcAddress(ownerModule,"GlassSourceOwnerQuery"):nullptr;
            HMODULE pinned=nullptr;
            if(!address||!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_PIN,
                reinterpret_cast<LPCWSTR>(address),&pinned)||pinned!=ownerModule)return -1;
            ownerQuery=reinterpret_cast<GlassExperimentSourceQuery>(address);
        }
        auto transformsConfig=std::filesystem::path(path);transformsConfig.replace_extension(L".transforms");
        bool transforms=false;
        if(std::filesystem::exists(transformsConfig))
        {
            std::ifstream f(transformsConfig);std::string mode;uint32_t pid=0;
            if(!(f>>mode>>pid)||mode!="original48-v1"||pid!=GetCurrentProcessId()||!ownerQuery)return -1;
            f>>std::ws;if(f.peek()!=std::char_traits<char>::eof())return -1;
            transforms=true;
        }
        *context = new RuntimeTimeline(std::filesystem::path(std::u8string(output.begin(), output.end())), query, packetQuery, queryV2, packetQueryV2,ownerQuery,transforms);
        auto epochConfig=std::filesystem::path(path);epochConfig.replace_extension(L".epoch-provider");
        if(std::filesystem::exists(epochConfig))
        {
            try
            {
                if(std::filesystem::file_size(epochConfig)>32768)throw std::runtime_error("Epoch config size");
                std::ifstream f(epochConfig);std::string line;
                if(!std::getline(f,line)||f.peek()!=std::char_traits<char>::eof())throw std::runtime_error("Epoch config format");
                if(!line.empty()&&line.back()=='\r')line.pop_back();
                const std::filesystem::path providerPath(std::u8string(line.begin(),line.end()));
                if(!providerPath.is_absolute())throw std::runtime_error("Epoch provider path");
                const auto providerModule=GetModuleHandleW(providerPath.c_str());
                auto get=reinterpret_cast<GlassExperimentQueryFn>(providerModule?GetProcAddress(providerModule,"GlassExperimentQuery"):nullptr);
                const auto api=get?get():nullptr;
                if(!api||api->size!=sizeof(*api)||api->abi!=GLASS_EXPERIMENT_ABI||!api->create||!api->event||!api->destroy)
                    throw std::runtime_error("Epoch provider ABI");
                if(api->create(host,&timelineEpochContext)!=0)throw std::runtime_error("Epoch provider create");
                timelineEpochApi=api;
            }
            catch(...){delete static_cast<RuntimeTimeline*>(*context);*context=nullptr;throw;}
        }
        return 0;
    }
    catch (...) { return -1; }
}
int32_t timelineEvent(void* context, const GlassExperimentEvent* event)
{
    try
    {
        if(!context||!event)return -1;
        return static_cast<RuntimeTimeline*>(context)->observe(*event);
    }
    catch (...) { return -1; }
}
void timelineDestroy(void* context)
{
    if(timelineEpochApi)
    {
        timelineEpochApi->destroy(timelineEpochContext);timelineEpochApi=nullptr;timelineEpochContext=nullptr;
    }
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
