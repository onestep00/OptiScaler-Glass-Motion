#pragma once
#include "../ExperimentCaptureOwner.h"
#include "../ExperimentControl.h"
#include "ExperimentClient.h"
#include <vector>
#include <future>
#include <thread>

class ModuleRecorderCheck
{
    GlassFg::ExperimentRuntime runtime;
    GlassFg::ExperimentCaptureOwner* owner = nullptr;
    std::filesystem::path loaded;
    std::vector<std::filesystem::path> loadedPaths;
    std::filesystem::path artifactsPath, compilerPath;
    ID3D12Device* observedDevice = nullptr;
    unsigned generations = 0, unloaded = 0;
    bool controlled = false;
    HANDLE shutdown = nullptr;
    std::thread controller;
    std::filesystem::path controlDirectory;
    inline static std::atomic<bool> submissionReady = false;
    static bool readyForCapture() { return submissionReady.load(); }
  public:
    void useControl() { controlled = true; }
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
        if (controlled)
        {
            if (!controller.joinable())
            {
                controlDirectory = session;
                shutdown = CreateEventW(nullptr, FALSE, FALSE, nullptr);
                require(shutdown != nullptr, "Control test shutdown event");
                auto ready = std::make_shared<std::promise<void>>(); auto future = ready->get_future();
                controller = std::thread([this, device, ready]
                {
                    try
                    {
                        auto* control = new GlassFg::ExperimentControl(device, controlDirectory, readyForCapture);
                        ready->set_value(); control->run(shutdown);
                    }
                    catch (...) { ready->set_exception(std::current_exception()); }
                });
                future.get();
                const auto blocked = ExperimentRequest(controlDirectory, GetCurrentProcessId(), "load", loaded);
                require(blocked.at("ok") == "0" && blocked.at("active_generation") == "0" &&
                        blocked.at("submission_observer_ready") == "0" && !std::filesystem::exists(output),
                        "Missing submission observation did not block capture preparation");
                submissionReady.store(true); // This fixture forwards every actual submission.
            }
            const auto result = ExperimentRequest(controlDirectory, GetCurrentProcessId(), "load", loaded);
            require(result.at("ok") == "1", "Control module load rejected");
            require(std::stoul(result.at("active_generation")) == generations + 1, "Control generation mismatch");
            const auto rejected = ExperimentRequest(controlDirectory, GetCurrentProcessId(), "load", loaded);
            require(rejected.at("ok") == "0" && rejected.at("active_generation") == result.at("active_generation"),
                    "Control duplicate load changed active module");
        }
        else runtime.replace(loaded, host);
        loadedPaths.push_back(loaded);
        ++generations;
        if (!owner && !controlled)
        {
            owner = new GlassFg::ExperimentCaptureOwner(runtime, device);
            require(GlassFg::RegisterGeometryDrawCapture(owner), "Module recorder owner registration");
        }
        return output;
    }
    std::filesystem::path replace() { return start(observedDevice, artifactsPath, compilerPath); }
    void collect() { if (!controlled) { if (owner) owner->collect(); unloaded += runtime.collect(); } }
    void finish()
    {
        if (controlled)
        {
            auto result = ExperimentRequest(controlDirectory, GetCurrentProcessId(), "disable");
            require(result.at("ok") == "1" && result.at("active_generation") == "0", "Control disable failed");
            const auto deadline = GetTickCount64() + 5000;
            while (result.at("loaded_modules") != "0" && GetTickCount64() < deadline)
            {
                Sleep(10); result = ExperimentRequest(controlDirectory, GetCurrentProcessId(), "status");
            }
            require(result.at("capture_recorded") == result.at("capture_retired") && result.at("capture_pending") == "0",
                    "Control capture retirement incomplete");
            unloaded = std::stoul(result.at("unloaded_modules"));
            SetEvent(shutdown); controller.join(); CloseHandle(shutdown); shutdown = nullptr;
        }
        else
        {
            require(owner != nullptr, "Module recorder not started");
            owner->stop(); runtime.disable(); owner->collect();
            // Unload joins the module's private worker, including pending file saves.
            unloaded += runtime.collect();
        }
        require(unloaded == generations, "Module recorder did not retire");
        for (const auto& path : loadedPaths) require(!GetModuleHandleW(path.c_str()), "Recorder DLL remained loaded");
    }
    ~ModuleRecorderCheck()
    {
        if (controller.joinable()) { SetEvent(shutdown); controller.join(); }
        if (shutdown) CloseHandle(shutdown);
    }
};
