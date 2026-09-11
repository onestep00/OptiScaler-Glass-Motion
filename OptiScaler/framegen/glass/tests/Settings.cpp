#include "pch.h"
#include <cstdio>
#include <stdexcept>
#include <fstream>
#include "../GlassSettings.cpp"
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
    value.capabilities = 127;
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
        require(!initial.enabled && initial.strength == 100 && initial.measureGpuTime, "defaults");
        GlassFg::WriteControls({ true, 500 });
        auto current = GlassFg::ReadControls();
        require(current.enabled && current.strength == 100, "clamp");
        auto path = GlassFg::settingsPath();
        {
            std::ofstream f(path);
            f << "[Unrelated]\nKeep=preserved\n";
        }
        require(GlassFg::save({ true, 37 }), "save");
        GlassFg::WriteControls({ false, 1 });
        require(GlassFg::load(), "reload");
        current = GlassFg::ReadControls();
        require(current.enabled && current.strength == 37 && current.measureGpuTime, "round trip");
        CSimpleIniA ini;
        require(ini.LoadFile(path.c_str()) >= 0, "reread");
        require(std::string(ini.GetValue("Unrelated", "Keep", "")) == "preserved", "other section damaged");
        require(SetFileAttributesW(path.c_str(), FILE_ATTRIBUTE_READONLY), "set readonly");
        bool denied = GlassFg::save({ false, 99 });
        SetFileAttributesW(path.c_str(), FILE_ATTRIBUTE_NORMAL);
        require(!denied, "readonly overwrite");
        require(GlassFg::load() && GlassFg::ReadControls().strength == 37, "failed save changed original");
        {
            std::ofstream f(path);
            f << "[GlassFG]\nEnabled=true\nStrength=-25\n";
        }
        require(GlassFg::load() && GlassFg::ReadControls().strength == 0, "negative clamp");
        require(!GlassFg::ReadControls().active(), "zero active");
        {
            std::ofstream f(path);
            f << "[GlassFG]\nStrength=invalid\n";
        }
        require(GlassFg::load() && !GlassFg::ReadControls().enabled && GlassFg::ReadControls().strength == 100,
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
