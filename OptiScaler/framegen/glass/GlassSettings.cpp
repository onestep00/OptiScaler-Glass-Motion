#include "pch.h"
#include "GlassControls.h"
#include "GlassArrayMapping.h"
#include "GeometryHealth.h"
#include "PackedMotionCapture.h"
#include "NvngxDlssgBridge.h"
#include "GlassHookProbe.h"
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
        // No settings file: the delivery convention is fixed and the hot hooks
        // stay idle.
        SetHooksIdle(true);
        return true;
    }
    if (error || ini.LoadFile(path.c_str()) < 0)
        return false;
    latestMilliseconds.store(-1.0, std::memory_order_relaxed);
    // User-facing names first, legacy key as the fallback so an old file keeps
    // working. The legacy spellings are removed on the next save.
    const auto flag = [&ini](const char* name, const char* legacy, bool fallback)
    {
        if (ini.GetValue("GlassFG", name, nullptr))
            return ini.GetBoolValue("GlassFG", name, fallback);
        return ini.GetBoolValue("GlassFG", legacy, fallback);
    };
    // Interior opacity threshold in percent. The old InteriorFollowPercent was
    // a blend strength, so it is not read as a threshold: an old file falls back
    // to the conservative default and the legacy key is removed on the next
    // save.
    const auto opacityPercent = ini.GetLongValue("GlassFG", "InteriorOpacityPercent", 50);
    const auto edgeWidth = ini.GetLongValue("GlassFG", "BorderWidthPx",
                                            ini.GetLongValue("GlassFG", "EdgeWidth", 2));
    // The delivered path removes the current frame's projection jitter at the
    // capture endpoint: the vertex stage adds the frame-to-frame difference of
    // the camera constant words and the compose adds nothing (mode 0, gain
    // 100). The capture records the object's own motion; the delta converts its
    // convention to the engine's. It is fixed here and only the live control
    // channel can select another diagnostic mode, so no INI option reaches the
    // delivered value.
    const auto zeroMotion = ini.GetBoolValue("GlassFG", "ZeroFrameGenerationMotion",
                                             ini.GetBoolValue("GlassFG", "ZeroMotion", false));
    // Second consumer: the composed motion/depth are handed to the DLSS-NR
    // evaluate as its own inputs. The engine's textures are not written, so the
    // option cannot reach DLSS-SR, Ray Reconstruction or the ray traced passes.
    const auto nrMotion = ini.GetBoolValue("GlassFG", "NrMotion", false);
    // Distance in metres; stored as 25 m steps, 0 = keep every surface.
    const auto farMeters = ini.GetLongValue("GlassFG", "SkipFartherThanMeters", 0);
    // Diagnostic opaque-pipeline probe, default off. Persisted so an offline
    // window can pre-arm it, and overridable live through opaqueprobe=on|off.
    // The controls word below does not carry it: this flag is session
    // diagnostic state, like depthkeep.
    SetOpaqueProbe(ini.GetBoolValue("GlassFG", "OpaqueProbe", false));
    // Engine-supply policy for the packed capture (GlassControls.h). Both are
    // session values outside the full controls word, like OpaqueProbe.
    SetVertexHistoryFallback(ini.GetBoolValue("GlassFG", "VertexHistoryFallback", false));
    SetGraftClassMask(static_cast<unsigned>(
        std::clamp(ini.GetLongValue("GlassFG", "GraftClassMask", long(GraftClassRootOnly)), 0L, long(GraftClassAll))));
    controls.store(Controls { ini.GetBoolValue("GlassFG", "Enabled", true),
                              static_cast<unsigned>(std::clamp(opacityPercent, 0L, 100L)),
                              ini.GetBoolValue("GlassFG", "MeasureGpuTime", true),
                              static_cast<unsigned>(std::clamp(edgeWidth, 1L, 4L)),
                              flag("ComposePass", "PackedDispatch", true),
                              static_cast<unsigned>(std::clamp(
                                  ini.GetLongValue("GlassFG", "ComposeRows",
                                                   ini.GetLongValue("GlassFG", "PackedRows", 32768)),
                                  1L, 32768L)),
                              flag("ReplaceFrameGenerationInputs", "PackedSubstitute", true),
                              flag("StepTrace", "Trace", false),
                              flag("StagedRamp", "AutoStage", false),
                              flag("ComposeCompute", "PackedCompute", true),
                              flag("SkipRecordRead", "PackedSkipRead", false),
                              flag("RewriteShaders", "CompilePipelines", true),
                              flag("PacketOrderElementIdentity", "GroupedOrder", true),
                              flag("MeasurePacketOrder", "ArrayProbe", false),
                              flag("PacketOrdinalElementIdentity", "PacketLocalOrder", false),
                              flag("ReadTimeDelivery", "PackedSupply", false),
                              flag("TransparencyLayerDelivery", "PackedLayer", false),
                              // Product default, identical to the struct default:
                              // the capture endpoint delta already removes the
                              // projection jitter, so the compose adds nothing.
                              // A file cannot select a different mode; only the
                              // live diagnostic channel can.
                              0u, 100u, zeroMotion, nrMotion,
                               static_cast<unsigned>(std::clamp((farMeters + 12L) / 25L, 0L, 15L)) }
                       .packed(),
                   std::memory_order_relaxed);
    SetGeometryPipelineCompilation(Controls::unpack(controls.load(std::memory_order_relaxed)).compilePipelines);
    SetHooksIdle(!Controls::unpack(controls.load(std::memory_order_relaxed)).enabled);
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
    ini.SetLongValue("GlassFG", "InteriorOpacityPercent", std::min(value.opacityPercent, 100u));
    ini.SetBoolValue("GlassFG", "MeasureGpuTime", value.measureGpuTime);
    ini.SetLongValue("GlassFG", "BorderWidthPx", std::clamp(value.edgeWidth, 1u, 4u));
    ini.SetBoolValue("GlassFG", "ComposePass", value.packedDispatch);
    ini.SetLongValue("GlassFG", "ComposeRows", std::clamp(value.packedRows, 1u, 32768u));
    ini.SetBoolValue("GlassFG", "ReplaceFrameGenerationInputs", value.packedSubstitute);
    ini.SetBoolValue("GlassFG", "StepTrace", value.trace);
    ini.SetBoolValue("GlassFG", "StagedRamp", value.autoStage);
    ini.SetBoolValue("GlassFG", "ComposeCompute", value.packedCompute);
    ini.SetBoolValue("GlassFG", "ReadTimeDelivery", value.packedSupply);
    ini.SetBoolValue("GlassFG", "TransparencyLayerDelivery", value.packedLayer);
    ini.SetBoolValue("GlassFG", "SkipRecordRead", value.packedSkipRead);
    ini.SetBoolValue("GlassFG", "RewriteShaders", value.compilePipelines);
    ini.SetBoolValue("GlassFG", "PacketOrderElementIdentity", value.groupedOrder);
    ini.SetBoolValue("GlassFG", "MeasurePacketOrder", value.arrayProbe);
    ini.SetBoolValue("GlassFG", "PacketOrdinalElementIdentity", value.packetLocalOrder);
    ini.SetBoolValue("GlassFG", "ZeroFrameGenerationMotion", value.zeroMotion);
    ini.SetBoolValue("GlassFG", "NrMotion", value.nrMotion);
    ini.SetBoolValue("GlassFG", "OpaqueProbe", OpaqueProbeEnabled());
    ini.SetBoolValue("GlassFG", "VertexHistoryFallback", VertexHistoryFallbackEnabled());
    ini.SetLongValue("GlassFG", "GraftClassMask", ReadGraftClassMask());
    ini.SetLongValue("GlassFG", "SkipFartherThanMeters", std::min(value.farSkipStep, 15u) * 25u);
    // Legacy spellings and retired options are removed so the file has exactly
    // one name per live option.
    for (const auto* legacy : { "Strength", "EdgeWidth", "InteriorFollowPercent", "InteriorLimitPx",
                                "InteriorMaxPx", "JitterMode", "JitterGain", "JitterCompensation",
                                "JitterGainPercent", "ZeroMotion", "WriteBackMotion",
                                "MotionWriteBack", "PackedWriteBack", "EngineArrayElementMapping",
                                "PackedDispatch", "PackedRows", "PackedSubstitute", "Trace", "AutoStage",
                                "PackedCompute", "PackedSupply", "PackedLayer", "PackedSkipRead",
                                "ArrayMapping", "CompilePipelines", "GroupedOrder", "ArrayProbe",
                                "PacketLocalOrder" })
        ini.Delete("GlassFG", legacy, false);
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
        previous.opacityPercent != value.opacityPercent || previous.edgeWidth != value.edgeWidth)
        latestMilliseconds.store(-1.0, std::memory_order_relaxed);
    controls.store(value.packed(), std::memory_order_relaxed);
    SetGeometryPipelineCompilation(value.compilePipelines);
    // "Enable correction" off must cost nothing: every hot hook returns after
    // one relaxed load, so the frame is byte-identical to the unmodded game.
    SetHooksIdle(!value.enabled);
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
    const bool open = ImGui::CollapsingHeader("Glass motion for Frame Generation (experimental)##GlassFG");
    const auto now = GetTickCount64();
    RefreshGeometryHealthIfNeeded(now);
    const auto health = ReadGeometryHealth();
    const bool applied = health.recentlyApplied(now);
    ImGui::TextColored(applied ? ImVec4(.4f, .85f, .4f, 1) : ImVec4(1, .7f, .3f, 1), "%s",
                       applied ? "Glass motion correction: running"
                               : "Glass motion correction: not reaching frame generation yet");
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Green means the corrected glass motion reached the frame generator in the last second.");
    if (!open)
        return;
    ImGui::PushID("GlassFG");
    auto value = ReadControls();
    // Every option states in one line what it changes in the picture, so the
    // panel reads without hovering. Tooltips keep the measured detail and the
    // INI key names stay next to the values they store.
    const auto describe = [](const char* text)
    {
        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
        ImGui::Indent(14.f);
        ImGui::TextWrapped("%s", text);
        ImGui::Unindent(14.f);
        ImGui::PopStyleColor();
    };
    ImGui::TextWrapped("Gives glass its own motion and depth to DLSS Frame Generation. Without the "
                       "correction the generated frames drag the transparent surface along with "
                       "whatever is visible behind it.");
    describe("Frame generation only: Super Resolution, Ray Reconstruction and ray tracing keep reading "
             "the engine's own motion vectors.");
    ImGui::SeparatorText("Everyday settings");
    bool changed = ImGui::Checkbox("Enable correction", &value.enabled);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Turns the whole correction off without unloading the module.");
    describe("Off leaves the picture exactly as the game renders it.");
    int opacityThreshold = static_cast<int>(value.opacityPercent);
    changed |= ImGui::SliderInt("Inside opacity threshold", &opacityThreshold, 0, 100, "%d%%");
    value.opacityPercent = static_cast<unsigned>(opacityThreshold);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Pixels whose opacity reaches this value take the exact glass\n"
                          "motion and depth. Light a surface adds (holograms, rain, glows)\n"
                          "counts by its displayed brightness: where it hides the background,\n"
                          "it is treated like an opaque surface. Pixels below it keep the\n"
                          "engine's motion, so the content seen through the glass stays on\n"
                          "the background.\n"
                          "0%% gives every covered pixel the glass motion.\n"
                          "100%% keeps all but solid surfaces and full-white light on the background.\n"
                          "Border pixels always take the exact glass motion while correction is on.");
    describe("Opacity above which a covered pixel takes the exact glass motion and depth; light a surface "
             "adds counts by its brightness. Below it the engine's motion is kept unchanged. The border "
             "always follows the glass. INI: InteriorOpacityPercent.");
    int edgeWidth = static_cast<int>(value.edgeWidth);
    changed |= ImGui::SliderInt("Border width", &edgeWidth, 1, 4, "%d px");
    value.edgeWidth = static_cast<unsigned>(edgeWidth);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("This many pixels inside every visible glass border take the glass surface\n"
                          "motion and the glass surface depth without blending.");
    describe("Pixels along each glass outline that take the exact glass motion and depth, with no "
             "blending. Raise it when a moving outline still leaks the background. INI: BorderWidthPx.");
    describe("The delivered motion is the captured object motion itself with the current frame's "
             "projection jitter removed. No strength, blend or user gain reaches the delivered value.");
    ImGui::SeparatorText("Diagnostics and second consumer");
    changed |= ImGui::Checkbox("Deliver zero motion to Frame Generation", &value.zeroMotion);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Sends an all-zero motion field to DLSS Frame Generation while depth keeps\n"
                          "the engine's value. This is the \"no motion vectors at all\" baseline for\n"
                          "comparing FG artifacts. The game's own motion texture is not written.");
    describe("Diagnostic baseline. The generated frames have no motion to follow at all. "
             "INI: ZeroFrameGenerationMotion.");
    changed |= ImGui::Checkbox("Also give the corrected motion to DLSS-NR", &value.nrMotion);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Hands the composed transparent-object motion and depth to the DLSS 5 neural\n"
                          "rendering pass through its own evaluate inputs, so its temporal history\n"
                          "reprojects the glass with the object's motion instead of the background's.\n"
                          "The engine's own textures are not written, so DLSS Super Resolution, Ray\n"
                          "Reconstruction and the ray traced passes keep the game's values.");
    describe("Off keeps the correction inside frame generation only. On substitutes the guides for the "
             "neural rendering evaluate as well. INI: NrMotion.");
    static const char* farItems =
        "Off\0" "25 m\0" "50 m\0" "75 m\0" "100 m\0" "125 m\0" "150 m\0" "175 m\0"
        "200 m\0" "225 m\0" "250 m\0" "275 m\0" "300 m\0" "325 m\0" "350 m\0" "375 m\0";
    int farStep = static_cast<int>(std::min(value.farSkipStep, 15u));
    if (ImGui::Combo("Ignore glass farther than", &farStep, farItems))
    {
        value.farSkipStep = static_cast<unsigned>(farStep);
        changed = true;
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Distant level-of-detail glass is dropped before capture: no motion record,\n"
                          "no correction and no per-frame cost for it. The comparison uses the\n"
                          "surface's view distance in metres. \"Off\" keeps every distance.");
    describe("Distant level-of-detail glass costs nothing per frame and keeps the engine's own motion. "
             "\"Off\" corrects every distance. INI: SkipFartherThanMeters.");
    const auto milliseconds = latestMilliseconds.load(std::memory_order_relaxed);
    if (!value.active())
        ImGui::TextDisabled("GPU correction: inactive");
    else if (!value.measureGpuTime)
        ImGui::TextDisabled("GPU correction: timing disabled (INI MeasureGpuTime)");
    else if (milliseconds >= 0.0)
        ImGui::Text("GPU correction: %.3f ms (last sample)", milliseconds);
    else
        ImGui::TextDisabled("GPU correction: waiting for a completed sample");
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Reads completed results without waiting. With the packed object path active\n"
                          "this is the compose GPU time: engine input copies plus the correction dispatch.\n"
                          "DLSS-G evaluation and surface capture are excluded.");
    static const char* result = nullptr;
    if (ImGui::Button("Save"))
        result = save(value) ? "Settings saved to OptiScaler.Glass.ini." : "Could not save settings.";
    ImGui::SameLine();
    if (ImGui::Button("Reload"))
        result = load() ? "Settings reloaded from OptiScaler.Glass.ini." : "Could not reload settings.";
    ImGui::SameLine();
    if (ImGui::Button("Recommended"))
    {
        value.enabled = true;
        value.opacityPercent = 50;
        value.edgeWidth = 2;
        value.jitterMode = 0;
        value.jitterGain = 100;
        changed = true;
        result = "Recommended values applied; press Save to keep them.";
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Correction on, 2 px border, half of the material opacity as the\n"
                          "inside threshold, camera jitter compensation at the measured default.");
    ImGui::SameLine();
    if (ImGui::Button("Reset all"))
    {
        value = Controls {};
        changed = true;
        result = "Built-in defaults restored; press Save to keep them.";
    }
    if (result)
        ImGui::TextWrapped("%s", result);
    ImGui::TextDisabled("Saved separately in OptiScaler.Glass.ini next to the module.");
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
    // Short delivery lines stay visible: they are the "did it arrive" check.
    const auto liveEvaluations = ReadLiveStatus(LiveStatusEvaluations);
    const auto liveSubstitutions = ReadLiveStatus(LiveStatusSubstitutions);
    const auto liveInFlight = ReadLiveStatus(LiveStatusComposeInFlight) != 0;
    const auto liveSubmitted = ReadLiveStatus(LiveStatusComposeSubmitted);
    const auto liveCompleted = ReadLiveStatus(LiveStatusComposeCompleted);
    const auto liveForced = ReadLiveStatus(LiveStatusComposeForced);
    const auto liveUnavailable = ReadLiveStatus(LiveStatusUnavailable) != 0;
    const auto liveRetiring = ReadLiveStatus(LiveStatusRetiring);
    ImGui::SeparatorText("Delivery to frame generation");
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
    ImGui::Text("Compose fence: %s; submitted %llu; completed %llu%s", liveInFlight ? "in flight" : "idle",
                static_cast<unsigned long long>(liveSubmitted), static_cast<unsigned long long>(liveCompleted),
                liveForced ? " (forced release)" : "");
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("submitted > completed means the packed compose is still executing.\n"
                          "The session teardown now waits for it, so a save load cannot free it early.");
    ImGui::TextDisabled("FG-only: DLSS-SR / Ray Reconstruction / ray tracing inputs are never modified");
    if (ImGui::TreeNode("Advanced and diagnostics##GlassFG-advanced"))
    {
        ImGui::TextWrapped("The same pipeline split into stages, plus the switches used to isolate a "
                           "failure or measure behavior. The defaults are correct for a normal run.");
        if (ImGui::TreeNode("Delivery to frame generation##GlassFG-delivery"))
        {
            bool packedDispatch = value.packedDispatch;
            changed |= ImGui::Checkbox("Run the object-motion compose pass", &packedDispatch);
            value.packedDispatch = packedDispatch;
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Runs the packed object-motion compose pass that feeds object motion into the FG inputs.\n"
                                  "Disable it to isolate driver or stability issues.");
            describe("Off removes the correction stage entirely; only the game's own motion is left. "
                     "INI: ComposePass.");
            int packedRows = static_cast<int>(value.packedRows);
            changed |= ImGui::SliderInt("Rows to correct (staging limit)", &packedRows, 1, 32768, "%d rows");
            value.packedRows = static_cast<unsigned>(packedRows);
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Safety limit: only the top rows receive the packed correction.\n"
                                  "Raise it after a clean run; the rest keeps the original motion.");
            describe("Only this many rows from the top receive the correction. 1440 = the whole frame "
                     "at a 1440-row target. INI: ComposeRows.");
            bool packedCompute = value.packedCompute;
            changed |= ImGui::Checkbox("Compute the compose pass", &packedCompute);
            value.packedCompute = packedCompute;
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Off keeps the input copies and the FG swap but skips the compose\n"
                                  "dispatch, so the swap can be tested with no new GPU work.");
            describe("Off keeps the input copies and the delivery but skips the GPU dispatch. "
                     "INI: ComposeCompute.");
            bool packedSubstitute = value.packedSubstitute;
            changed |= ImGui::Checkbox("Replace frame-generation motion and depth", &packedSubstitute);
            value.packedSubstitute = packedSubstitute;
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Swaps the FG motion/depth inputs for the composed object-motion outputs.\n"
                                  "Off runs the dispatch without touching the FG inputs to isolate driver resets.");
            describe("Off runs everything but gives frame generation the original engine inputs. "
                     "INI: ReplaceFrameGenerationInputs.");
            bool packedSupply = value.packedSupply;
            changed |= ImGui::Checkbox("Deliver through motion/depth input reads", &packedSupply);
            value.packedSupply = packedSupply;
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Returns the composed motion and depth for every read of those keys.\n"
                                  "The provider copies input pointers on some evaluations and reuses them\n"
                                  "on the rest, so a swap around a single call reaches only some frames.");
            describe("A second delivery route for the same values, answering input reads instead of "
                     "swapping the table around the call. INI: ReadTimeDelivery.");
            bool packedLayer = value.packedLayer;
            changed |= ImGui::Checkbox("Deliver as a DLSS-G transparency layer", &packedLayer);
            value.packedLayer = packedLayer;
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Answers DLSS.TransparencyLayerMvecs with the composed motion and\n"
                                  "DLSS.TransparencyLayerOpacity with the packed coverage weight, so the\n"
                                  "generator can treat the transparent surface as its own layer.");
            describe("Offers the glass as its own layer (motion plus coverage) instead of only replacing "
                     "motion. INI: TransparencyLayerDelivery.");
            ImGui::TreePop();
        }
        if (ImGui::TreeNode("Object identity: arrays, particles##GlassFG-identity"))
        {
            ImGui::TextWrapped("How repeated transparent objects are matched to their own previous "
                               "frame. The engine exposes no element id at the draw site, so each "
                               "switch below is a guess that the order probe has to confirm first.");
            // Retired 2026-09-17. The grouped-array element mapping is published
            // by the module's own engine group hooks and consumed without a
            // switch, so the old toggle changed nothing. The INI key and the
            // `arraymap` control request are still parsed and stored, and the
            // live counters stay in the status tree above.
            ImGui::Text("Matches repeated objects by the engine's own element index.");
            describe("The mapping is supplied by the module's group hooks; the retired "
                     "EngineArrayElementMapping switch is only stored.");
            bool groupedOrder = value.groupedOrder;
            changed |= ImGui::Checkbox("Grouped arrays: packet order is the element", &groupedOrder);
            value.groupedOrder = groupedOrder;
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Grouped update arrays (flag 0x2000) expose no source index at the draw\n"
                                  "site. The packet ordinal is used as the element position and the array's\n"
                                  "observed lifetime generation invalidates it whenever the array mutates.\n"
                                  "Turn off if a repacked group shows a one-frame ghost.");
            describe("Repeated objects in a group are matched by their draw order; any change to the "
                     "group drops the history. INI: PacketOrderElementIdentity.");
            bool packetLocalOrder = value.packetLocalOrder;
            changed |= ImGui::Checkbox("Particles and instanced glass: packet order is the element", &packetLocalOrder);
            value.packetLocalOrder = packetLocalOrder;
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Particles and other instanced transparency use the engine's packet-local\n"
                                  "selection, which exposes no source index. Enabling this keys those elements\n"
                                  "by packet ordinal and the observed lifetime generation. Turn it on only\n"
                                  "after the order probe reports no permutation for that family.");
            describe("Same rule as above for particles and instanced transparency. INI: "
                     "PacketOrdinalElementIdentity.");
            bool arrayProbe = value.arrayProbe;
            changed |= ImGui::Checkbox("Measure whether packet order is stable", &arrayProbe);
            value.arrayProbe = arrayProbe;
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Hashes the element bytes each grouped array packet receives and compares\n"
                                  "them with the previous frame for the same array. If the element set is\n"
                                  "unchanged while the positions move, the packet ordinal is not a stable\n"
                                  "element identity and the counter reports it.");
            describe("Detection only: counts how often a group is reordered without changing its "
                     "contents. INI: MeasurePacketOrder.");
            ImGui::TreePop();
        }
        if (ImGui::TreeNode("Isolation and logging##GlassFG-isolation"))
        {
            bool compilePipelines = value.compilePipelines;
            changed |= ImGui::Checkbox("Rewrite engine shaders", &compilePipelines);
            value.compilePipelines = compilePipelines;
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Off leaves the D3D12 hooks in place but never creates the rewritten\n"
                                  "pipelines, to separate hook problems from pipeline problems.");
            describe("Off keeps every hook installed but never builds the rewritten shaders. "
                     "INI: RewriteShaders.");
            bool packedSkipRead = value.packedSkipRead;
            changed |= ImGui::Checkbox("Skip reading captured object records", &packedSkipRead);
            value.packedSkipRead = packedSkipRead;
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Runs the compose dispatch without reading the object records the capture\n"
                                  "raster wrote, so a GPU stall separates the dispatch from the read.");
            describe("Diagnostic: the dispatch runs without the captured record read. INI: SkipRecordRead.");
            bool trace = value.trace;
            changed |= ImGui::Checkbox("Step trace log", &trace);
            value.trace = trace;
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Writes one flushed line per dispatch, substitution and submit step.\n"
                                  "A driver reset then leaves the last executed step in the log.");
            describe("One flushed log line per pipeline step, so a crash points at the step. "
                     "INI: StepTrace.");
            bool autoStage = value.autoStage;
            changed |= ImGui::Checkbox("Automatic staged ramp", &autoStage);
            value.autoStage = autoStage;
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Unattended order: 240 rows, then full rows, then the FG input swap.\n"
                                  "Each step is logged as AUTO_STAGE so a reset is attributable.");
            describe("Unattended ramp: 240 rows, then full rows, then the input swap. INI: StagedRamp.");
            ImGui::TreePop();
        }
        ImGui::TreePop();
    }
    // The advanced switches above also land in the same snapshot. The everyday
    // block already stored its own value; storing again is idempotent.
    if (changed)
        WriteControls(value);
    ImGui::SeparatorText("Status details");
    ImGui::TextWrapped("%s", health.reason(now));
    if (health.sampledMs && now >= health.sampledMs)
        ImGui::TextDisabled("Report %.1f s ago; engine frame %u", (now - health.sampledMs) / 1000.0, health.frame);
    ImGui::TextDisabled("UI samples once per second without waiting. Paused/menu scenes can stop progress.");
    ImGui::TextWrapped("Healthy capture and a rising FG frame count confirm the object buffer reached the FG correction pass.");
    ImGui::TextDisabled("Details: OptiScaler.Glass.log / OptiScaler.Glass.Geometry.log");
    if (ImGui::TreeNode("Diagnostics counters##GlassFG-counters"))
    {
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
        ImGui::Text("nvngx_dlssg evaluate hook: %s; create hook: %s; calls %llu",
                    NvngxDlssgHookInstalled() ? "installed" : "not installed",
                    NvngxDlssgCreateHookFlag().load(std::memory_order_relaxed) ? "installed" : "not installed",
                    static_cast<unsigned long long>(NvngxDlssgHookCalls().load(std::memory_order_relaxed)));
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("The create hook is what proves a handle belongs to NVSDK_NGX_Feature_FrameGeneration\n"
                              "when the driver-level evaluation names only MotionVectors/Depth. Without it the\n"
                              "correction cannot tell the generator from the upscaler or Ray Reconstruction.");
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
        ImGui::BeginDisabled();
        bool preview = false;
        ImGui::Checkbox("Show selected regions (pending runtime preview)", &preview);
        ImGui::EndDisabled();
        ImGui::TreePop();
    }
    ImGui::PopID();
}
} // namespace GlassFg
