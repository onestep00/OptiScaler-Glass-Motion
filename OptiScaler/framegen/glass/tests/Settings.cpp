#include "pch.h"
#include <cstdio>
#include <stdexcept>
#include <fstream>
#include "../GlassSettings.cpp"
namespace GlassFg
{
// Headless UI fixture has no game capture owner.
PackedMotionCaptureStatus ReadPackedMotionCaptureStatus() noexcept { return {}; }
}
static std::filesystem::path auditDirectory;
namespace Util
{
std::filesystem::path DllPath() { return auditDirectory / L"OptiScaler.dll"; }
} // namespace Util
void require(bool ok, const char* what)
{
    if (!ok)
        throw std::runtime_error(what);
}
static void healthChecks()
{
    GlassFg::GeometryHealth value;
    value.capabilities = 255;
    value.sampledMs = 1000;
    value.counts[GlassFg::GeometryPackets] = 100;
    value.counts[GlassFg::GeometryCompiled] = 5;
    value.counts[GlassFg::GeometryPipelineMatches] = 20;
    value.counts[GlassFg::GeometryBindingMatches] = 10;
    GlassFg::PublishGeometryHealth(value);
    auto observed = GlassFg::ReadGeometryHealth();
    require(!observed.recentlyApplied(1000), "Installed hooks falsely reported FG application");
    require(std::string(observed.reason(1000)).find("not connected") != std::string::npos,
            "Capture producer gap hidden");
    value.sampledMs = 1100;
    value.counts[GlassFg::GeometryCaptureDraws] = 10;
    GlassFg::PublishGeometryHealth(value);
    require(!GlassFg::ReadGeometryHealth().recentlyApplied(1100), "Capture alone reported FG application");
    value.sampledMs = 1200;
    value.counts[GlassFg::GeometryFgReplacements] = 3;
    GlassFg::PublishGeometryHealth(value);
    observed = GlassFg::ReadGeometryHealth();
    require(observed.recentlyApplied(1200) && !observed.recentlyApplied(16201), "FG freshness gate");
    value.sampledMs = 17000;
    value.counts[GlassFg::GeometryPackets] = 200;
    GlassFg::PublishGeometryHealth(value);
    require(std::string(GlassFg::ReadGeometryHealth().reason(17000)).find("matches have stopped") != std::string::npos,
            "Historical binding matches hid stopped progress");
    value.counts[GlassFg::GeometryPackets] = 50;
    GlassFg::PublishGeometryHealth(value);
    require(GlassFg::ReadGeometryHealth().counts[GlassFg::GeometryPackets] == 200, "Older report regressed counters");
    static unsigned refreshed = 0;
    GlassFg::GeometryTelemetry::refresh.store(+[] { ++refreshed; });
    GlassFg::RefreshGeometryHealthIfNeeded(1000);
    GlassFg::RefreshGeometryHealthIfNeeded(1500);
    GlassFg::RefreshGeometryHealthIfNeeded(2000);
    GlassFg::GeometryTelemetry::refresh.store(nullptr);
    require(refreshed == 2, "UI refresh is not throttled");
}
int wmain(int argc, wchar_t** argv)
{
    try
    {
        require(argc == 2, "directory required");
        healthChecks();
        auditDirectory = std::filesystem::absolute(argv[1]);
        require(std::filesystem::create_directory(auditDirectory), "fresh audit directory required");
        auto initial = GlassFg::ReadControls();
        require(!initial.enabled && initial.opacityPercent == 50 && initial.measureGpuTime, "defaults");
        GlassFg::WriteControls({ true, 500 });
        auto current = GlassFg::ReadControls();
        require(current.enabled && current.opacityPercent == 100, "clamp");
        auto path = GlassFg::settingsPath();
        {
            std::ofstream f(path);
            f << "[Unrelated]\nKeep=preserved\n";
        }
        require(GlassFg::save({ true, 37 }), "save");
        GlassFg::WriteControls({ false, 1 });
        require(GlassFg::load(), "reload");
        current = GlassFg::ReadControls();
        require(current.enabled && current.opacityPercent == 37 && current.measureGpuTime, "round trip");
        // The grouped-array order probe and the packet-order identity switch are
        // separate diagnostics: a live session must be able to turn the probe on
        // without changing which element key the correction uses.
        require(!current.arrayProbe && current.groupedOrder, "array probe default");
        {
            auto probe = current;
            probe.arrayProbe = true;
            probe.groupedOrder = false;
            require(GlassFg::save(probe), "save array probe");
        }
        require(GlassFg::load(), "reload array probe");
        current = GlassFg::ReadControls();
        require(current.arrayProbe && !current.groupedOrder, "array probe round trip");
        require(!current.packetLocalOrder, "packet-local order default");
        {
            auto local = current;
            local.packetLocalOrder = true;
            require(GlassFg::save(local), "save packet-local order");
        }
        require(GlassFg::load() && GlassFg::ReadControls().packetLocalOrder, "packet-local order round trip");
        // The capture path computes its object motion in jittered projection
        // space, so delivery subtracts the capture endpoint delta and the
        // compose term stays at zero (mode 0, gain 100). The value is fixed in
        // code: an old INI entry or the retired marker cannot select a
        // diagnostic mode, because a stale file once forced the compose term
        // back on and re-added about +1.0*J per frame. Only the live control
        // channel (glass-debug.request) opens the diagnostic modes.
        {
            auto defaults = GlassFg::ReadControls();
            require(defaults.jitterMode == 0 && defaults.jitterGain == 100, "jitter delivery default");
            auto stored = defaults;
            stored.jitterMode = 5;
            stored.jitterGain = 100;
            require(GlassFg::save(stored), "save a diagnostic jitter mode");
            CSimpleIniA file;
            require(file.LoadFile(path.c_str()) >= 0 &&
                        !file.KeyExists("GlassFG", "JitterCompensation") &&
                        !file.KeyExists("GlassFG", "JitterGainPercent"),
                    "jitter keys are not written");
            require(GlassFg::load() && GlassFg::ReadControls().jitterMode == 0 &&
                        GlassFg::ReadControls().jitterGain == 100,
                    "a file cannot select a delivery mode");
            file.SetLongValue("GlassFG", "JitterCompensation", 3);
            file.SetLongValue("GlassFG", "JitterGainPercent", 100);
            require(file.SaveFile(path.c_str()) >= 0, "write legacy jitter keys");
            require(GlassFg::load() && GlassFg::ReadControls().jitterMode == 0 &&
                        GlassFg::ReadControls().jitterGain == 100,
                    "legacy jitter keys cannot select a delivery mode");
            const auto marker = GlassFg::settingsPath().parent_path() / L"glass-jitter.on";
            {
                std::ofstream f(marker);
                f << "diagnostic\n";
            }
            require(GlassFg::load() && GlassFg::ReadControls().jitterMode == 0 &&
                        GlassFg::ReadControls().jitterGain == 100,
                    "the retired marker cannot select a delivery mode");
            std::error_code cleanup;
            std::filesystem::remove(marker, cleanup);
        }
        require(GlassFg::save({ true, 37 }) && GlassFg::load(), "restore array probe");
        require(!GlassFg::ReadControls().arrayProbe && GlassFg::ReadControls().groupedOrder &&
                    !GlassFg::ReadControls().packetLocalOrder,
                "array probe restored");
        // Renamed options: the current names round trip, the old spellings still
        // load, and saving leaves exactly one name owning each value.
        {
            auto renamed = GlassFg::ReadControls();
            renamed.farSkipStep = 6;
            renamed.packedRows = 900;
            renamed.arrayMapping = true;
            require(GlassFg::save(renamed), "save renamed options");
            require(GlassFg::load(), "reload renamed options");
            auto loaded = GlassFg::ReadControls();
            require(loaded.farSkipStep == 6 && loaded.packedRows == 900 && loaded.arrayMapping,
                    "renamed option round trip");
            CSimpleIniA renamedIni;
            require(renamedIni.LoadFile(path.c_str()) >= 0, "reread renamed options");
            require(renamedIni.GetLongValue("GlassFG", "SkipFartherThanMeters", -1) == 150, "far meters stored");
            require(renamedIni.GetLongValue("GlassFG", "ComposeRows", -1) == 900, "compose rows stored");
            require(std::string(renamedIni.GetValue("GlassFG", "PackedRows", "")) == "", "legacy rows removed");
            require(std::string(renamedIni.GetValue("GlassFG", "ArrayMapping", "")) == "",
                    "legacy array mapping removed");
        }
        {
            std::ofstream f(path);
            f << "[GlassFG]\nEnabled=true\nPackedRows=777\nGroupedOrder=false\nSkipFartherThanMeters=50\n";
        }
        require(GlassFg::load(), "legacy names reload");
        {
            auto legacy = GlassFg::ReadControls();
            require(legacy.packedRows == 777 && !legacy.groupedOrder && legacy.farSkipStep == 2,
                    "legacy names honored");
        }
        require(GlassFg::save({ true, 37 }) && GlassFg::load(), "restore renamed");
        require(GlassFg::ReadControls().packedRows == 240 && GlassFg::ReadControls().groupedOrder &&
                    !GlassFg::ReadControls().arrayMapping && GlassFg::ReadControls().farSkipStep == 0,
                "renamed restored");
        // The rename blocks rewrote the whole file; restore the unrelated
        // section and the threshold the read-only check below expects.
        {
            std::ofstream f(path);
            f << "[Unrelated]\nKeep=preserved\n[GlassFG]\nEnabled=true\nInteriorOpacityPercent=37\n";
        }
        CSimpleIniA ini;
        require(ini.LoadFile(path.c_str()) >= 0, "reread");
        require(std::string(ini.GetValue("Unrelated", "Keep", "")) == "preserved", "other section damaged");
        require(SetFileAttributesW(path.c_str(), FILE_ATTRIBUTE_READONLY), "set readonly");
        bool denied = GlassFg::save({ false, 99 });
        SetFileAttributesW(path.c_str(), FILE_ATTRIBUTE_NORMAL);
        require(!denied, "readonly overwrite");
        require(GlassFg::load() && GlassFg::ReadControls().opacityPercent == 37, "failed save changed original");
        {
            std::ofstream f(path);
            f << "[GlassFG]\nEnabled=true\nInteriorOpacityPercent=-25\n";
        }
        require(GlassFg::load() && GlassFg::ReadControls().opacityPercent == 0, "negative clamp");
        // Threshold 0 is a valid request: every covered pixel takes the object
        // motion, so the correction stays active.
        require(GlassFg::ReadControls().active(), "zero threshold active");
        {
            std::ofstream f(path);
            f << "[GlassFG]\nInteriorOpacityPercent=invalid\n";
        }
        require(GlassFg::load() && !GlassFg::ReadControls().enabled && GlassFg::ReadControls().opacityPercent == 50,
                "malformed fallback");
        GlassFg::WriteControls({ true, 50, true });
        GlassFg::PublishGpuMilliseconds(.123);
        ImGui::CreateContext();
        auto& io = ImGui::GetIO();
        io.IniFilename = nullptr;
        io.DisplaySize = ImVec2(1000, 800);
        io.DeltaTime = 1.f / 60.f;
        io.BackendFlags |= ImGuiBackendFlags_RendererHasTextures;
        for (unsigned i = 0; i < 2; ++i)
        {
            ImGui::NewFrame();
            ImGui::SetNextWindowPos(ImVec2(0, 0));
            ImGui::SetNextWindowSize(ImVec2(900, 700));
            ImGui::Begin("Glass settings audit");
            ImGui::SetNextItemOpen(true, ImGuiCond_Always);
            GlassFg::RenderSettings();
            ImGui::End();
            ImGui::Render();
        }
        auto draw = ImGui::GetDrawData();
        require(draw && draw->TotalVtxCount > 0, "UI empty");
        printf("SETTINGS_OK defaults=1 clamp=1 round_trip=1 unrelated_preserved=1 failed_save_preserves_original=1 "
               "malformed_fallback=1 hook_only_not_applied=1 capture_only_not_applied=1 stale_evidence=1 "
               "health_refresh_throttled=1 imgui_vertices=%d game_files_changed=0\n",
               draw->TotalVtxCount);
        ImGui::DestroyContext();
        return 0;
    }
    catch (const std::exception& e)
    {
        fprintf(stderr, "FAILED %s\n", e.what());
        return 1;
    }
}
