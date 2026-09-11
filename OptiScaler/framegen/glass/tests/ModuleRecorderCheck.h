#pragma once
#include "../ExperimentCaptureOwner.h"
#include <vector>

class ModuleRecorderCheck
{
    GlassFg::ExperimentRuntime runtime;
    GlassFg::ExperimentCaptureOwner* owner = nullptr;
    std::filesystem::path loaded;
    std::vector<std::filesystem::path> loadedPaths;
    std::filesystem::path artifactsPath, compilerPath;
    ID3D12Device* observedDevice = nullptr;
    unsigned generations = 0, unloaded = 0;
  public:
    std::filesystem::path start(ID3D12Device* device, const std::filesystem::path& artifacts,
                                 const std::filesystem::path& compiler)
    {
        artifactsPath = artifacts; compilerPath = compiler; observedDevice = device;
        const auto session = std::filesystem::absolute(artifacts /
            (L"module-recorder-" + std::to_wstring(GetTickCount64()) + L"-\uAC80\uC0AC"));
        require(std::filesystem::create_directory(session), "Fresh module recorder session");
        loaded = session / "experiment-coverage.dll";
        std::filesystem::copy_file(artifacts / "experiment-coverage.dll", loaded);
        const auto output = session / "capture";
        auto config = loaded; config.replace_extension(L".config");
        std::ofstream file(config, std::ios::binary);
        for (const auto& path : { std::filesystem::absolute(compiler), output })
        {
            const auto utf8 = path.u8string();
            file.write(reinterpret_cast<const char*>(utf8.data()), std::streamsize(utf8.size())); file << '\n';
        }
        file.close(); require(bool(file), "Module recorder config write");
        const GlassExperimentHost host { sizeof(host), GLASS_EXPERIMENT_ABI, GlassExperimentCapture, device };
        runtime.replace(loaded, host);
        loadedPaths.push_back(loaded);
        ++generations;
        if (!owner)
        {
            owner = new GlassFg::ExperimentCaptureOwner(runtime, device);
            require(GlassFg::RegisterGeometryDrawCapture(owner), "Module recorder owner registration");
        }
        return output;
    }
    std::filesystem::path replace() { return start(observedDevice, artifactsPath, compilerPath); }
    void collect() { if (owner) owner->collect(); unloaded += runtime.collect(); }
    void finish()
    {
        require(owner != nullptr, "Module recorder not started");
        owner->stop(); runtime.disable(); owner->collect();
        // Unload joins the module's private worker, including pending file saves.
        unloaded += runtime.collect();
        require(unloaded == generations, "Module recorder did not retire");
        for (const auto& path : loadedPaths) require(!GetModuleHandleW(path.c_str()), "Recorder DLL remained loaded");
    }
};
