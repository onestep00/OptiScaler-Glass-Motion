#include "pch.h"
#include "GlassDebugControl.h"
#include "GlassControls.h"
#include "GlassMotionIdentity.h"
#include "NativeHost.h"
#include "PackedMotionCapture.h"
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
         << " autostage=" << controls.autoStage << " writeback=" << controls.packedWriteBack << "\n";
    file << "packed initialized=" << packed.initialized << " healthy=" << packed.healthy
         << " admitted=" << packed.admittedDraws << " captured_frames=" << packed.capturedFrames
         << " fg_frames=" << packed.fgFrames << " missing_pipeline=" << packed.missingPipeline
         << " unknown_identity=" << packed.unknownIdentity << " topology_rejected=" << packed.topologyRejected
         << " slot_busy=" << packed.slotBusy << " ordering_rejected=" << packed.orderingRejected
         << " no_fg_frame=" << packed.noFgFrame << " no_fg_queue=" << packed.noFgQueue
         << " acquire_no_candidate=" << packed.acquireNoCandidate << " acquire_stale=" << packed.acquireStalePair
         << " acquire_ambiguous=" << packed.acquireAmbiguous
         << " acquire_consumer_busy=" << packed.acquireConsumerBusy << "\n";
    file << "identity resolved=" << identity.resolved << " rejected=" << identity.rejected
         << " no_owner=" << identity.noOwner << " no_view=" << identity.noView
         << " no_lifetime=" << identity.noLifetime << " no_element_index=" << identity.noElementIndex << "\n";
    file << "history hits=" << packed.historyHits << " inserted=" << packed.historyInserted
         << " reclaimed=" << packed.historyReclaimed << " rejected_topology=" << packed.historyRejectedTopology
         << " set_full=" << packed.historySetFull << " arena_full=" << packed.historyArenaFull
         << " live=" << packed.historyLive << "\n";
    const auto host = ReadNativeHostStatus();
    file << "host evaluations=" << host.evaluations << " substitutions=" << host.substitutions
         << " captures=" << host.captures << " active=" << host.active << " retiring=" << host.retiring
         << " stopped=" << host.stopped << " unavailable=" << host.unavailable << "\n";
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
            else if (line.rfind("trace=", 0) == 0)
                value.trace = line.substr(6) == "on";
            else if (line.rfind("autostage=", 0) == 0)
                value.autoStage = line.substr(10) == "on";
            else if (line.rfind("compute=", 0) == 0)
                value.packedCompute = line.substr(8) == "on";
            else if (line.rfind("writeback=", 0) == 0)
                value.packedWriteBack = line.substr(10) == "on";
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
