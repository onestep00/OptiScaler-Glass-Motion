#include "pch.h"
#include "GlassControls.h"
#include "GlassArrayMapping.h"
#include "GeometryHealth.h"
#include "PackedMotionCapture.h"
#include "NvngxDlssgBridge.h"
#include <Util.h>
#include <SimpleIni.h>
#include <imgui/imgui.h>
#include <atomic>
#include <cmath>
#include <mutex>

namespace GlassFg
{
namespace
{
std::atomic<uint64_t> controls { Controls {}.packed() };
std::once_flag loaded;
std::atomic<double> latestMilliseconds { -1.0 };
std::atomic<RuntimeStatus> runtimeStatus { RuntimeStatus::Waiting };

std::filesystem::path settingsPath() { return Util::DllPath().parent_path() / L"OptiScaler.Glass.ini"; }

bool load()
{
    CSimpleIniA ini;
    const auto path = settingsPath();
    std::error_code error;
    if (!std::filesystem::exists(path, error) && !error)
    {
        controls.store(Controls {}.packed(), std::memory_order_relaxed);
        return true;
    }
    if (error || ini.LoadFile(path.c_str()) < 0)
        return false;
    latestMilliseconds.store(-1.0, std::memory_order_relaxed);
    const auto strength = ini.GetLongValue("GlassFG", "Strength", 100);
    controls.store(Controls { ini.GetBoolValue("GlassFG", "Enabled", false),
                              static_cast<unsigned>(std::clamp(strength, 0L, 100L)),
                              ini.GetBoolValue("GlassFG", "MeasureGpuTime", true),
                              static_cast<unsigned>(std::clamp(ini.GetLongValue("GlassFG", "EdgeWidth", 2), 1L, 4L)),
                              ini.GetBoolValue("GlassFG", "PackedDispatch", true),
                              static_cast<unsigned>(std::clamp(ini.GetLongValue("GlassFG", "PackedRows", 240), 1L, 32768L)),
                              ini.GetBoolValue("GlassFG", "PackedSubstitute", false),
                              ini.GetBoolValue("GlassFG", "Trace", false),
                              ini.GetBoolValue("GlassFG", "AutoStage", false),
                              ini.GetBoolValue("GlassFG", "PackedCompute", true),
                              ini.GetBoolValue("GlassFG", "PackedWriteBack", false),
                              ini.GetBoolValue("GlassFG", "PackedSkipRead", false),
                              ini.GetBoolValue("GlassFG", "ArrayMapping", false),
                              ini.GetBoolValue("GlassFG", "CompilePipelines", true),
                              ini.GetBoolValue("GlassFG", "GroupedOrder", true),
                              ini.GetBoolValue("GlassFG", "ArrayProbe", false) }
                       .packed(),
                   std::memory_order_relaxed);
    SetGeometryPipelineCompilation(Controls::unpack(controls.load(std::memory_order_relaxed)).compilePipelines);
    return true;
}

bool save(Controls value)
{
    CSimpleIniA ini;
    const auto path = settingsPath();
    std::error_code error;
    const bool exists = std::filesystem::exists(path, error);
    if (error || (exists && ini.LoadFile(path.c_str()) < 0))
        return false;
    ini.SetBoolValue("GlassFG", "Enabled", value.enabled);
    ini.SetLongValue("GlassFG", "Strength", std::min(value.strength, 100u));
    ini.SetBoolValue("GlassFG", "MeasureGpuTime", value.measureGpuTime);
    ini.SetLongValue("GlassFG", "EdgeWidth", std::clamp(value.edgeWidth, 1u, 4u));
    ini.SetBoolValue("GlassFG", "PackedDispatch", value.packedDispatch);
    ini.SetLongValue("GlassFG", "PackedRows", std::clamp(value.packedRows, 1u, 32768u));
    ini.SetBoolValue("GlassFG", "PackedSubstitute", value.packedSubstitute);
    ini.SetBoolValue("GlassFG", "Trace", value.trace);
    ini.SetBoolValue("GlassFG", "AutoStage", value.autoStage);
    ini.SetBoolValue("GlassFG", "PackedCompute", value.packedCompute);
    ini.SetBoolValue("GlassFG", "PackedWriteBack", value.packedWriteBack);
    ini.SetBoolValue("GlassFG", "PackedSkipRead", value.packedSkipRead);
    ini.SetBoolValue("GlassFG", "ArrayMapping", value.arrayMapping);
    ini.SetBoolValue("GlassFG", "CompilePipelines", value.compilePipelines);
    ini.SetBoolValue("GlassFG", "GroupedOrder", value.groupedOrder);
    ini.SetBoolValue("GlassFG", "ArrayProbe", value.arrayProbe);
    auto temporary = path;
    temporary += L".tmp";
    if (ini.SaveFile(temporary.c_str()) < 0)
        return false;
    // Preserve the old file if the replacement fails. The UI reports failure.
    return MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != FALSE;
}
} // namespace

Controls ReadControls()
{
    std::call_once(loaded, [] { load(); });
    return Controls::unpack(controls.load(std::memory_order_relaxed));
}

void WriteControls(Controls value)
{
    auto previous = ReadControls();
    if (previous.measureGpuTime != value.measureGpuTime || previous.enabled != value.enabled ||
        previous.strength != value.strength || previous.edgeWidth != value.edgeWidth)
        latestMilliseconds.store(-1.0, std::memory_order_relaxed);
    controls.store(value.packed(), std::memory_order_relaxed);
    SetGeometryPipelineCompilation(value.compilePipelines);
}

void PublishGpuMilliseconds(double milliseconds)
{
    if (std::isfinite(milliseconds) && milliseconds >= 0.0)
        latestMilliseconds.store(milliseconds, std::memory_order_relaxed);
}

double ReadGpuMilliseconds() noexcept
{
    return latestMilliseconds.load(std::memory_order_relaxed);
}

void PublishRuntimeStatus(RuntimeStatus status) { runtimeStatus.store(status, std::memory_order_relaxed); }

void RenderSettings()
{
    const bool open = ImGui::CollapsingHeader("Transparent surface correction (experimental)##GlassFG");
    const auto now = GetTickCount64();
    RefreshGeometryHealthIfNeeded(now);
    const auto health = ReadGeometryHealth();
    const bool applied = health.recentlyApplied(now);
    ImGui::TextColored(applied ? ImVec4(.4f, .85f, .4f, 1) : ImVec4(1, .7f, .3f, 1), "%s",
                       applied ? "Object MV -> FG: recently applied" : "Object MV -> FG: NOT applied");
    if (!open)
        return;
    ImGui::PushID("GlassFG");
    auto value = ReadControls();
    bool changed = ImGui::Checkbox("Enable glass motion correction", &value.enabled);
    int strength = static_cast<int>(value.strength);
    changed |= ImGui::SliderInt("Correction strength", &strength, 0, 100, "%d%%");
    value.strength = static_cast<unsigned>(strength);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Scales the exact material opacity used for object motion inside the edge.\n"
                          "The selected edge always uses exact object motion while correction is enabled.");
    int edgeWidth = static_cast<int>(value.edgeWidth);
    changed |= ImGui::SliderInt("Object edge width", &edgeWidth, 1, 4, "%d px");
    value.edgeWidth = static_cast<unsigned>(edgeWidth);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Applies exact object motion and surface depth to this many pixels inside each visible object edge.");
    bool packedDispatch = value.packedDispatch;
    changed |= ImGui::Checkbox("Packed object-motion dispatch", &packedDispatch);
    value.packedDispatch = packedDispatch;
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Runs the packed object-motion compose pass that feeds object motion into the FG inputs.\n"
                          "Disable it to isolate driver or stability issues.");
    int packedRows = static_cast<int>(value.packedRows);
    changed |= ImGui::SliderInt("Packed dispatch rows", &packedRows, 1, 1440, "%d rows");
    value.packedRows = static_cast<unsigned>(packedRows);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Safety limit: only the top rows receive the packed correction.\n"
                          "Raise it after a clean run; the rest keeps the original motion.");
    bool packedSubstitute = value.packedSubstitute;
    changed |= ImGui::Checkbox("Replace FG motion/depth inputs", &packedSubstitute);
    value.packedSubstitute = packedSubstitute;
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Swaps the FG motion/depth inputs for the composed object-motion outputs.\n"
                          "Off runs the dispatch without touching the FG inputs to isolate driver resets.");
    bool trace = value.trace;
    changed |= ImGui::Checkbox("Step trace log", &trace);
    value.trace = trace;
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Writes one flushed line per dispatch, substitution and submit step.\n"
                          "A driver reset then leaves the last executed step in the log.");
    bool autoStage = value.autoStage;
    changed |= ImGui::Checkbox("Automatic staged ramp", &autoStage);
    value.autoStage = autoStage;
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Unattended order: 240 rows, then full rows, then the FG input swap.\n"
                          "Each step is logged as AUTO_STAGE so a reset is attributable.");
    bool packedCompute = value.packedCompute;
    changed |= ImGui::Checkbox("Packed compose compute", &packedCompute);
    value.packedCompute = packedCompute;
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Off keeps the input copies and the FG swap but skips the compose\n"
                          "dispatch, so the swap can be tested with no new GPU work.");
    bool packedWriteBack = value.packedWriteBack;
    changed |= ImGui::Checkbox("Write correction into game inputs", &packedWriteBack);
    value.packedWriteBack = packedWriteBack;
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Copies the composed motion and depth back into the engine's own\n"
                          "FG inputs instead of substituting foreign resources.");
    bool packedSkipRead = value.packedSkipRead;
    changed |= ImGui::Checkbox("Diagnostic: skip packed record read", &packedSkipRead);
    value.packedSkipRead = packedSkipRead;
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Runs the compose dispatch without reading the object records the capture\n"
                          "raster wrote, so a GPU stall separates the dispatch from the read.");
    bool arrayMapping = value.arrayMapping;
    changed |= ImGui::Checkbox("Use engine array element mapping", &arrayMapping);
    value.arrayMapping = arrayMapping;
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Consumes the grouped-array element mapping published by the live\n"
                          "plugin so repeated objects keep per-element motion history.");
    bool groupedOrder = value.groupedOrder;
    changed |= ImGui::Checkbox("Grouped arrays: packet order element identity", &groupedOrder);
    value.groupedOrder = groupedOrder;
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Grouped update arrays (flag 0x2000) expose no source index at the draw\n"
                          "site. The packet ordinal is used as the element position and the array's\n"
                          "observed lifetime generation invalidates it whenever the array mutates.\n"
                          "Turn off if a repacked group shows a one-frame ghost.");
    bool arrayProbe = value.arrayProbe;
    changed |= ImGui::Checkbox("Diagnostic: measure grouped array reordering", &arrayProbe);
    value.arrayProbe = arrayProbe;
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Hashes the element bytes each grouped array packet receives and compares\n"
                          "them with the previous frame for the same array. If the element set is\n"
                          "unchanged while the positions move, the packet ordinal is not a stable\n"
                          "element identity and the counter reports it.");
    if (changed)
        WriteControls(value);
    const auto milliseconds = latestMilliseconds.load(std::memory_order_relaxed);
    if (!value.active())
        ImGui::TextDisabled("GPU correction: inactive");
    else if (!value.measureGpuTime)
        ImGui::TextDisabled("GPU correction: timing disabled in INI");
    else if (milliseconds >= 0.0)
        ImGui::Text("GPU correction: %.3f ms (last sample)", milliseconds);
    else
        ImGui::TextDisabled("GPU correction: waiting for a completed sample");
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Reads completed results without waiting. With the packed object path active\n"
                          "this is the compose GPU time: engine input copies plus the correction dispatch.\n"
                          "DLSS-G evaluation and surface capture are excluded.");

    static const char* result = nullptr;
    if (ImGui::Button("Save glass settings"))
        result = save(value) ? "Glass settings saved." : "Could not save glass settings.";
    ImGui::SameLine();
    if (ImGui::Button("Reload glass settings"))
        result = load() ? "Glass settings reloaded." : "Could not load glass settings.";
    ImGui::SameLine();
    if (ImGui::Button("Reset glass defaults"))
    {
        WriteControls({});
        result = "Defaults restored. Save to keep them.";
    }
    if (result)
        ImGui::TextWrapped("%s", result);
    ImGui::TextDisabled("Saved separately in OptiScaler.Glass.ini");
    if (value.active())
    {
        switch (runtimeStatus.load(std::memory_order_relaxed))
        {
        case RuntimeStatus::Correcting:
            ImGui::TextWrapped("Engine-object motion correction active (experimental).");
            break;
        case RuntimeStatus::Unavailable:
            ImGui::TextWrapped("Correction unavailable for this session. See OptiScaler.Glass.log.");
            break;
        case RuntimeStatus::Retiring:
            ImGui::TextWrapped("Waiting for previous correction work to finish.");
            break;
        case RuntimeStatus::Stopped:
            ImGui::TextWrapped("Correction stopped after graphics shutdown. Waiting for native DLSS-G recreation.");
            break;
        default:
            ImGui::TextWrapped("Waiting for compatible native DLSS-G inputs.");
            break;
        }
    }
    ImGui::Separator();
    ImGui::TextUnformatted("Object motion / compatibility diagnostics");
    ImGui::TextWrapped("%s", health.reason(now));
    const auto hook = [&](const char* label, GeometryCapability capability, GeometryEvidence evidence)
    {
        const auto count = static_cast<unsigned long long>(health.counts[evidence]);
        ImGui::Text("%s: %s; calls %llu", label,
                    !health.has(capability) ? "unavailable"
                    : count                 ? "observed"
                                            : "installed, waiting",
                    count);
    };
    ImGui::Text("Engine layout: %s", health.has(GeometryEngineLayout) ? "structure validated" : "not validated");
    hook("D3D12 creation hooks", GeometryCreationHooks, GeometryPipelines);
    hook("Object identity hooks", GeometryObjectHooks, GeometryIdentities);
    hook("Engine draw hooks", GeometryDrawHooks, GeometryEngineDraws);
    hook("D3D12 draw hooks", GeometryCommandHooks, GeometryPublicDraws);
    ImGui::Text("Shaders: %llu compiled / %llu pending / %llu rejected",
                static_cast<unsigned long long>(health.counts[GeometryCompiled]),
                static_cast<unsigned long long>(health.counts[GeometryPending]),
                static_cast<unsigned long long>(health.counts[GeometryCompileRejected]));
    ImGui::Text("World draw joins: %llu; pipeline %llu; valid bindings %llu",
                static_cast<unsigned long long>(health.counts[GeometryPackets]),
                static_cast<unsigned long long>(health.counts[GeometryPipelineMatches]),
                static_cast<unsigned long long>(health.counts[GeometryBindingMatches]));
    ImGui::Text("Indirect commands: %llu known / %llu unknown",
                static_cast<unsigned long long>(health.counts[GeometryIndirectKnown]),
                static_cast<unsigned long long>(health.counts[GeometryIndirectUnknown]));
    ImGui::Text("Object MV captures: %llu; FG input replacements: %llu",
                static_cast<unsigned long long>(health.counts[GeometryCaptureDraws]),
                static_cast<unsigned long long>(health.counts[GeometryFgReplacements]));
    const auto packed = ReadPackedMotionCaptureStatus();
    ImGui::Text("Packed object path: %s; draws %llu; frames %llu; FG frames %llu",
                !packed.initialized ? "not initialized" : packed.healthy ? "healthy" : "failed",
                static_cast<unsigned long long>(packed.admittedDraws),
                static_cast<unsigned long long>(packed.capturedFrames),
                static_cast<unsigned long long>(packed.fgFrames));
    ImGui::Text("Skipped: pipeline %llu; identity %llu; topology %llu; capacity %llu; ordering %llu",
                static_cast<unsigned long long>(packed.missingPipeline),
                static_cast<unsigned long long>(packed.unknownIdentity),
                static_cast<unsigned long long>(packed.topologyRejected),
                static_cast<unsigned long long>(packed.mappingOverflow + packed.historyOverflow + packed.slotBusy),
                static_cast<unsigned long long>(packed.orderingRejected));
    ImGui::Text("N-1 element history: hits %llu; inserted %llu; live %llu",
                static_cast<unsigned long long>(packed.historyHits),
                static_cast<unsigned long long>(packed.historyInserted),
                static_cast<unsigned long long>(packed.historyLive));
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("inserted grows when an element key changes between frames, which means the\n"
                          "previous transform was not reused for that element.");
    // Stage verdicts: whether the composed inputs actually reach the FG
    // evaluation, and whether our own compose work is still executing. The
    // compose fence pair is what the session teardown waits for.
    // The host counter is the one the 9/10 injection point feeds: it counts the
    // engine's own DLSS-G evaluations that reach the NGX proxy. The tag-path
    // counter is a second, narrower route and must not stand in for it.
    const auto liveEvaluations = ReadLiveStatus(LiveStatusEvaluations);
    const auto liveTagFrames = ReadStreamlineFrameCalls();
    const auto liveSubstitutions = ReadLiveStatus(LiveStatusSubstitutions);
    const auto liveInFlight = ReadLiveStatus(LiveStatusComposeInFlight) != 0;
    const auto liveSubmitted = ReadLiveStatus(LiveStatusComposeSubmitted);
    const auto liveCompleted = ReadLiveStatus(LiveStatusComposeCompleted);
    const auto liveForced = ReadLiveStatus(LiveStatusComposeForced);
    const auto liveUnavailable = ReadLiveStatus(LiveStatusUnavailable) != 0;
    const auto liveRetiring = ReadLiveStatus(LiveStatusRetiring);
    ImGui::Text("FG swap: %llu applied / %llu evaluations%s",
                static_cast<unsigned long long>(liveSubstitutions),
                static_cast<unsigned long long>(liveEvaluations),
                liveUnavailable ? " (unavailable)" : liveRetiring ? " (draining previous session)" : "");
    if (!liveEvaluations)
    {
        ImGui::SameLine();
        ImGui::TextDisabled(value.packedSubstitute ? "- FG path has not reached the host hook yet"
                                                   : "- FG input swap is off");
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("The correction runs on the host's native DLSS-G call. It only runs when the\n"
                              "engine's frame generation actually reaches this module's NGX entry point.");
    }
    else if (!liveSubstitutions && value.packedSubstitute)
        ImGui::SameLine(), ImGui::TextDisabled("- swap requested but not applied");
    ImGui::Text("Streamline DLSS-G tag frames: %llu", static_cast<unsigned long long>(liveTagFrames));
    ImGui::Text("nvngx_dlssg evaluate hook: %s; calls %llu",
                NvngxDlssgHookInstalled() ? "installed" : "not installed",
                static_cast<unsigned long long>(NvngxDlssgHookCalls().load(std::memory_order_relaxed)));
    ImGui::Text("Compose fence: %s; submitted %llu; completed %llu%s", liveInFlight ? "in flight" : "idle",
                static_cast<unsigned long long>(liveSubmitted), static_cast<unsigned long long>(liveCompleted),
                liveForced ? " (forced release)" : "");
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("submitted > completed means the packed compose is still executing.\n"
                          "The session teardown now waits for it, so a save load cannot free it early.");
    ImGui::Text("Identity rejects: owner %llu; resolve %llu; raster %llu; shape %llu; viewport %llu",
                static_cast<unsigned long long>(packed.unknownOwnerSpan + packed.unknownOwnerMismatch),
                static_cast<unsigned long long>(packed.unknownResolve + packed.unknownFieldMismatch),
                static_cast<unsigned long long>(packed.rasterRejected),
                static_cast<unsigned long long>(packed.shapeRejected),
                static_cast<unsigned long long>(packed.viewportRejected));
    ImGui::Text("FG boundary misses: frame %llu; queue %llu; acquire n/s/a/b %llu/%llu/%llu/%llu",
                static_cast<unsigned long long>(packed.noFgFrame),
                static_cast<unsigned long long>(packed.noFgQueue),
                static_cast<unsigned long long>(packed.acquireNoCandidate),
                static_cast<unsigned long long>(packed.acquireStalePair),
                static_cast<unsigned long long>(packed.acquireAmbiguous),
                static_cast<unsigned long long>(packed.acquireConsumerBusy));
    const auto mapping = ReadArrayMappingStats();
    ImGui::Text("Array element mapping: entries %u; published %llu; replaced %llu; evictions %llu", mapping.entries,
                static_cast<unsigned long long>(mapping.published),
                static_cast<unsigned long long>(mapping.replaced),
                static_cast<unsigned long long>(mapping.evictions));
    ImGui::Text("Mapping lookups: %llu; hits %llu; misses %llu; out of range %llu",
                static_cast<unsigned long long>(mapping.lookups),
                static_cast<unsigned long long>(mapping.hits),
                static_cast<unsigned long long>(mapping.misses),
                static_cast<unsigned long long>(mapping.outOfRange));
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("misses = the queried object has no published entry; out of range = the entry\n"
                          "exists but the draw ordinal is outside its output range.");
    if (health.sampledMs && now >= health.sampledMs)
        ImGui::TextDisabled("Report %.1f s ago; engine frame %u", (now - health.sampledMs) / 1000.0, health.frame);
    ImGui::TextDisabled("UI samples once per second without waiting. Paused/menu scenes can stop progress.");
    ImGui::TextWrapped("Healthy capture and a rising FG frame count confirm the object buffer reached the FG correction pass.");
    ImGui::TextDisabled("Details: OptiScaler.Glass.log / OptiScaler.Glass.Geometry.log");
    ImGui::BeginDisabled();
    bool preview = false;
    ImGui::Checkbox("Show selected regions (pending runtime preview)", &preview);
    ImGui::EndDisabled();
    ImGui::PopID();
}
} // namespace GlassFg
