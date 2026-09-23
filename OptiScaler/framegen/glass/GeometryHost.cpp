#include "pch.h"
#include "GeometryCoverageRecorder.h"
#include "ExperimentHost.h"
#include "GeometryCreation.h"
#include "CyberpunkLayout.h"
#include "CyberpunkObjects.h"
#include "CyberpunkDraws.h"
#include "CyberpunkGroups.h"
#include "GeometryCommands.h"
#include "GeometryViews.h"
#include "GeometryHealth.h"
#include "GeometryGateTrace.h"
#include "GeometryPipelineCache.h"
#include "PackedMotionCapture.h"
#include "GlassMotionIdentity.h"
#include "GlassDebugControl.h"
#include "GlassControls.h"
#include "GlassHookProbe.h"
#include "NativeHost.h"
#include "GlassHostTiming.h"
#include <Util.h>
#include <State.h>
#include <algorithm>
#include <atomic>
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
        // Degradation flag for the window text and the log: a full history arena
        // is the measured state in which the object correction stops covering
        // the scene even though the replacement itself keeps running.
        {
            const auto packed = ReadPackedMotionCaptureStatus();
            PublishGeometryMotionDegraded(packed.historyArenaPages != 0 &&
                                          packed.historyArenaUsedPages >= packed.historyArenaPages &&
                                          packed.historyArenaLargestFree == 0);
        }
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
                // Grouped instance arrays: the element's original array index is
                // published from the engine's own grouped update so the motion
                // history is keyed by the object, not by the draw ordinal.
                const bool groups = draws && InitializeCyberpunkGroups(GetModuleHandleW(nullptr));
                const bool commands = ready && StartGeometryCommands(device);
                // The native state observer was installed lazily inside the first
                // frame generation evaluation, which put its Detours transaction
                // (thread enumeration plus suspend) on the engine render thread at
                // focus regain. Device creation runs before the render thread pool
                // exists and no frame is being presented, so it is installed here.
                const bool observerReady = commands && PreinstallNativeObserver(device);
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
                                      (groups ? GeometryGroupHooks : 0u) |
                                      (commands ? GeometryCommandHooks : 0u);
                health.sampledMs = GetTickCount64();
                PublishGeometryHealth(health);
                // The packed object-motion HLSL compile (measured 160.85ms inside
                // the first frame generation evaluation on 2026-09-16 05:17:34)
                // is the moment the engine rebuilds its frame generation lists,
                // which is when the game gets focus back. Compiling it from the
                // device-creation path leaves only the per-session pipeline for
                // the render thread.
                WarmPackedShaderOnce();
                // The hot hooks stay idle until the settings layer has run, so
                // the load and the TSC calibration must not be left to the first
                // render-thread call: both would stall a frame there.
                (void)ReadControls();
                WarmHookCostProbe();
                GeometryTelemetry::refresh.store(refreshHealth, std::memory_order_release);
                // The live channel and the periodic log must not depend on the
                // game reaching the FG path: menus and loading screens never
                // evaluate DLSS-G. One 1 Hz thread keeps the tick alive; the
                // existing frame hooks stay the primary source.
                std::thread([] {
                    std::uint64_t reportedMs = 0;
                    for (;;)
                    {
                        std::this_thread::sleep_for(std::chrono::milliseconds(1000));
                        RefreshGeometryHealthIfNeeded(GetTickCount64());
                        // The module report formats ~30 lines and flushes the
                        // log. It used to run inside the frame generation
                        // callback, which is the engine's render thread.
                        const auto now = GetTickCount64();
                        if (now - reportedMs >= 2000)
                        {
                            reportedMs = now;
                            ReportNativeHostLog();
                        }
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
        PublishGeometryMotionDegraded(packed.historyArenaPages != 0 &&
                                      packed.historyArenaUsedPages >= packed.historyArenaPages &&
                                      packed.historyArenaLargestFree == 0);
        if (packed.historyArenaPages != 0 && packed.historyArenaUsedPages >= packed.historyArenaPages &&
            packed.historyArenaLargestFree == 0)
            std::fprintf(log,
                         "GEOMETRY_PACKED_DEGRADED history_arena_full used=%u/%u live=%u arena_full=%llu "
                         "frame=%u retired=%u pinned=%u slot_recovered=%llu\n",
                         packed.historyArenaUsedPages, packed.historyArenaPages, packed.historyLive,
                         static_cast<unsigned long long>(packed.historyArenaFull), packed.historyFrame,
                         packed.historyRetiredFrame, packed.historyPinnedEntries,
                         static_cast<unsigned long long>(packed.slotRecovered));
        fprintf(log,
                "GEOMETRY_PACKED initialized=%u healthy=%u registered=%u extent=%ux%u admitted=%llu captured_frames=%llu "
                "fg_frames=%llu missing_pipeline=%llu delta_missing_draws=%llu unknown_identity=%llu topology_rejected=%llu mapping_overflow=%llu "
                "history_overflow=%llu slot_busy=%llu ordering_rejected=%llu no_fg_frame=%llu no_fg_queue=%llu "
                "packed_ready=%llu packed_rejected=%llu packed_delta_missing=%llu acquire_no_candidate=%llu acquire_stale=%llu "
                "acquire_ambiguous=%llu acquire_consumer_busy=%llu coverage_fallback=%llu "
                "packed_opaqueprobe_ready=%llu packed_opaqueprobe_rejected=%llu\n",
                packed.initialized, packed.healthy, packed.registered, packed.width, packed.height,
                packed.admittedDraws, packed.capturedFrames, packed.fgFrames, packed.missingPipeline,
                static_cast<unsigned long long>(packed.deltaMissingDraws),
                packed.unknownIdentity, packed.topologyRejected, packed.mappingOverflow, packed.historyOverflow,
                packed.slotBusy, packed.orderingRejected, packed.noFgFrame, packed.noFgQueue,
                creation.cache.packedReady, creation.cache.packedRejected,
                static_cast<unsigned long long>(creation.cache.packedDeltaMissing), packed.acquireNoCandidate,
                packed.acquireStalePair, packed.acquireAmbiguous, packed.acquireConsumerBusy,
                static_cast<unsigned long long>(ReadPackedCoverageFallbackCount()),
                creation.cache.packedOpaqueProbeReady, creation.cache.packedOpaqueProbeRejected);
        fprintf(log,
                "GEOMETRY_PACKED_SPLIT unknown_owner_span=%llu unknown_resolve=%llu unknown_owner_mismatch=%llu "
                "unknown_field_mismatch=%llu unknown_no_array_generation=%llu raster_rejected=%llu shape_rejected=%llu "
                "viewport_rejected=%llu\n",
                packed.unknownOwnerSpan, packed.unknownResolve, packed.unknownOwnerMismatch,
                packed.unknownFieldMismatch, packed.unknownNoArrayGeneration, packed.rasterRejected,
                packed.shapeRejected, packed.viewportRejected);
        // Why ReadCyberpunkMeshShape refused a draw. Everything above is filed
        // under shape_rejected, which cannot say whether the draw lost its
        // flush correlation or the mesh chunk itself was unreadable.
        const auto shape = ReadCyberpunkShapeStats();
        fprintf(log,
                "GEOMETRY_SHAPE_SPLIT no_flush=%llu no_batch=%llu empty_objects=%llu mesh_chunk=%llu frame=%llu "
                "instances=%llu records=%llu header=%llu buffers=%llu chunk_read=%llu unstable_chunk=%llu "
                "header_moved=%llu buffers_moved=%llu fields=%llu fields_vertices=%llu fields_indices=%llu "
                "fields_streams=%llu fields_stream_range=%llu fields_index_type=%llu fields_index_offset=%llu "
                "total=%llu\n",
                static_cast<unsigned long long>(shape.noFlush), static_cast<unsigned long long>(shape.noBatch),
                static_cast<unsigned long long>(shape.emptyObjects),
                static_cast<unsigned long long>(shape.meshChunk), static_cast<unsigned long long>(shape.frame),
                static_cast<unsigned long long>(shape.instances), static_cast<unsigned long long>(shape.records),
                static_cast<unsigned long long>(shape.header), static_cast<unsigned long long>(shape.buffers),
                static_cast<unsigned long long>(shape.chunkRead),
                static_cast<unsigned long long>(shape.unstableChunk),
                static_cast<unsigned long long>(shape.headerMoved),
                static_cast<unsigned long long>(shape.buffersMoved),
                static_cast<unsigned long long>(shape.fields),
                static_cast<unsigned long long>(shape.fieldsVertices),
                static_cast<unsigned long long>(shape.fieldsIndices),
                static_cast<unsigned long long>(shape.fieldsStreams),
                static_cast<unsigned long long>(shape.fieldsStreamRange),
                static_cast<unsigned long long>(shape.fieldsIndexType),
                static_cast<unsigned long long>(shape.fieldsIndexOffset),
                static_cast<unsigned long long>(shape.rejected));
        // Which field check refused a draw is only half of the answer; the
        // first few refusals also print the numbers that decided them.
        static std::size_t shapeSamplesPrinted = 0;
        CyberpunkShapeSample shapeSample[4] {};
        const auto shapeSampleCount = ReadCyberpunkShapeSamples(shapeSample, 4);
        for (; shapeSamplesPrinted < shapeSampleCount; ++shapeSamplesPrinted)
        {
            const auto& item = shapeSample[shapeSamplesPrinted];
            fprintf(log,
                    "GEOMETRY_SHAPE_FIELDS n=%llu reason=%u vertices=%u indices=%u flush_indices=%u streams=%u "
                    "index_type=%u index_offset=%u mesh=%llx chunk=%u\n",
                    static_cast<unsigned long long>(shapeSamplesPrinted), item.reason, item.vertices,
                    item.indices, item.flushIndices, item.streams, item.indexType, item.indexOffset,
                    static_cast<unsigned long long>(item.mesh), item.chunk);
        }
        if (!creation.cache.lastPackedError.empty())
            fprintf(log, "GEOMETRY_PACKED_ERROR last_error=%s\n", creation.cache.lastPackedError.c_str());
        // Bounded admission report. The packed counters above only show the
        // gates that were given a counter; the silent ones (no rewritten
        // pipeline, no replayable bindings, prepare's early returns) are the
        // common case for ordinary content and were invisible before this line.
        if (GateArmed())
        {
            const auto& gate = Gate();
            fprintf(log,
                    "GEOMETRY_GATE draws=%llu no_pipeline=%llu no_bindings=%llu no_record=%llu no_owner=%llu "
                    "prepare_lock=%llu prepare_failed=%llu prepare_command=%llu prepare_frameid=%llu "
                    "prepare_instances=%llu prepare_mapping=%llu prepare_pipeline=%llu prepare_notpacked=%llu "
                    "prepare_root=%llu prepare_raster=%llu prepare_shape=%llu prepare_viewport=%llu "
                    "prepare_frameslot=%llu prepare_ordering=%llu prepare_span=%llu prepare_history=%llu "
                    "prepare_noelement=%llu bind_rejected=%llu captured=%llu "
                    "unseen_pipeline_probes=%llu unseen_pipeline_distinct=%llu\n",
                    gate.stage[GateObjectDraws].load(), gate.stage[GateNoPipeline].load(),
                    gate.stage[GateNoBindings].load(), gate.stage[GateNoRecord].load(),
                    gate.stage[GateNoOwner].load(),
                    gate.stage[GatePrepareLock].load(), gate.stage[GatePrepareFailed].load(),
                    gate.stage[GatePrepareCommand].load(), gate.stage[GatePrepareFrameId].load(),
                    gate.stage[GatePrepareInstances].load(), gate.stage[GatePrepareMapping].load(),
                    gate.stage[GatePreparePipeline].load(), gate.stage[GatePrepareNotPacked].load(),
                    gate.stage[GatePrepareRoot].load(), gate.stage[GatePrepareRaster].load(),
                    gate.stage[GatePrepareShape].load(), gate.stage[GatePrepareViewport].load(),
                    gate.stage[GatePrepareFrameSlot].load(), gate.stage[GatePrepareOrdering].load(),
                    gate.stage[GatePrepareSpan].load(), gate.stage[GatePrepareHistory].load(),
                    gate.stage[GatePrepareNoElement].load(), gate.stage[GateBindRejected].load(),
                    gate.stage[GateCaptured].load(), gate.unseenPipelineProbes.load(),
                    gate.unseenPipelineDistinct.load());
            fprintf(log,
                    "GEOMETRY_UNSEEN unknown=%llu accepted_not_ready=%llu rejected_transparent=%llu "
                    "rejected_opaque=%llu\n",
                    static_cast<unsigned long long>(gate.unseenUnknown.load()),
                    static_cast<unsigned long long>(gate.unseenAcceptedNotReady.load()),
                    static_cast<unsigned long long>(gate.unseenRejectedTransparent.load()),
                    static_cast<unsigned long long>(gate.unseenRejectedOpaque.load()));
            const auto& census = GateCreationCensus();
            fprintf(log, "GEOMETRY_CREATION created=%llu accepted=%llu rejected=%llu rejected_transparent=%llu\n",
                    static_cast<unsigned long long>(census.created.load()),
                    static_cast<unsigned long long>(census.accepted.load()),
                    static_cast<unsigned long long>(census.rejected.load()),
                    static_cast<unsigned long long>(census.rejectedTransparent.load()));
            fprintf(log, "GEOMETRY_CREATION_REASON root=%llu vertex=%llu pixel=%llu vertex_bytes=%llu "
                         "pixel_bytes=%llu targets=%llu samples=%llu stages=%llu stream=%llu topology=%llu "
                         "layout=%llu depth_write=%llu stencil=%llu blend=%llu root_unknown=%llu limits=%llu "
                         "compilation_off=%llu\n",
                    static_cast<unsigned long long>(census.reason[GateCandidateRoot].load()),
                    static_cast<unsigned long long>(census.reason[GateCandidateVertex].load()),
                    static_cast<unsigned long long>(census.reason[GateCandidatePixel].load()),
                    static_cast<unsigned long long>(census.reason[GateCandidateVertexBytes].load()),
                    static_cast<unsigned long long>(census.reason[GateCandidatePixelBytes].load()),
                    static_cast<unsigned long long>(census.reason[GateCandidateTargets].load()),
                    static_cast<unsigned long long>(census.reason[GateCandidateSamples].load()),
                    static_cast<unsigned long long>(census.reason[GateCandidateShaderStages].load()),
                    static_cast<unsigned long long>(census.reason[GateCandidateStreamOutput].load()),
                    static_cast<unsigned long long>(census.reason[GateCandidateTopology].load()),
                    static_cast<unsigned long long>(census.reason[GateCandidateInputLayout].load()),
                    static_cast<unsigned long long>(census.reason[GateCandidateDepthWrite].load()),
                    static_cast<unsigned long long>(census.reason[GateCandidateStencilWrite].load()),
                    static_cast<unsigned long long>(census.reason[GateCandidateBlend].load()),
                    static_cast<unsigned long long>(census.reason[GateCandidateRootUnknown].load()),
                    static_cast<unsigned long long>(census.reason[GateCandidateLimits].load()),
                    static_cast<unsigned long long>(census.reason[GateCandidateCompilationOff].load()));
            fprintf(log, "GEOMETRY_CREATION_TRANSPARENT depth_write=%llu stencil=%llu blend=%llu root_unknown=%llu "
                         "limits=%llu samples=%llu topology=%llu layout=%llu\n",
                    static_cast<unsigned long long>(census.transparentReason[GateCandidateDepthWrite].load()),
                    static_cast<unsigned long long>(census.transparentReason[GateCandidateStencilWrite].load()),
                    static_cast<unsigned long long>(census.transparentReason[GateCandidateBlend].load()),
                    static_cast<unsigned long long>(census.transparentReason[GateCandidateRootUnknown].load()),
                    static_cast<unsigned long long>(census.transparentReason[GateCandidateLimits].load()),
                    static_cast<unsigned long long>(census.transparentReason[GateCandidateSamples].load()),
                    static_cast<unsigned long long>(census.transparentReason[GateCandidateTopology].load()),
                    static_cast<unsigned long long>(census.transparentReason[GateCandidateInputLayout].load()));
            // Per-pipeline coverage since gate=on (GeometryPipelineCache.h). Every
            // report prints the totals line. The entry lines follow only when a
            // counter moved since the previous report: most drawn first, at most
            // 256, pipelines with draws only. vs/ps are the sha256 prefixes of the
            // native catalog (all-cache-techniques.json). id is the pipeline
            // column of the dump id table (dump-<serial>-pipelines.txt).
            {
                static std::atomic<std::uint64_t> reportedDraws { UINT64_MAX };
                std::vector<GeometryPipelineCoverage> pipelines;
                GeometryPipelineCache::readCoverage(pipelines);
                std::uint64_t draws = 0, captures = 0, drawn = 0, captured = 0;
                for (const auto& item : pipelines)
                {
                    draws += item.draws;
                    captures += item.captures;
                    drawn += item.draws != 0 ? 1u : 0u;
                    captured += item.captures != 0 ? 1u : 0u;
                }
                const bool moved = reportedDraws.exchange(draws, std::memory_order_relaxed) != draws;
                const auto shown = moved ? (std::min)(drawn, std::uint64_t(256)) : 0;
                fprintf(log, "GEOMETRY_PIPELINES n=%zu drawn=%llu captured=%llu shown=%llu draws=%llu captures=%llu\n",
                        pipelines.size(), static_cast<unsigned long long>(drawn),
                        static_cast<unsigned long long>(captured), static_cast<unsigned long long>(shown),
                        static_cast<unsigned long long>(draws), static_cast<unsigned long long>(captures));
                std::sort(pipelines.begin(), pipelines.end(),
                          [](const GeometryPipelineCoverage& a, const GeometryPipelineCoverage& b)
                          { return a.draws != b.draws ? a.draws > b.draws : a.identity < b.identity; });
                for (std::size_t i = 0; i < shown; ++i)
                {
                    const auto& item = pipelines[i];
                    fprintf(log,
                            "GEOMETRY_PIPELINE id=%llu vs=%016llx ps=%016llx kind=%s history=%u draws=%llu "
                            "captures=%llu graft=%llu array=%llu array_rejected=%llu\n",
                            static_cast<unsigned long long>(item.identity),
                            static_cast<unsigned long long>(item.vertexHash),
                            static_cast<unsigned long long>(item.pixelHash), GeometryGraftKindName(item.kind),
                            item.history ? 1u : 0u, static_cast<unsigned long long>(item.draws),
                            static_cast<unsigned long long>(item.captures),
                            static_cast<unsigned long long>(item.graft), static_cast<unsigned long long>(item.array),
                            static_cast<unsigned long long>(item.arrayRejected));
                }
            }
        }
        const auto identity = ReadGlassMotionIdentityStats();
        fprintf(log,
                "GEOMETRY_IDENTITY resolved=%llu rejected=%llu no_owner=%llu no_view=%llu no_lifetime=%llu "
                "no_element_index=%llu no_element_parent=%llu no_element_order=%llu "
                "element_mapped=%llu element_unmapped=%llu\n",
                identity.resolved, identity.rejected, identity.noOwner, identity.noView, identity.noLifetime,
                identity.noElementIndex, identity.noElementParent, identity.noElementOrder, identity.elementMapped,
                identity.elementUnmapped);
        const auto groups = ReadGroupedArrayStats();
        fprintf(log,
                "GEOMETRY_GROUPS grouped=%llu staged=%llu published=%llu aborted=%llu append=%llu in_range=%llu "
                "outside=%llu dropped=%llu sample_calls=%llu sample_cycles=%llu sample_max_cycles=%llu\n",
                groups.groupedCalls, groups.staged, groups.published, groups.aborted, groups.appendCalls,
                groups.appendInRange, groups.appendOutside, groups.appendDropped, groups.sampleCalls,
                groups.sampleCycles, groups.sampleMaxCycles);
        fprintf(log,
                "GEOMETRY_PARENT no_flag=%llu no_entry=%llu no_ticket=%llu no_slot=%llu no_mesh=%llu "
                "no_header=%llu no_selection=%llu seeded=%llu memo_hit=%llu memo_miss=%llu\n",
                packets.parentNoFlag, packets.parentNoEntry, packets.parentNoTicket, packets.parentNoSlot,
                packets.parentNoMesh, packets.parentNoHeader, packets.parentNoSelection, packets.parentSeeded,
                packets.parentMemoHits, packets.parentMemoMisses);
        std::fprintf(log,
                     "GEOMETRY_SELECTION grouped=%llu non_global=%llu range=%llu\n",
                     static_cast<unsigned long long>(packets.parentNoSelectionGrouped),
                     static_cast<unsigned long long>(packets.parentNoSelectionNonGlobal),
                     static_cast<unsigned long long>(packets.parentNoSelectionRange));
        std::fprintf(log,
                     "GEOMETRY_PACKET_LOCAL count1=%llu count_more=%llu count_more_skin=%llu "
                     "gate_pass=%llu gate_count1=%llu gate_skin=%llu\n",
                     static_cast<unsigned long long>(packets.parentNonGlobalCount1),
                     static_cast<unsigned long long>(packets.parentNonGlobalCountMore),
                     static_cast<unsigned long long>(packets.parentNonGlobalCountMoreSkin),
                     static_cast<unsigned long long>(packets.arrayProbeLocalGatePass),
                     static_cast<unsigned long long>(packets.arrayProbeLocalGateCount),
                     static_cast<unsigned long long>(packets.arrayProbeLocalGateSkin));
        std::fprintf(log,
                     "GEOMETRY_ARRAY_ORDER grouped=%llu compared=%llu permuted=%llu changed=%llu "
                     "local=%llu local_compared=%llu local_permuted=%llu local_changed=%llu "
                     "same_address=%llu distinct_address=%llu local_same_address=%llu "
                     "local_distinct_address=%llu local_unreadable=%llu local_base_moved=%llu\n",
                     static_cast<unsigned long long>(packets.arrayProbeGrouped),
                     static_cast<unsigned long long>(packets.arrayProbeCompared),
                     static_cast<unsigned long long>(packets.arrayProbePermuted),
                     static_cast<unsigned long long>(packets.arrayProbeChanged),
                     static_cast<unsigned long long>(packets.arrayProbeLocal),
                     static_cast<unsigned long long>(packets.arrayProbeLocalCompared),
                     static_cast<unsigned long long>(packets.arrayProbeLocalPermuted),
                     static_cast<unsigned long long>(packets.arrayProbeLocalChanged),
                     static_cast<unsigned long long>(packets.arrayProbeSameAddress),
                     static_cast<unsigned long long>(packets.arrayProbeDistinctAddress),
                     static_cast<unsigned long long>(packets.arrayProbeLocalSameAddress),
                     static_cast<unsigned long long>(packets.arrayProbeLocalDistinctAddress),
                     static_cast<unsigned long long>(packets.arrayProbeLocalUnreadable),
                     static_cast<unsigned long long>(packets.arrayProbeLocalBaseMoved));
        fprintf(log, "GEOMETRY_CHUNKS unknown=");
        for (const auto& entry : packed.unknownChunks)
            if (entry.count) std::fprintf(log, "%u:%llu,", entry.chunk, static_cast<unsigned long long>(entry.count));
        std::fprintf(log, " topology=");
        for (const auto& entry : packed.topologyChunks)
            if (entry.count) std::fprintf(log, "%u:%llu,", entry.chunk, static_cast<unsigned long long>(entry.count));
        std::fprintf(log, " missing=");
        for (const auto& entry : packed.missingChunks)
            if (entry.count) std::fprintf(log, "%u:%llu,", entry.chunk, static_cast<unsigned long long>(entry.count));
        std::fprintf(log, " delta_missing=");
        for (const auto& entry : packed.deltaMissingChunks)
            if (entry.count) std::fprintf(log, "%u:%llu,", entry.chunk, static_cast<unsigned long long>(entry.count));
        std::fprintf(log, "\n");
        // N-1 stability: inserted grows when an element key changes between
        // frames, which means that element had no usable previous transform.
        std::fprintf(log,
                     "GEOMETRY_HISTORY hits=%llu inserted=%llu reclaimed=%llu rejected_topology=%llu set_full=%llu "
                     "arena_full=%llu arena_reclaimed=%llu live=%u arena_pages=%u arena_used=%u arena_free_max=%u "
                     "frame=%u retired=%u pinned=%u\n",
                     static_cast<unsigned long long>(packed.historyHits),
                     static_cast<unsigned long long>(packed.historyInserted),
                     static_cast<unsigned long long>(packed.historyReclaimed),
                     static_cast<unsigned long long>(packed.historyRejectedTopology),
                     static_cast<unsigned long long>(packed.historySetFull),
                     static_cast<unsigned long long>(packed.historyArenaFull),
                     static_cast<unsigned long long>(packed.historyArenaReclaimed), packed.historyLive,
                     packed.historyArenaPages, packed.historyArenaUsedPages, packed.historyArenaLargestFree,
                     packed.historyFrame, packed.historyRetiredFrame, packed.historyPinnedEntries);
        std::fprintf(log, "GEOMETRY_ARENA_FULL sizes=");
        for (unsigned i = 0; i < packed.historyArenaFullPageCount; ++i)
            std::fprintf(log, "%s%u", i ? "," : "", packed.historyArenaFullPages[i]);
        std::fprintf(log, "\n");
        // Which engine mesh families the packed capture actually admitted. A
        // visible transparent object (a glass, a railing) missing from this list
        // was never captured, which no per-chunk rejection counter can show.
        for (unsigned i = 0; i < packed.admittedSpanFamilyCount; ++i)
        {
            const auto& family = packed.admittedSpans[i];
            std::fprintf(log, "GEOMETRY_SPAN chunk=%u mesh=%u verts=%u spans=%llu\n", family.chunk, family.mesh,
                         family.vertices, static_cast<unsigned long long>(family.count));
        }
        std::fprintf(log, "GEOMETRY_SPAN_TOTAL spans=%llu evicted=%llu families=%u\n",
                     static_cast<unsigned long long>(packed.frameSpanCount),
                     static_cast<unsigned long long>(packed.spanFamilyEvictions),
                     packed.admittedSpanFamilyCount);
        std::fprintf(log, "GEOMETRY_OVERFLOW chunks=");
        for (const auto& entry : packed.overflowChunks)
            if (entry.count) std::fprintf(log, "%u:%llu,", entry.chunk, static_cast<unsigned long long>(entry.count));
        std::fprintf(log, "\n");
        // Presented-frame rate against the engine frame rate. A ratio near 1
        // means frame generation is not running; 2/3/4 identifies the
        // multiplier the driver is actually presenting. This is the only
        // process-local proof that generated frames exist, and the game's own
        // counter reports the render rate, not the presented one.
        static std::atomic<std::uint64_t> presentSamples { 0 }, presentCount { 0 }, engineCount { 0 },
            sampleMs { 0 };
        const auto sampleNow = GetTickCount64();
        const auto previousMs = sampleMs.load(std::memory_order_relaxed);
        if (sampleNow >= previousMs + 2000)
        {
            UINT lastPresent = 0;
            IDXGISwapChain* swapchain = State::Instance().currentSwapchain;
            if (swapchain && SUCCEEDED(swapchain->GetLastPresentCount(&lastPresent)))
            {
                const auto previousPresent = presentCount.load(std::memory_order_relaxed);
                const auto previousEngine = engineCount.load(std::memory_order_relaxed);
                const auto seconds = double(sampleNow - previousMs) / 1000.0;
                if (previousMs && seconds > 0.0 && lastPresent >= previousPresent &&
                    packed.capturedFrames >= previousEngine)
                {
                    const auto presentDelta = std::uint64_t(lastPresent) - previousPresent;
                    const auto engineDelta = packed.capturedFrames - previousEngine;
                    const auto samples = presentSamples.load(std::memory_order_relaxed) + 1;
                    presentSamples.store(samples, std::memory_order_relaxed);
                    std::fprintf(log,
                                 "GEOMETRY_PRESENT presents=%llu engine_frames=%llu ratio=%.2f present_fps=%.1f "
                                 "engine_fps=%.1f sample=%llu epoch=%.3f\n",
                                 static_cast<unsigned long long>(presentDelta),
                                 static_cast<unsigned long long>(engineDelta),
                                 engineDelta ? double(presentDelta) / double(engineDelta) : 0.0,
                                 double(presentDelta) / seconds, double(engineDelta) / seconds,
                                 static_cast<unsigned long long>(samples), EpochSeconds());
                    {
                        // CPU cost of the frame generation callbacks. The render
                        // thread only adds; the health thread formats and resets,
                        // so this never allocates inside a submission.
                        const auto averageMs = [](const HostTiming& value)
                        {
                            const auto count = value.count.load(std::memory_order_relaxed);
                            return count ? double(value.totalUs.load(std::memory_order_relaxed)) / double(count) / 1000.0
                                         : 0.0;
                        };
                        const auto maximumMs = [](const HostTiming& value)
                        { return double(value.maxUs.load(std::memory_order_relaxed)) / 1000.0; };
                        const auto countOf = [](const HostTiming& value)
                        { return static_cast<unsigned long long>(value.count.load(std::memory_order_relaxed)); };
                        auto& evaluation = EvaluationTiming();
                        auto& submission = SubmissionTiming();
                        auto& capture = CaptureTiming();
                        auto& compose = ComposeTiming();
                        auto& composeQueue = ComposeQueueTiming();
                        auto& composeReset = ComposeResetTiming();
                        auto& composeRecord = ComposeRecordTiming();
                        auto& composeExecute = ComposeExecuteTiming();
                        auto& composeSignal = ComposeSignalTiming();
                        // CPU cost of the hooks themselves. This is the number
                        // that answers "what does the correction cost per
                        // frame": every hot hook is counted and one call in
                        // HookCostSampling is timed, then the samples are scaled
                        // by the engine frames in the same window.
                        // Per-thread slots are summed and reset here, so the hot
                        // path stays a lock-free increment in the owning thread.
                        const auto commandHooks = TakeHookCost(&HookCostSlot::command);
                        const auto indexedHooks = TakeHookCost(&HookCostSlot::indexed);
                        const auto runHooks = TakeHookCost(&HookCostSlot::run);
                        const auto appendHooks = TakeHookCost(&HookCostSlot::append);
                        const auto rigidHooks = TakeHookCost(&HookCostSlot::rigid);
                        const auto skinnedHooks = TakeHookCost(&HookCostSlot::skinned);
                        const auto observerHooks = TakeHookCost(&HookCostSlot::observer);
                        // The stored command family covers both shapes; the
                        // reported command total stays the sum of its parts.
                        const auto commandAll = SumHookCosts(commandHooks, indexedHooks);
                        const auto drawHooks = SumHookCosts(SumHookCosts(runHooks, appendHooks),
                                                            SumHookCosts(rigidHooks, skinnedHooks));
                        static std::atomic<std::uint32_t> hookFrame { 0 };
                        const auto previousHookFrame = hookFrame.exchange(commands.lastFrame, std::memory_order_relaxed);
                        const auto hookFrames =
                            commands.lastFrame > previousHookFrame ? commands.lastFrame - previousHookFrame : 0u;
                        // The probe times one call in HookCostSampling, so each
                        // family is scaled by calls/timed before it is compared
                        // with a measured frame cost. The timer is stopped around
                        // the engine's own call, so this is our code only.
                        const auto hookMs = [](const HookCostTotals& value)
                        { return HookCostScaledMs(value.selfCycles, value.calls, value.timed); };
                        const auto hookMaxMs = [](const HookCostTotals& value)
                        { return double(value.maxCycles) / TscHertz() * 1000.0; };
                        const auto hookMsPerFrame =
                            hookFrames ? (hookMs(commandAll) + hookMs(drawHooks) + hookMs(observerHooks)) /
                                             double(hookFrames)
                                       : 0.0;
                        std::fprintf(log,
                                     "GLASS_TIMING evaluate_n=%llu evaluate_ms=%.3f evaluate_max_ms=%.3f "
                                     "capture_n=%llu capture_ms=%.3f capture_max_ms=%.3f "
                                     "compose_n=%llu compose_ms=%.3f compose_max_ms=%.3f "
                                     "composeq_n=%llu composeq_ms=%.3f composeq_max_ms=%.3f "
                                     "cqreset_max_ms=%.3f cqrecord_max_ms=%.3f cqexec_max_ms=%.3f "
                                     "cqsignal_max_ms=%.3f submit_n=%llu submit_ms=%.3f submit_max_ms=%.3f "
                                     "hook_frames=%u "
                                     "hook_cmd_calls=%llu hook_cmd_timed=%llu hook_cmd_ms=%.3f hook_cmd_max_ms=%.3f "
                                     "hook_cmd_stalls=%llu "
                                     "hook_draw_calls=%llu hook_draw_timed=%llu hook_draw_ms=%.3f hook_draw_max_ms=%.3f "
                                     "hook_draw_stalls=%llu "
                                     "hook_indexed_calls=%llu hook_indexed_ms=%.3f "
                                     "hook_run_calls=%llu hook_run_ms=%.3f "
                                     "hook_append_calls=%llu hook_append_ms=%.3f "
                                     "hook_rigid_calls=%llu hook_rigid_ms=%.3f "
                                     "hook_skinned_calls=%llu hook_skinned_ms=%.3f "
                                     "hook_obs_calls=%llu hook_obs_timed=%llu hook_obs_ms=%.3f hook_obs_max_ms=%.3f "
                                     "hook_obs_stalls=%llu "
                                     "hook_ms_per_frame=%.4f "
                                     "epoch=%.3f\n",
                                     countOf(evaluation), averageMs(evaluation), maximumMs(evaluation),
                                     countOf(capture), averageMs(capture), maximumMs(capture), countOf(compose),
                                     averageMs(compose), maximumMs(compose), countOf(composeQueue),
                                     averageMs(composeQueue), maximumMs(composeQueue), maximumMs(composeReset),
                                     maximumMs(composeRecord), maximumMs(composeExecute), maximumMs(composeSignal),
                                     countOf(submission), averageMs(submission), maximumMs(submission), hookFrames,
                                     commandAll.calls, commandAll.timed, hookMs(commandAll),
                                     hookMaxMs(commandAll), commandAll.stalls, drawHooks.calls, drawHooks.timed,
                                     hookMs(drawHooks), hookMaxMs(drawHooks), drawHooks.stalls, indexedHooks.calls,
                                     hookMs(indexedHooks), runHooks.calls, hookMs(runHooks), appendHooks.calls,
                                     hookMs(appendHooks), rigidHooks.calls, hookMs(rigidHooks), skinnedHooks.calls,
                                     hookMs(skinnedHooks), observerHooks.calls,
                                     observerHooks.timed, hookMs(observerHooks), hookMaxMs(observerHooks),
                                     observerHooks.stalls, hookMsPerFrame,
                                     EpochSeconds());
                        // Stage split of the two dominant hook shapes. The
                        // timers ride the same 1/64 sample as the family
                        // total, so these numbers are per sampled call and are
                        // read against each other, not as a scaled total.
                        const auto stageTotals = TakeHookStages();
                        unsigned long long stageEntries = 0;
                        for (const auto value : stageTotals.calls)
                            stageEntries += value;
                        if (stageEntries)
                        {
                            const auto stageNs = [&](unsigned stage)
                            {
                                return stageTotals.calls[stage] ?
                                    double(stageTotals.cycles[stage]) / double(stageTotals.calls[stage]) /
                                        TscHertz() * 1.0e9 :
                                    0.0;
                            };
                            std::fprintf(log,
                                         "HOOK_STAGE append_read_n=%llu append_read_ns=%.0f "
                                         "append_parent_n=%llu append_parent_ns=%.0f "
                                         "append_tail_n=%llu append_tail_ns=%.0f "
                                         "indexed_head_n=%llu indexed_head_ns=%.0f "
                                         "indexed_tail_n=%llu indexed_tail_ns=%.0f "
                                         "append_ticket_ns=%.0f append_fields_ns=%.0f append_select_ns=%.0f "
                                         "indexed_read_ns=%.0f indexed_find_ns=%.0f indexed_pipeline_ns=%.0f\n",
                                         stageTotals.calls[HookStageAppendRead],
                                         stageNs(HookStageAppendRead),
                                         stageTotals.calls[HookStageAppendSelect],
                                         stageNs(HookStageAppendTicket) + stageNs(HookStageAppendFields) +
                                             stageNs(HookStageAppendSelect),
                                         stageTotals.calls[HookStageAppendTail],
                                         stageNs(HookStageAppendTail),
                                         stageTotals.calls[HookStageIndexedRead],
                                         stageNs(HookStageIndexedRead) + stageNs(HookStageIndexedFind) +
                                             stageNs(HookStageIndexedPipeline),
                                         stageTotals.calls[HookStageIndexedTail],
                                         stageNs(HookStageIndexedTail),
                                         stageNs(HookStageAppendTicket),
                                         stageNs(HookStageAppendFields),
                                         stageNs(HookStageAppendSelect),
                                         stageNs(HookStageIndexedRead),
                                         stageNs(HookStageIndexedFind),
                                         stageNs(HookStageIndexedPipeline));
                        }
                        std::fflush(log);
                        evaluation.reset();
                        submission.reset();
                        capture.reset();
                        compose.reset();
                        composeQueue.reset();
                        composeReset.reset();
                        composeRecord.reset();
                        composeExecute.reset();
                        composeSignal.reset();
                    }
                }
                presentCount.store(lastPresent, std::memory_order_relaxed);
                engineCount.store(packed.capturedFrames, std::memory_order_relaxed);
                sampleMs.store(sampleNow, std::memory_order_relaxed);
            }
            else
            {
                sampleMs.store(sampleNow, std::memory_order_relaxed);
            }
        }
    }
    catch (...)
    {
    }
}
} // namespace GlassFg
