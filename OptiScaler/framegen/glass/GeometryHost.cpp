#include "pch.h"
#include "GeometryCoverageRecorder.h"
#include "ExperimentHost.h"
#include "GeometryCreation.h"
#include "CyberpunkLayout.h"
#include "CyberpunkObjects.h"
#include "CyberpunkDraws.h"
#include "GeometryCommands.h"
#include "GeometryViews.h"
#include "GeometryHealth.h"
#include "PackedMotionCapture.h"
#include "GlassMotionIdentity.h"
#include "GlassDebugControl.h"
#include "NativeHost.h"
#include <Util.h>
#include <chrono>
#include <mutex>
#include <thread>

namespace GlassFg
{
namespace
{
void refreshHealth() noexcept
{
    try
    {
        // One-second live control poll: no new thread and no per-draw cost.
        PollGlassDebugControl();
        // Deferred diagnostic write-out (reads back a finished dump and stops
        // as soon as nothing is pending).
        ServiceNativeDiagnostics();
        GeometryCreationStats creation;
        auto health = ReadGeometryHealth();
        if (TryGeometryCreationCounters(creation))
        {
            health.counts[GeometryRoots] = creation.roots;
            health.counts[GeometryPipelines] = creation.graphics + creation.streamGraphics;
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
                // The target-view registry supplies the product view identity and
                // must not depend on the diagnostic experiment switch.
                const bool views = commands && StartGeometryViews(device);
                // The experiment host and the packed object-motion capture share
                // one capture-owner slot, so the diagnostic host requires its own
                // explicit opt-in file.
                const bool experimentCapture =
                    draws && commands && std::filesystem::is_regular_file(directory / L"Glass" / L"experiment-capture.enable");
                if (draws && commands && !(experimentCapture && StartExperimentHost(device, directory / L"Glass")))
                    StartGeometryCoverageRecorder(device, compiler, directory / L"Glass" / L"capture-objects.request");
                GeometryHealth health;
                health.capabilities = GeometryStarted | (files ? GeometryCompiler : 0u) |
                                      (ready ? GeometryCreationHooks : 0u) |
                                      (GetCyberpunkLayout(GetModuleHandleW(nullptr)) ? GeometryEngineLayout : 0u) |
                                      (objects ? GeometryObjectHooks : 0u) | (draws ? GeometryDrawHooks : 0u) |
                                      (commands ? GeometryCommandHooks : 0u);
                health.sampledMs = GetTickCount64();
                PublishGeometryHealth(health);
                GeometryTelemetry::refresh.store(refreshHealth, std::memory_order_release);
                // The live channel and the periodic log must not depend on the
                // game reaching the FG path: menus and loading screens never
                // evaluate DLSS-G. One 1 Hz thread keeps the tick alive; the
                // existing frame hooks stay the primary source.
                std::thread([] {
                    for (;;)
                    {
                        std::this_thread::sleep_for(std::chrono::milliseconds(1000));
                        RefreshGeometryHealthIfNeeded(GetTickCount64());
                    }
                }).detach();
                if (FILE* log = _wfopen((directory / L"OptiScaler.Glass.Geometry.log").c_str(), L"a"))
                {
                    fprintf(
                        log,
                        "GEOMETRY_CREATION active=%u compiler_files=%u mesh_registry=%u draw_packets=%u commands=%u target_views=%u "
                        "engine_layout=relocatable_profiles actual_draw_replacement=0 fg_substitution=0\n",
                        ready, files, objects, draws, commands, views);
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
                          creation.graphics + creation.streamGraphics,
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
        const auto views = GetGeometryViewStats();
        fprintf(log, "GEOMETRY_VIEWS active=%u healthy=%u heaps=%llu writes=%llu copies=%llu lookups=%llu misses=%llu\n",
                views.active, views.healthy, views.heaps, views.writes, views.copies, views.lookups, views.misses);
        fprintf(log, "GEOMETRY_HEALTH capabilities=%u frame=%u object_capture=%llu fg_replacements=%llu reason=%s\n",
                health.capabilities, health.frame, health.counts[GeometryCaptureDraws],
                health.counts[GeometryFgReplacements], health.reason(health.sampledMs));
        fprintf(log,
                "GEOMETRY_OBSERVE roots=%llu graphics=%llu streams=%llu stream_graphics=%llu "
                "stream_compute=%llu stream_rejected=%llu compiled=%llu pending=%llu rejected=%llu "
                "observed=%llu observation_filtered=%llu observation_invalid=%llu observation_capacity_rejected=%llu observation_bytes=%llu "
                "batches=%llu appends=%llu identities=%llu engine_draws=%llu packet_rejected=%llu "
                "recordings=%llu capacity_rejected=%llu indexed=%llu packets=%llu instances=%llu "
                "draw_identities=%llu pipeline_ready=%llu bindings_ready=%llu frame=%u "
                "object_snapshot_rejected=%llu signatures=%llu indirect_known=%llu indirect_unknown=%llu "
                "actual_draw_replacement=0 fg_substitution=0\n",
                creation.roots, creation.graphics, creation.streams, creation.streamGraphics,
                creation.streamNonGraphics, creation.streamRejected, creation.cache.ready, creation.cache.pending,
                creation.cache.rejected, creation.observation.retained, creation.observation.filtered,
                creation.observation.invalid, creation.observation.capacityRejected,
                static_cast<unsigned long long>(creation.observation.retainedBytes),
                packets.batches, packets.appends, packets.identities, packets.draws,
                packets.rejected, commands.recordings, commands.capacityRejected, commands.indexed, commands.packets,
                commands.instances, commands.identities, commands.pipelinesReady, commands.bindingsReady,
                commands.lastFrame, objects.snapshotsRejected, commands.signatures, commands.indirectKnown,
                commands.indirectUnknown);
        if (!creation.cache.lastError.empty())
            fprintf(log, "GEOMETRY_COMPILER last_error=%s\n", creation.cache.lastError.c_str());
        const auto packed = ReadPackedMotionCaptureStatus();
        fprintf(log,
                "GEOMETRY_PACKED initialized=%u healthy=%u registered=%u extent=%ux%u admitted=%llu captured_frames=%llu "
                "fg_frames=%llu missing_pipeline=%llu unknown_identity=%llu topology_rejected=%llu mapping_overflow=%llu "
                "history_overflow=%llu slot_busy=%llu ordering_rejected=%llu no_fg_frame=%llu no_fg_queue=%llu "
                "packed_ready=%llu packed_rejected=%llu acquire_no_candidate=%llu acquire_stale=%llu "
                "acquire_ambiguous=%llu acquire_consumer_busy=%llu coverage_fallback=%llu\n",
                packed.initialized, packed.healthy, packed.registered, packed.width, packed.height,
                packed.admittedDraws, packed.capturedFrames, packed.fgFrames, packed.missingPipeline,
                packed.unknownIdentity, packed.topologyRejected, packed.mappingOverflow, packed.historyOverflow,
                packed.slotBusy, packed.orderingRejected, packed.noFgFrame, packed.noFgQueue,
                creation.cache.packedReady, creation.cache.packedRejected, packed.acquireNoCandidate,
                packed.acquireStalePair, packed.acquireAmbiguous, packed.acquireConsumerBusy,
                static_cast<unsigned long long>(ReadPackedCoverageFallbackCount()));
        fprintf(log,
                "GEOMETRY_PACKED_SPLIT unknown_owner_span=%llu unknown_resolve=%llu unknown_owner_mismatch=%llu "
                "unknown_field_mismatch=%llu unknown_no_array_generation=%llu raster_rejected=%llu shape_rejected=%llu "
                "viewport_rejected=%llu\n",
                packed.unknownOwnerSpan, packed.unknownResolve, packed.unknownOwnerMismatch,
                packed.unknownFieldMismatch, packed.unknownNoArrayGeneration, packed.rasterRejected,
                packed.shapeRejected, packed.viewportRejected);
        if (!creation.cache.lastPackedError.empty())
            fprintf(log, "GEOMETRY_PACKED_ERROR last_error=%s\n", creation.cache.lastPackedError.c_str());
        const auto identity = ReadGlassMotionIdentityStats();
        fprintf(log,
                "GEOMETRY_IDENTITY resolved=%llu rejected=%llu no_owner=%llu no_view=%llu no_lifetime=%llu "
                "no_element_index=%llu no_element_parent=%llu no_element_order=%llu\n",
                identity.resolved, identity.rejected, identity.noOwner, identity.noView, identity.noLifetime,
                identity.noElementIndex, identity.noElementParent, identity.noElementOrder);
        fprintf(log,
                "GEOMETRY_PARENT no_flag=%llu no_entry=%llu no_ticket=%llu no_slot=%llu no_mesh=%llu "
                "no_header=%llu no_selection=%llu seeded=%llu\n",
                packets.parentNoFlag, packets.parentNoEntry, packets.parentNoTicket, packets.parentNoSlot,
                packets.parentNoMesh, packets.parentNoHeader, packets.parentNoSelection, packets.parentSeeded);
        std::fprintf(log,
                     "GEOMETRY_SELECTION grouped=%llu non_global=%llu range=%llu\n",
                     static_cast<unsigned long long>(packets.parentNoSelectionGrouped),
                     static_cast<unsigned long long>(packets.parentNoSelectionNonGlobal),
                     static_cast<unsigned long long>(packets.parentNoSelectionRange));
        std::fprintf(log,
                     "GEOMETRY_ARRAY_ORDER grouped=%llu compared=%llu permuted=%llu changed=%llu "
                     "same_address=%llu distinct_address=%llu\n",
                     static_cast<unsigned long long>(packets.arrayProbeGrouped),
                     static_cast<unsigned long long>(packets.arrayProbeCompared),
                     static_cast<unsigned long long>(packets.arrayProbePermuted),
                     static_cast<unsigned long long>(packets.arrayProbeChanged),
                     static_cast<unsigned long long>(packets.arrayProbeSameAddress),
                     static_cast<unsigned long long>(packets.arrayProbeDistinctAddress));
        fprintf(log, "GEOMETRY_CHUNKS unknown=");
        for (const auto& entry : packed.unknownChunks)
            if (entry.count) std::fprintf(log, "%u:%llu,", entry.chunk, static_cast<unsigned long long>(entry.count));
        std::fprintf(log, " topology=");
        for (const auto& entry : packed.topologyChunks)
            if (entry.count) std::fprintf(log, "%u:%llu,", entry.chunk, static_cast<unsigned long long>(entry.count));
        std::fprintf(log, " missing=");
        for (const auto& entry : packed.missingChunks)
            if (entry.count) std::fprintf(log, "%u:%llu,", entry.chunk, static_cast<unsigned long long>(entry.count));
        std::fprintf(log, "\n");
        // N-1 stability: inserted grows when an element key changes between
        // frames, which means that element had no usable previous transform.
        std::fprintf(log,
                     "GEOMETRY_HISTORY hits=%llu inserted=%llu reclaimed=%llu rejected_topology=%llu set_full=%llu "
                     "arena_full=%llu arena_reclaimed=%llu live=%u\n",
                     static_cast<unsigned long long>(packed.historyHits),
                     static_cast<unsigned long long>(packed.historyInserted),
                     static_cast<unsigned long long>(packed.historyReclaimed),
                     static_cast<unsigned long long>(packed.historyRejectedTopology),
                     static_cast<unsigned long long>(packed.historySetFull),
                     static_cast<unsigned long long>(packed.historyArenaFull),
                     static_cast<unsigned long long>(packed.historyArenaReclaimed), packed.historyLive);
    }
    catch (...)
    {
    }
}
} // namespace GlassFg
