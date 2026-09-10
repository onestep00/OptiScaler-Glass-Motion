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
int wmain(int argc, wchar_t** argv)
{
    try
    {
        require(argc == 2, "directory required");
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
               "malformed_fallback=1 imgui_vertices=%d game_files_changed=0\n",
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
