#include "pch.h"
#include "GlassControls.h"
#include "GeometryHealth.h"
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
std::atomic<uint32_t> controls { Controls {}.packed() };
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
                              ini.GetBoolValue("GlassFG", "MeasureGpuTime", true) }
                       .packed(),
                   std::memory_order_relaxed);
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
        previous.strength != value.strength)
        latestMilliseconds.store(-1.0, std::memory_order_relaxed);
    controls.store(value.packed(), std::memory_order_relaxed);
}

void PublishGpuMilliseconds(double milliseconds)
{
    if (std::isfinite(milliseconds) && milliseconds >= 0.0)
        latestMilliseconds.store(milliseconds, std::memory_order_relaxed);
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
        ImGui::SetTooltip("Lower values require stronger surface evidence and correct fewer regions.\n"
                          "0%% bypasses correction. 100%% uses the validated candidate thresholds.\n"
                          "Surface and background motion vectors are not averaged.");
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
        ImGui::SetTooltip("Samples one in 30 rendered frames. Reads completed results without waiting.\n"
                          "Measures correction input copies and compute passes; excludes DLSS-G and surface capture.");

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
            ImGui::TextWrapped("Static-surface correction active (experimental).");
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
    if (health.sampledMs && now >= health.sampledMs)
        ImGui::TextDisabled("Report %.1f s ago; engine frame %u", (now - health.sampledMs) / 1000.0, health.frame);
    ImGui::TextDisabled("UI samples once per second without waiting. Paused/menu scenes can stop progress.");
    ImGui::TextWrapped(
        "A working hook does not prove object MV is applied. This build still lacks the object capture/FG producer.");
    ImGui::TextDisabled("Details: OptiScaler.Glass.log / OptiScaler.Glass.Geometry.log");
    ImGui::BeginDisabled();
    bool preview = false;
    ImGui::Checkbox("Show selected regions (pending runtime preview)", &preview);
    ImGui::EndDisabled();
    ImGui::PopID();
}
} // namespace GlassFg
