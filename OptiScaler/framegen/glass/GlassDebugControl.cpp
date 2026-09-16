#include "pch.h"
#include "GlassDebugControl.h"
#include "GlassControls.h"
#include "GlassMotionIdentity.h"
#include "NativeHost.h"
#include "PackedMotionCapture.h"
#include "GlassPluginHost.h"
#include "GlassArrayMapping.h"
#include "GeometryPipeline.h"
#include "CyberpunkDraws.h"
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
    file << "controls enabled=" << controls.enabled << " strength=" << controls.strength
         << " edge=" << controls.edgeWidth << " packed_dispatch=" << controls.packedDispatch
         << " packed_rows=" << controls.packedRows << " packed_substitute=" << controls.packedSubstitute
         << " packed_compute=" << controls.packedCompute << " trace=" << controls.trace
         << " autostage=" << controls.autoStage << " writeback=" << controls.packedWriteBack
         << " readskip=" << controls.packedSkipRead
         << " arraymap=" << controls.arrayMapping << " pipelines=" << controls.compilePipelines
         << " grouped=" << controls.groupedOrder << " arrayprobe=" << controls.arrayProbe
         << " packetlocal=" << controls.packetLocalOrder << " supply=" << controls.packedSupply << "\n";
    const auto draws = GetCyberpunkDrawStatus();
    file << "array_order grouped=" << draws.arrayProbeGrouped << " compared=" << draws.arrayProbeCompared
         << " permuted=" << draws.arrayProbePermuted << " changed=" << draws.arrayProbeChanged
         << " | local=" << draws.arrayProbeLocal << " local_compared=" << draws.arrayProbeLocalCompared
         << " local_permuted=" << draws.arrayProbeLocalPermuted << " local_changed=" << draws.arrayProbeLocalChanged
         << " same_address=" << draws.arrayProbeSameAddress
         << " distinct_address=" << draws.arrayProbeDistinctAddress << "\n";
    file << "packed initialized=" << packed.initialized << " healthy=" << packed.healthy
         << " admitted=" << packed.admittedDraws << " captured_frames=" << packed.capturedFrames
         << " fg_frames=" << packed.fgFrames << " missing_pipeline=" << packed.missingPipeline
         << " unknown_identity=" << packed.unknownIdentity << " topology_rejected=" << packed.topologyRejected
         << " slot_busy=" << packed.slotBusy << " ordering_rejected=" << packed.orderingRejected
         << " slot_reclaimed=" << packed.slotReclaimed
         << " no_fg_frame=" << packed.noFgFrame << " no_fg_queue=" << packed.noFgQueue
         << " acquire_no_candidate=" << packed.acquireNoCandidate << " acquire_stale=" << packed.acquireStalePair
         << " coverage_fallback=" << ReadPackedCoverageFallbackCount()
         << " acquire_ambiguous=" << packed.acquireAmbiguous
         << " acquire_consumer_busy=" << packed.acquireConsumerBusy << "\n";
    file << "identity resolved=" << identity.resolved << " rejected=" << identity.rejected
         << " no_owner=" << identity.noOwner << " no_view=" << identity.noView
         << " no_lifetime=" << identity.noLifetime << " no_element_index=" << identity.noElementIndex
         << " no_element_parent=" << identity.noElementParent << " no_element_order=" << identity.noElementOrder
         << " no_view_state=" << identity.noViewState << " no_view_unknown=" << identity.noViewUnknown
         << " no_view_descriptor=" << identity.noViewDescriptor << "\n";
       file << "history hits=" << packed.historyHits << " inserted=" << packed.historyInserted
            << " reclaimed=" << packed.historyReclaimed << " rejected_topology=" << packed.historyRejectedTopology
            << " set_full=" << packed.historySetFull << " arena_full=" << packed.historyArenaFull
            << " arena_reclaimed=" << packed.historyArenaReclaimed
            << " live=" << packed.historyLive << " arena_pages=" << packed.historyArenaPages
            << " arena_used=" << packed.historyArenaUsedPages
            << " arena_free_max=" << packed.historyArenaLargestFree << "\n";
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
    file << "plugin loaded=" << (GlassPluginLoaded() ? 1 : 0) << "\n";
    const auto mapping = ReadArrayMappingStats();
    file << "arraymap entries=" << mapping.entries << " published=" << mapping.published
         << " replaced=" << mapping.replaced << " lookups=" << mapping.lookups << " hits=" << mapping.hits
         << " misses=" << mapping.misses << " out_of_range=" << mapping.outOfRange
         << " evictions=" << mapping.evictions << "\n";
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
            auto value = ReadControls();
            if (line.rfind("packed=", 0) == 0)
                value.packedDispatch = line.substr(7) == "on";
            else if (line.rfind("rows=", 0) == 0)
                value.packedRows = static_cast<unsigned>(std::clamp(std::strtoul(line.c_str() + 5, nullptr, 10), 1ul, 32768ul));
            else if (line.rfind("edge=", 0) == 0)
                value.edgeWidth = static_cast<unsigned>(std::clamp(std::strtoul(line.c_str() + 5, nullptr, 10), 1ul, 4ul));
            else if (line.rfind("strength=", 0) == 0)
                value.strength = static_cast<unsigned>(std::clamp(std::strtoul(line.c_str() + 9, nullptr, 10), 0ul, 100ul));
            else if (line.rfind("substitute=", 0) == 0)
                value.packedSubstitute = line.substr(11) == "on";
            else if (line.rfind("readskip=", 0) == 0)
                value.packedSkipRead = line.substr(9) == "on";
            else if (line.rfind("trace=", 0) == 0)
                value.trace = line.substr(6) == "on";
            else if (line.rfind("supply=", 0) == 0)
                value.packedSupply = line.substr(7) == "on";
            else if (line.rfind("autostage=", 0) == 0)
                value.autoStage = line.substr(10) == "on";
            else if (line.rfind("compute=", 0) == 0)
                value.packedCompute = line.substr(8) == "on";
            else if (line.rfind("writeback=", 0) == 0)
                value.packedWriteBack = line.substr(10) == "on";
            else if (line.rfind("arraymap=", 0) == 0)
                value.arrayMapping = line.substr(9) == "on";
            else if (line.rfind("pipelines=", 0) == 0)
                value.compilePipelines = line.substr(10) == "on";
            else if (line.rfind("enabled=", 0) == 0)
                value.enabled = line.substr(8) == "on";
            else if (line.rfind("grouped=", 0) == 0)
                value.groupedOrder = line.substr(8) == "on";
            else if (line.rfind("arrayprobe=", 0) == 0)
                value.arrayProbe = line.substr(11) == "on";
            else if (line.rfind("packetlocal=", 0) == 0)
                value.packetLocalOrder = line.substr(12) == "on";
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
