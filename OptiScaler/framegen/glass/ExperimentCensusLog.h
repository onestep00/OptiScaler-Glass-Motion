#pragma once
#include "ExperimentCensusAbi.h"
#include <array>
#include <atomic>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>

namespace GlassFg
{
// Diagnostic CPU metadata only. Fixed storage is allocated once at module
// preparation. No callback-owned pointer, shader/texture/bone copy or file I/O.
class ExperimentCensusLog
{
    static constexpr unsigned Capacity = 32768;
    struct Row
    {
        uint64_t frame = 0;
        GlassExperimentCensusInput input {};
        std::array<GlassExperimentTarget, 9> targets {};
        std::array<GlassExperimentObject, 32> objects {};
        uint32_t objectsSaved = 0;
    };
    std::unique_ptr<Row[]> rows = std::make_unique<Row[]>(Capacity);
    unsigned count = 0;
    std::mutex mutex;
    std::atomic<uint64_t> contended = 0, overflow = 0;
  public:
    int32_t observe(const GlassExperimentEvent& event)
    {
        if (event.payloadVersion != GLASS_EXPERIMENT_CENSUS_VERSION ||
            event.payloadBytes != sizeof(GlassExperimentCensusInput) || !event.payload) return -1;
        const auto& input = *static_cast<const GlassExperimentCensusInput*>(event.payload);
        if (input.size != sizeof(input) || input.draw.size != sizeof(input.draw)) return -1;
        std::unique_lock lock(mutex, std::try_to_lock);
        if (!lock) { ++contended; return 0; }
        if (count == Capacity) { ++overflow; return 0; }
        auto& row = rows[count++]; row.frame = event.frame; row.input = input;
        if (input.draw.objectAt)
            for (unsigned i = 0; i < input.draw.objectCount && i < row.objects.size(); ++i)
                if (input.draw.objectAt(input.draw.source, i, &row.objects[i]) == 1) ++row.objectsSaved;
        if (input.draw.targetAt)
            for (unsigned i = 0; i < row.targets.size(); ++i)
            {
                auto& target = row.targets[i]; target.size = sizeof(target);
                if (input.draw.targetAt(input.draw.targetSource, i, &target) != 1) target = {};
            }
        auto& saved = row.input.draw;
        saved.descriptor = nullptr; saved.descriptorBytes = 0; saved.pipelineAccess = {};
        saved.source = nullptr; saved.objectAt = nullptr; saved.meshShape = nullptr;
        saved.targetSource = nullptr; saved.targetAt = nullptr;
        saved.bindingSource = nullptr; saved.bindingAt = nullptr;
        // command/PSO/root fields are numeric observations, not retained objects.
        return 1;
    }
    // Called only after all CPU callbacks end (module destroy, control thread).
    void save(const std::filesystem::path& output) const
    {
        std::ofstream csv(output / "draw-census.csv");
        csv << "sequence,operation,frame,recording,command,callsite,pipeline_address,pipeline_identity,root_address,"
               "mesh,chunk,object_entries,indices_or_vertices,instances,start_index_or_vertex,base_vertex,start_instance,"
               "raster_flags,raster_usable,root_replayable,rtv_count,dsv_handle,signature,max_commands,arguments,"
               "argument_offset,counter,counter_offset";
        for (unsigned i = 0; i < 6; ++i) csv << ",viewport_" << i;
        for (unsigned i = 0; i < 4; ++i) csv << ",scissor_" << i;
        for (unsigned i = 0; i < 9; ++i) csv << ",target_" << i << "_resource,target_" << i << "_revision";
        csv << ",object_entries_saved\n";
        std::ofstream binary(output / "draw-census.targets.bin", std::ios::binary);
        std::ofstream objects(output / "draw-census.objects.csv");
        objects << "sequence,entry,proxy,mesh,slot,generation,first,count,transform_index,global\n";
        for (unsigned index = 0; index < count; ++index)
        {
            const auto& row = rows[index]; const auto& r = row.input; const auto& d = r.draw;
            csv << r.sequence << ',' << r.operation << ',' << row.frame << ',' << d.recording << ','
                << reinterpret_cast<uint64_t>(d.command) << ',' << r.callsite << ','
                << reinterpret_cast<uint64_t>(d.originalPipeline) << ',' << d.pipelineIdentity << ','
                << reinterpret_cast<uint64_t>(d.originalRoot) << ',' << d.mesh << ',' << d.chunk << ',' << d.objectCount << ','
                << d.indices << ',' << d.instances << ',' << d.startIndex << ',' << d.baseVertex << ',' << d.startInstance << ','
                << r.rasterFlags << ',' << d.rasterKnown << ',' << d.rootReplayable << ',' << d.renderTargetCount << ','
                << d.depthTarget << ',' << r.signature << ',' << r.maxCommands << ',' << r.arguments << ','
                << r.argumentOffset << ',' << r.counter << ',' << r.counterOffset;
            for (const auto v : d.viewport) csv << ',' << v;
            for (const auto s : d.scissor) csv << ',' << s;
            for (const auto& target : row.targets) csv << ',' << target.resource << ',' << target.revision;
            csv << ',' << row.objectsSaved << '\n';
            for (unsigned i = 0; i < row.objects.size() && i < d.objectCount; ++i)
            {
                const auto& object = row.objects[i];
                objects << r.sequence << ',' << i << ',' << object.proxy << ',' << object.mesh << ',' << object.slot << ','
                        << object.generation << ',' << object.first << ',' << object.count << ',' << object.transformIndex << ','
                        << object.globalRange << '\n';
            }
            binary.write(reinterpret_cast<const char*>(row.targets.data()), sizeof(row.targets));
        }
        csv.close(); binary.close(); objects.close();
        if (!csv || !binary || !objects) throw std::runtime_error("Census output write failed");
        std::ofstream done(output / "draw-census.done");
        done << "format=1\nrows=" << count << "\ncapacity=" << Capacity << "\ncpu_storage_bytes=" << Capacity * sizeof(Row)
             << "\ncontended=" << contended.load() << "\noverflow=" << overflow.load()
             << "\ntarget_record_bytes=" << sizeof(GlassExperimentTarget) << "\ntargets_per_row=9"
                "\norder=cpu_observation\nframe_zero=unknown\nindirect_counts=upper_bound_only"
                "\ngpu_copies=0\nobject_motion_produced=0\n";
        done.close(); if (!done) throw std::runtime_error("Census completion write failed");
    }
};
} // namespace GlassFg
