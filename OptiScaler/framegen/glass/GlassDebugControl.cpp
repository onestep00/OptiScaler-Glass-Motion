#include "pch.h"
#include "GlassDebugControl.h"
#include "GlassControls.h"
#include "GlassMotionIdentity.h"
#include "NativeHost.h"
#include "PackedMotionCapture.h"
#include "GlassPluginHost.h"
#include "GlassArrayMapping.h"
#include "NvngxDlssgBridge.h"
#include "GeometryPipeline.h"
#include "GeometryPipelineCache.h"
#include "CyberpunkDraws.h"
#include "CyberpunkGroups.h"
#include "GeometryGateTrace.h"
#include "GlassHookProbe.h"
#include <Util.h>
#include <cstdio>
#include <fstream>

namespace GlassFg
{
namespace
{
std::atomic<bool> shaderReloadRequested = false;
std::atomic<bool> readyWritten = false;

std::filesystem::path controlPath(const wchar_t* name)
{
    return Util::DllPath().parent_path() / L"Glass" / name;
}

// The live channel must be verifiable from outside: this file lands wherever
// the module's own writes are redirected, which is the directory the game can
// also read requests from.
void writeReadyFile()
{
    if (readyWritten.exchange(true, std::memory_order_acq_rel))
        return;
    std::ofstream file(controlPath(L"glass-debug.ready"), std::ios::trunc);
    if (!file)
        return;
    file << "module=" << Util::DllPath().string() << "\n";
    file << "directory=" << controlPath(L"").string() << "\n";
    file << "ok=1\n";
}

void writeStatus(std::ofstream& file)
{
    const auto packed = ReadPackedMotionCaptureStatus();
    const auto identity = ReadGlassMotionIdentityStats();
    const auto controls = ReadControls();
    const auto farCutoff = controls.farCutoffMeters();
    file << "controls enabled=" << controls.enabled << " opacity=" << controls.opacityPercent
         << " edge=" << controls.edgeWidth << " packed_dispatch=" << controls.packedDispatch
         << " packed_rows=" << controls.packedRows << " packed_substitute=" << controls.packedSubstitute
         << " packed_compute=" << controls.packedCompute << " trace=" << controls.trace
         << " autostage=" << controls.autoStage << " writeback=" << controls.packedWriteBack
         << " nrmotion=" << controls.nrMotion
         << " engine_writes=0"
         << " zeromv=" << controls.zeroMotion
         << " readskip=" << controls.packedSkipRead
         << " arraymap=" << controls.arrayMapping << " pipelines=" << controls.compilePipelines
         << " grouped=" << controls.groupedOrder << " arrayprobe=" << controls.arrayProbe
         << " packetlocal=" << controls.packetLocalOrder << " supply=" << controls.packedSupply
         << " layer=" << controls.packedLayer << " jitter=" << controls.jitterMode
         << " jittergain=" << controls.jitterGain << " jitterlog=" << controls.jitterLog
         << " far=" << (farCutoff > 0.f ? std::to_string(unsigned(farCutoff)) + "m" : std::string("off"))
         << " enggate=" << (controls.engineGate ? 1 : 0) << " gatepx=" << controls.engineGatePx()
         << " depthkeep=" << (DepthKeepEnabled() ? 1 : 0)
         << " opaqueprobe=" << (OpaqueProbeEnabled() ? 1 : 0)
         << " stripes=" << (StripeProbeEnabled() ? 1 : 0)
         << " hookstages=" << (HookStageTiming() ? 1 : 0)
         << " gate=" << (GateArmed() ? 1 : 0) << "\n";
    const auto draws = GetCyberpunkDrawStatus();
    file << "array_order grouped=" << draws.arrayProbeGrouped << " compared=" << draws.arrayProbeCompared
         << " permuted=" << draws.arrayProbePermuted << " changed=" << draws.arrayProbeChanged
         << " | local=" << draws.arrayProbeLocal << " local_compared=" << draws.arrayProbeLocalCompared
         << " local_permuted=" << draws.arrayProbeLocalPermuted << " local_changed=" << draws.arrayProbeLocalChanged
         << " same_address=" << draws.arrayProbeSameAddress
         << " distinct_address=" << draws.arrayProbeDistinctAddress
         << " | local_same_address=" << draws.arrayProbeLocalSameAddress
         << " local_distinct_address=" << draws.arrayProbeLocalDistinctAddress
         << " local_unreadable=" << draws.arrayProbeLocalUnreadable
         << " local_base_moved=" << draws.arrayProbeLocalBaseMoved
         << " | local_gate_pass=" << draws.arrayProbeLocalGatePass
         << " local_gate_count1=" << draws.arrayProbeLocalGateCount
         << " local_gate_skin=" << draws.arrayProbeLocalGateSkin << "\n";
    file << "packed initialized=" << packed.initialized << " healthy=" << packed.healthy
         << " admitted=" << packed.admittedDraws << " captured_frames=" << packed.capturedFrames
         << " fg_frames=" << packed.fgFrames << " missing_pipeline=" << packed.missingPipeline
         << " unknown_identity=" << packed.unknownIdentity << " topology_rejected=" << packed.topologyRejected
         << " slot_busy=" << packed.slotBusy << " ordering_rejected=" << packed.orderingRejected
            << " slot_reclaimed=" << packed.slotReclaimed
            << " slot_recovered=" << packed.slotRecovered
         << " no_fg_frame=" << packed.noFgFrame << " no_fg_queue=" << packed.noFgQueue
         << " acquire_no_candidate=" << packed.acquireNoCandidate << " acquire_stale=" << packed.acquireStalePair
         << " coverage_fallback=" << ReadPackedCoverageFallbackCount()
         << " acquire_ambiguous=" << packed.acquireAmbiguous
         << " acquire_consumer_busy=" << packed.acquireConsumerBusy
         << " packed_opaqueprobe_ready=" << ReadOpaqueProbeReadyCount()
         << " packed_opaqueprobe_rejected=" << ReadOpaqueProbeRejectedCount() << "\n";
    file << "identity resolved=" << identity.resolved << " rejected=" << identity.rejected
         << " no_owner=" << identity.noOwner << " no_view=" << identity.noView
         << " no_lifetime=" << identity.noLifetime << " no_element_index=" << identity.noElementIndex
         << " no_element_parent=" << identity.noElementParent << " no_element_order=" << identity.noElementOrder
         << " no_view_state=" << identity.noViewState << " no_view_unknown=" << identity.noViewUnknown
         << " no_view_descriptor=" << identity.noViewDescriptor
         << " no_view_no_record=" << identity.noViewNoRecord
         << " no_view_null_resource=" << identity.noViewNullResource
         << " element_mapped=" << identity.elementMapped
         << " element_unmapped=" << identity.elementUnmapped << "\n";
       file << "history hits=" << packed.historyHits << " inserted=" << packed.historyInserted
            << " reclaimed=" << packed.historyReclaimed << " rejected_topology=" << packed.historyRejectedTopology
            << " set_full=" << packed.historySetFull << " arena_full=" << packed.historyArenaFull
            << " arena_reclaimed=" << packed.historyArenaReclaimed
            << " live=" << packed.historyLive << " arena_pages=" << packed.historyArenaPages
            << " arena_used=" << packed.historyArenaUsedPages
            << " arena_free_max=" << packed.historyArenaLargestFree
            << " frame=" << packed.historyFrame
            << " retired=" << packed.historyRetiredFrame
            << " pinned=" << packed.historyPinnedEntries << "\n";
       // Captured mesh families and the chunks that lost their arena block.
       // Both are needed to tell "never captured" from "captured but dropped".
       for (unsigned i = 0; i < packed.admittedSpanFamilyCount; ++i)
       {
           const auto& family = packed.admittedSpans[i];
           file << "span chunk=" << family.chunk << " mesh=" << family.mesh << " verts=" << family.vertices
                << " spans=" << family.count << "\n";
       }
       file << "span_total spans=" << packed.frameSpanCount << " evicted=" << packed.spanFamilyEvictions << "\n";
       file << "overflow_chunks";
       for (const auto& entry : packed.overflowChunks)
           if (entry.count) file << " " << entry.chunk << ":" << entry.count;
       file << "\n";
    file << "gpu_ms=" << ReadGpuMilliseconds() << "\n";
    const auto host = ReadNativeHostStatus();
    file << "host evaluations=" << host.evaluations << " substitutions=" << host.substitutions
         << " captures=" << host.captures << " active=" << host.active << " retiring=" << host.retiring
         << " stopped=" << host.stopped << " unavailable=" << host.unavailable << "\n";
    // Per-generated-frame index split: an index that is evaluated but never
    // substituted keeps the engine's original motion vectors for that frame.
    file << "host_by_index";
    for (unsigned i = 0; i < 8; ++i)
        if (host.evaluationsByIndex[i] != 0)
            file << " " << i << ":" << host.substitutionsByIndex[i] << "/" << host.evaluationsByIndex[i];
    file << " (substituted/evaluated) skipped_reused=" << host.unsubstitutedReusedMotion
         << " skipped_uncorrected=" << host.unsubstitutedFreshMotion << "\n";
    file << "host_by_path dlssg=" << host.substitutionsByPath[0] << "/" << host.evaluationsByPath[0]
         << " alias=" << host.substitutionsByPath[1] << "/" << host.evaluationsByPath[1]
         << " prepared=" << host.preparedByPath[0] << "/" << host.preparedByPath[1] << "\n";
    // DLSS FG only: evaluations that are provably not the frame generator and
    // were passed through, plus the read-back of the parameter names the
    // substitution touched. Both have to show that nothing else was changed.
    file << "fg_gate nonfg_passed=" << host.nonFrameGenerationEvaluations
         << " provider_confirmed=" << host.providerConfirmedEvaluations
         << " provider_unconfirmed=" << host.providerUnconfirmedEvaluations
         << " caller_confirmed=" << host.callerConfirmedEvaluations
         << " caller_mode=" << FrameGenerationCallerMode()
         << " restore_checked=" << host.restoreChecks << " restore_failed=" << host.restoreFailures
         << " alias_substituted=" << host.substitutionsByPath[1] << "\n";
    file << "plugin loaded=" << (GlassPluginLoaded() ? 1 : 0) << "\n";
    const auto mapping = ReadArrayMappingStats();
    file << "arraymap entries=" << mapping.entries << " published=" << mapping.published
         << " replaced=" << mapping.replaced << " lookups=" << mapping.lookups << " hits=" << mapping.hits
         << " range_hits=" << mapping.rangeHits << " range_ambiguous=" << mapping.rangeAmbiguous
         << " misses=" << mapping.misses << " out_of_range=" << mapping.outOfRange
         << " evictions=" << mapping.evictions << "\n";
    // Engine grouped-update hooks: the grouped call publishes one array mapping
    // per update, the append hook recovers each element's original source index.
    // A live session needs all five numbers to tell "hook never ran" from
    // "capture aborted" from "published but never queried".
    const auto groups = ReadGroupedArrayStats();
    file << "groups calls=" << groups.groupedCalls << " staged=" << groups.staged
         << " published=" << groups.published << " aborted=" << groups.aborted
         << " append_calls=" << groups.appendCalls << " in_range=" << groups.appendInRange
         << " outside=" << groups.appendOutside << " dropped=" << groups.appendDropped
         << " sample_calls=" << groups.sampleCalls << " sample_cycles=" << groups.sampleCycles
         << " sample_max_cycles=" << groups.sampleMaxCycles << "\n";
    // Bounded first-N keys. The plugin publishes outputStart-based ranges while
    // the identity provider queries the packet transform index, so a live miss
    // has to be attributable to either the proxy or the ordinal domain.
    file << "mapprobe_publish";
    for (unsigned i = 0; i < mapping.publishProbeCount; ++i)
        file << " " << std::hex << mapping.publishProbe[i].proxy << "@" << std::dec << mapping.publishProbe[i].value
             << "+" << mapping.publishProbe[i].ordinal;
    file << "\n";
    file << "mapprobe_lookup";
    for (unsigned i = 0; i < mapping.lookupProbeCount; ++i)
        file << " " << std::hex << mapping.lookupProbe[i].proxy << ":" << std::dec << mapping.lookupProbe[i].ordinal
             << "=" << mapping.lookupProbe[i].result;
    file << "\n";
    file << "ok=1\n";
}
} // namespace

bool TakeShaderReloadRequest() noexcept
{
    return shaderReloadRequested.exchange(false, std::memory_order_acq_rel);
}

void PollGlassDebugControl() noexcept
{
    try
    {
        writeReadyFile();
        const auto request = controlPath(L"glass-debug.request");
        std::error_code error;
        if (!std::filesystem::exists(request, error) || error)
            return;
        // Consume the request by renaming it, so a command runs exactly once.
        const auto consumed = controlPath(L"glass-debug.consumed");
        std::filesystem::rename(request, consumed, error);
        if (error)
        {
            // Windows refuses a rename onto an existing file. Clear the stale
            // target and retry once instead of stalling the channel forever.
            std::error_code ignored;
            std::filesystem::remove(consumed, ignored);
            error.clear();
            std::filesystem::rename(request, consumed, error);
            if (error)
                return;
        }
        std::ifstream input(consumed);
        std::ofstream output(controlPath(L"glass-debug.response"), std::ios::trunc);
        if (!input || !output)
            return;
        std::string line;
        while (std::getline(input, line))
        {
            while (!line.empty() && (line.back() == '\r' || line.back() == ' '))
                line.pop_back();
            if (line.empty())
                continue;
            if (line == "status")
                continue;
            if (line == "reload-shader")
            {
                shaderReloadRequested.store(true, std::memory_order_release);
                output << "reload-shader=queued\n";
                continue;
            }
            if (line == "soft-reload")
            {
                RequestNativeSoftReload();
                output << "soft-reload=queued\n";
                continue;
            }
            // Staged protocol: probe runs the new GPU work without swapping the
            // FG inputs; apply then enables the input replacement.
            if (line == "probe")
            {
                auto value = ReadControls();
                value.packedDispatch = true;
                value.packedSubstitute = false;
                value.packedRows = 240;
                WriteControls(value);
                output << "probe=dispatch_on_substitute_off_rows_240\n";
                continue;
            }
            if (line == "apply")
            {
                auto value = ReadControls();
                value.packedDispatch = true;
                value.packedSubstitute = true;
                WriteControls(value);
                output << "apply=substitute_on\n";
                continue;
            }
            if (line == "dump")
            {
                RequestPackedDump();
                output << "dump=queued\n";
                continue;
            }
            if (line == "gate=on" || line == "gate=off" || line == "gate=reset")
            {
                auto& gate = Gate();
                if (line == "gate=reset")
                {
                    for (auto& stage : gate.stage)
                        stage.store(0, std::memory_order_relaxed);
                    gate.unseenPipelineProbes.store(0, std::memory_order_relaxed);
                    gate.unseenPipelineDistinct.store(0, std::memory_order_relaxed);
                    output << "gate=reset\n";
                    continue;
                }
                const bool armed = line == "gate=on";
                for (auto& stage : gate.stage)
                    stage.store(0, std::memory_order_relaxed);
                gate.unseenPipelineProbes.store(0, std::memory_order_relaxed);
                gate.unseenPipelineDistinct.store(0, std::memory_order_relaxed);
                GateArm(armed);
                output << "gate=" << (armed ? "on" : "off") << "\n";
                continue;
            }
            if (line == "plugin=load" || line == "plugin=reload" || line == "plugin=unload")
            {
                char message[160] {};
                bool okay = false;
                if (line != "plugin=load")
                    UnloadGlassPlugin(message, sizeof(message));
                if (line != "plugin=unload")
                    okay = LoadGlassPlugin(message, sizeof(message));
                else
                    okay = true;
                output << line << " ok=" << (okay ? 1 : 0) << " " << message << " loaded="
                       << (GlassPluginLoaded() ? 1 : 0) << "\n";
                continue;
            }
            if (line.rfind("nrmotion=", 0) == 0)
            {
                // Second consumer: hand the composed motion and depth to the
                // DLSS-NR evaluate as its own inputs. The engine's own motion
                // texture is never written, so nothing else reads the value.
                auto value = ReadControls();
                value.nrMotion = line.substr(9) == "on";
                value.packedWriteBack = false;
                WriteControls(value);
                output << "nrmotion=" << (value.nrMotion ? 1 : 0) << " engine_writes=0\n";
                continue;
            }
            if (line.rfind("fgcaller=", 0) == 0)
            {
                // Diagnostic only and never persisted: the default identity for
                // the driver-level DLSS-G evaluation is the calling module
                // (strict). "any" exists to prove, in one session, whether the
                // evaluation that reaches this hook is the generator at all,
                // and "off" restores the pre-existing gate behaviour.
                const auto mode = line.substr(9);
                const unsigned value = mode == "off" ? 0u : mode == "any" ? 2u : 1u;
                SetFrameGenerationCallerMode(value);
                output << "fgcaller=" << (value == 0u ? "off" : value == 2u ? "any" : "strict")
                       << " mode=" << FrameGenerationCallerMode() << "\n";
                continue;
            }
            if (line.rfind("depthkeep=", 0) == 0)
            {
                // Diagnostic depth route, live channel only like the hook-skip
                // stage: the value is not persisted and the settings panel does
                // not carry it. On keeps the engine's depth under a substituted
                // motion, so one session can separate a depth-driven occlusion
                // change from a motion-driven one.
                const bool enabled = line.substr(10) == "on" || line.substr(10) == "1";
                SetDepthKeep(enabled);
                output << "depthkeep=" << (enabled ? 1 : 0) << "\n";
                continue;
            }
            if (line.rfind("opaqueprobe=", 0) == 0)
            {
                // Diagnostic opaque-pipeline probe. The INI key GlassFG/
                // OpaqueProbe seeds the same flag at load; this channel also
                // changes it in a live session. On admits the depth-writing and
                // unblended pipelines the product gate refuses so their packed
                // captures can be compared against the engine's own motion.
                const bool enabled = line.substr(12) == "on" || line.substr(12) == "1";
                SetOpaqueProbe(enabled);
                output << "opaqueprobe=" << (enabled ? 1 : 0) << "\n";
                continue;
            }
            if (line.rfind("stripes=", 0) == 0)
            {
                // Simultaneous stripe A/B. On, the compose keeps the object's
                // motion in even 64-pixel column bands and leaves the engine's
                // value in odd bands, so one delivered frame carries both arms.
                // Live channel only; the delivered default stays off.
                const bool enabled = line.substr(8) == "on" || line.substr(8) == "1";
                SetStripeProbe(enabled);
                output << "stripes=" << (enabled ? 1 : 0) << "\n";
                continue;
            }
            auto value = ReadControls();
            if (line.rfind("packed=", 0) == 0)
                value.packedDispatch = line.substr(7) == "on";
            else if (line.rfind("rows=", 0) == 0)
                value.packedRows = static_cast<unsigned>(std::clamp(std::strtoul(line.c_str() + 5, nullptr, 10), 1ul, 32768ul));
            else if (line.rfind("edge=", 0) == 0)
                value.edgeWidth = static_cast<unsigned>(std::clamp(std::strtoul(line.c_str() + 5, nullptr, 10), 1ul, 4ul));
            else if (line.rfind("opacity=", 0) == 0)
                value.opacityPercent =
                    static_cast<unsigned>(std::clamp(std::strtoul(line.c_str() + 8, nullptr, 10), 0ul, 100ul));
            // Legacy spelling from the blend-strength era; kept so an old control
            // file still sets the threshold instead of failing the request.
            else if (line.rfind("strength=", 0) == 0)
                value.opacityPercent =
                    static_cast<unsigned>(std::clamp(std::strtoul(line.c_str() + 9, nullptr, 10), 0ul, 100ul));
            else if (line.rfind("substitute=", 0) == 0)
                value.packedSubstitute = line.substr(11) == "on";
            else if (line.rfind("readskip=", 0) == 0)
                value.packedSkipRead = line.substr(9) == "on";
            else if (line.rfind("zeromv=", 0) == 0)
                value.zeroMotion = line.substr(7) == "on";
            else if (line.rfind("trace=", 0) == 0)
                value.trace = line.substr(6) == "on";
            else if (line.rfind("supply=", 0) == 0)
                value.packedSupply = line.substr(7) == "on";
            else if (line.rfind("layer=", 0) == 0)
                value.packedLayer = line.substr(6) == "on";
            else if (line.rfind("autostage=", 0) == 0)
                value.autoStage = line.substr(10) == "on";
            else if (line.rfind("compute=", 0) == 0)
                value.packedCompute = line.substr(8) == "on";
            else if (line.rfind("arraymap=", 0) == 0)
                value.arrayMapping = line.substr(9) == "on";
            else if (line.rfind("pipelines=", 0) == 0)
                value.compilePipelines = line.substr(10) == "on";
            else if (line.rfind("enabled=", 0) == 0)
                value.enabled = line.substr(8) == "on";
            else if (line == "hookstages=on" || line == "hookstages=off")
            {
                SetHookStageTiming(line == "hookstages=on");
                output << "hookstages=" << (HookStageTiming() ? 1 : 0) << "\n";
                continue;
            }
            else if (line.rfind("hookskip=", 0) == 0)
            {
                // Diagnostic only: cuts the command hooks back to a stage so
                // the per-stage cost can be measured without a restart. Not
                // stored in the INI and not part of the saved controls.
                const auto stage = line.substr(9);
                SetHookSkipMode(stage == "find"      ? 2u
                                : stage == "bindings" ? 1u
                                : stage == "draws"    ? 3u
                                : stage == "all"      ? 4u
                                : stage == "indexed"   ? 5u
                                                      : 0u);
                output << "hookskip=" << stage << "\n";
                continue;
            }
            else if (line.rfind("grouped=", 0) == 0)
                value.groupedOrder = line.substr(8) == "on";
            else if (line.rfind("arrayprobe=", 0) == 0)
                value.arrayProbe = line.substr(11) == "on";
            else if (line.rfind("packetlocal=", 0) == 0)
                value.packetLocalOrder = line.substr(12) == "on";
            else if (line.rfind("jitter=", 0) == 0)
                value.jitterMode = static_cast<unsigned>(std::clamp(std::strtoul(line.c_str() + 7, nullptr, 10), 0ul, 7ul));
            else if (line.rfind("jittergain=", 0) == 0)
                value.jitterGain = static_cast<unsigned>(std::clamp(std::strtoul(line.c_str() + 11, nullptr, 10), 0ul, 255ul));
            else if (line.rfind("jitterlog=", 0) == 0)
                value.jitterLog = line.substr(10) == "on" || line.substr(10) == "1";
            else if (line.rfind("enggate=", 0) == 0)
                value.engineGate = line.substr(8) == "on" || line.substr(8) == "1";
            else if (line.rfind("gatepx=", 0) == 0)
            {
                // Radius in pixels, rounded to the 1/16 px steps the control
                // word carries (0..7 steps -> 0..0.4375 px).
                const double pixels = std::strtod(line.c_str() + 7, nullptr);
                const double steps = pixels * 16.0;
                value.engineGateSteps = static_cast<unsigned>(
                    std::clamp(steps + (steps >= 0.0 ? 0.5 : -0.5), 0.0, 7.0));
            }
            else if (line.rfind("far=", 0) == 0)
            {
                // Distance in metres; 0 or "off" keeps every surface. Rounded to
                // the 25 m steps the settings panel offers.
                const auto text = line.substr(4);
                const long meters = text == "off" ? 0 : std::strtol(text.c_str(), nullptr, 10);
                value.farSkipStep = static_cast<unsigned>(std::clamp((meters + 12) / 25, 0L, 15L));
            }
            else
            {
                output << "unknown=" << line << "\n";
                continue;
            }
            WriteControls(value);
            output << "applied=" << line << "\n";
        }
        writeStatus(output);
        std::filesystem::remove(consumed, error);
    }
    catch (...)
    {
    }
}
} // namespace GlassFg
