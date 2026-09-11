#include "pch.h"
#include "GeometryCreation.h"
#include "CyberpunkLayout.h"
#include "CyberpunkObjects.h"
#include "CyberpunkDraws.h"
#include "GeometryCommands.h"
#include "GeometryHealth.h"
#include <Util.h>
#include <mutex>

namespace GlassFg
{
namespace
{
void refreshHealth() noexcept
{
    try
    {
        GeometryCreationStats creation;
        auto health = ReadGeometryHealth();
        if (TryGeometryCreationCounters(creation))
        {
            health.counts[GeometryRoots] = creation.roots;
            health.counts[GeometryPipelines] = creation.graphics;
            health.counts[GeometryCompiled] = creation.cache.ready;
            health.counts[GeometryPending] = creation.cache.pending;
            health.counts[GeometryCompileRejected] = creation.cache.rejected;
        }
        const auto packets = GetCyberpunkDrawStatus();
        const auto commands = GetGeometryCommandStats();
        health.counts[GeometryIdentities] = packets.identities;
        health.counts[GeometryEngineDraws] = packets.draws;
        health.counts[GeometryPublicDraws] = commands.indexed;
        health.counts[GeometryPackets] = commands.packets;
        health.counts[GeometryPipelineMatches] = commands.pipelinesReady;
        health.counts[GeometryBindingMatches] = commands.bindingsReady;
        health.counts[GeometryIndirectKnown] = commands.indirectKnown;
        health.counts[GeometryIndirectUnknown] = commands.indirectUnknown;
        health.frame = commands.lastFrame;
        health.sampledMs = GetTickCount64();
        PublishGeometryHealth(health);
    }
    catch (...)
    {
    }
}
} // namespace
void InitializeGeometryHost(ID3D12Device* device) noexcept
{
    try
    {
        static std::once_flag once;
        std::call_once(
            once,
            [device]
            {
                if (!device || !IsCyberpunkExecutable(GetModuleHandleW(nullptr)))
                    return;
                const auto directory = Util::DllPath().parent_path();
                const auto compiler = directory / L"Glass" / L"dxcompiler.dll";
                const bool files = std::filesystem::is_regular_file(compiler) &&
                                   std::filesystem::is_regular_file(directory / L"Glass" / L"dxil.dll");
                const bool ready = files && StartGeometryCreation(device, compiler);
                const bool objects = ready && InitializeCyberpunkObjects(GetModuleHandleW(nullptr));
                const bool draws = objects && InitializeCyberpunkDraws(GetModuleHandleW(nullptr));
                const bool commands = ready && StartGeometryCommands(device);
                GeometryHealth health;
                health.capabilities = GeometryStarted | (files ? GeometryCompiler : 0u) |
                                      (ready ? GeometryCreationHooks : 0u) |
                                      (GetCyberpunkLayout(GetModuleHandleW(nullptr)) ? GeometryEngineLayout : 0u) |
                                      (objects ? GeometryObjectHooks : 0u) | (draws ? GeometryDrawHooks : 0u) |
                                      (commands ? GeometryCommandHooks : 0u);
                health.sampledMs = GetTickCount64();
                PublishGeometryHealth(health);
                GeometryTelemetry::refresh.store(refreshHealth, std::memory_order_release);
                if (FILE* log = _wfopen((directory / L"OptiScaler.Glass.Geometry.log").c_str(), L"a"))
                {
                    fprintf(
                        log,
                        "GEOMETRY_CREATION active=%u compiler_files=%u mesh_registry=%u draw_packets=%u commands=%u "
                        "engine_layout=relocatable_profiles actual_draw_replacement=0 fg_substitution=0\n",
                        ready, files, objects, draws, commands);
                    fclose(log);
                }
            });
    }
    catch (...)
    {
    } // Optional acquisition never changes the application's result.
}
void ReportGeometryHost(FILE* log) noexcept
{
    try
    {
        const auto creation = GetGeometryCreationStats();
        const auto objects = GetCyberpunkObjectStatus();
        const auto packets = GetCyberpunkDrawStatus();
        const auto commands = GetGeometryCommandStats();
        auto health = ReadGeometryHealth();
        health.counts = { creation.roots,
                          creation.graphics,
                          creation.cache.ready,
                          creation.cache.pending,
                          creation.cache.rejected,
                          packets.identities,
                          packets.draws,
                          commands.indexed,
                          commands.packets,
                          commands.pipelinesReady,
                          commands.bindingsReady,
                          commands.indirectKnown,
                          commands.indirectUnknown,
                          0,
                          0 };
        health.frame = commands.lastFrame;
        health.sampledMs = GetTickCount64();
        PublishGeometryHealth(health);
        health = ReadGeometryHealth();
        if (!log)
            return;
        fprintf(log, "GEOMETRY_HEALTH capabilities=%u frame=%u object_capture=%llu fg_replacements=%llu reason=%s\n",
                health.capabilities, health.frame, health.counts[GeometryCaptureDraws],
                health.counts[GeometryFgReplacements], health.reason(health.sampledMs));
        fprintf(log,
                "GEOMETRY_OBSERVE roots=%llu graphics=%llu compiled=%llu pending=%llu rejected=%llu "
                "batches=%llu appends=%llu identities=%llu engine_draws=%llu packet_rejected=%llu "
                "recordings=%llu capacity_rejected=%llu indexed=%llu packets=%llu instances=%llu "
                "draw_identities=%llu pipeline_ready=%llu bindings_ready=%llu frame=%u "
                "object_snapshot_rejected=%llu signatures=%llu indirect_known=%llu indirect_unknown=%llu "
                "actual_draw_replacement=0 fg_substitution=0\n",
                creation.roots, creation.graphics, creation.cache.ready, creation.cache.pending,
                creation.cache.rejected, packets.batches, packets.appends, packets.identities, packets.draws,
                packets.rejected, commands.recordings, commands.capacityRejected, commands.indexed, commands.packets,
                commands.instances, commands.identities, commands.pipelinesReady, commands.bindingsReady,
                commands.lastFrame, objects.snapshotsRejected, commands.signatures, commands.indirectKnown,
                commands.indirectUnknown);
        if (!creation.cache.lastError.empty())
            fprintf(log, "GEOMETRY_COMPILER last_error=%s\n", creation.cache.lastError.c_str());
    }
    catch (...)
    {
    }
}
} // namespace GlassFg
