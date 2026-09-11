#include "pch.h"
#include "GeometryCreation.h"
#include "CyberpunkSurfacePass.h"
#include "CyberpunkObjects.h"
#include "CyberpunkDraws.h"
#include "GeometryCommands.h"
#include <Util.h>
#include <mutex>

namespace GlassFg
{
void InitializeGeometryHost(ID3D12Device* device) noexcept
{
    try
    {
        static std::once_flag once;
        std::call_once(
            once,
            [device]
            {
                CyberpunkSurfacePass executable;
                if (!device || !executable.initialize(GetModuleHandleW(nullptr), nullptr))
                    return;
                const auto directory = Util::DllPath().parent_path();
                const auto compiler = directory / L"Glass" / L"dxcompiler.dll";
                const bool files = std::filesystem::is_regular_file(compiler) &&
                                   std::filesystem::is_regular_file(directory / L"Glass" / L"dxil.dll");
                const bool ready = files && StartGeometryCreation(device, compiler);
                const bool objects = ready && InitializeCyberpunkObjects(GetModuleHandleW(nullptr));
                const bool draws = objects && InitializeCyberpunkDraws(GetModuleHandleW(nullptr));
                const bool commands = draws && StartGeometryCommands(device);
                if (FILE* log = _wfopen((directory / L"OptiScaler.Glass.Geometry.log").c_str(), L"a"))
                {
                    fprintf(
                        log,
                        "GEOMETRY_CREATION active=%u compiler_files=%u mesh_registry=%u draw_packets=%u commands=%u "
                        "actual_draw_replacement=0 fg_substitution=0\n",
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
    if (!log)
        return;
    try
    {
        const auto creation = GetGeometryCreationStats();
        const auto objects = GetCyberpunkObjectStatus();
        const auto packets = GetCyberpunkDrawStatus();
        const auto commands = GetGeometryCommandStats();
        fprintf(log,
                "GEOMETRY_OBSERVE roots=%llu graphics=%llu compiled=%llu pending=%llu rejected=%llu "
                "batches=%llu appends=%llu identities=%llu engine_draws=%llu packet_rejected=%llu "
                "recordings=%llu capacity_rejected=%llu indexed=%llu packets=%llu instances=%llu "
                "draw_identities=%llu pipeline_ready=%llu bindings_ready=%llu frame=%u "
                "object_snapshot_rejected=%llu actual_draw_replacement=0 fg_substitution=0\n",
                creation.roots, creation.graphics, creation.cache.ready, creation.cache.pending,
                creation.cache.rejected, packets.batches, packets.appends, packets.identities, packets.draws,
                packets.rejected, commands.recordings, commands.capacityRejected, commands.indexed, commands.packets,
                commands.instances, commands.identities, commands.pipelinesReady, commands.bindingsReady,
                commands.lastFrame, objects.snapshotsRejected);
        if (!creation.cache.lastError.empty())
            fprintf(log, "GEOMETRY_COMPILER last_error=%s\n", creation.cache.lastError.c_str());
    }
    catch (...)
    {
    }
}
} // namespace GlassFg
