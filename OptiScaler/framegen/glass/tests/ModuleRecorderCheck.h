#pragma once
#include "../ExperimentCaptureOwner.h"
#include "../ExperimentControl.h"
#include "ExperimentClient.h"
#include "ObservedCaptureQueue.h"
#include <vector>
#include <future>
#include <thread>
#include <sstream>

class ModuleRecorderCheck
{
    GlassFg::ExperimentRuntime runtime;
    GlassFg::ExperimentCensusObserver census { &runtime,
        [](void* p) noexcept { return static_cast<GlassFg::ExperimentRuntime*>(p)->censusEnabled(); },
        [](void* p, const GlassExperimentEvent& e) noexcept
        { try { static_cast<GlassFg::ExperimentRuntime*>(p)->observe(e); } catch (...) {} } };
    GlassFg::ExperimentCaptureOwner* owner = nullptr;
    std::filesystem::path loaded;
    std::vector<std::filesystem::path> loadedPaths;
    std::filesystem::path artifactsPath, compilerPath;
    ID3D12Device* observedDevice = nullptr;
    unsigned generations = 0, unloaded = 0;
    bool controlled = false;
    std::string selection;
    HANDLE shutdown = nullptr;
    std::thread controller;
    std::filesystem::path controlDirectory;
    inline static std::atomic<bool> submissionReady = false;
    static bool readyForCapture() { return submissionReady.load(); }
  public:
    void useControl() { controlled = true; }
    void selectNext(uint64_t pipeline, uint64_t target)
    {
        selection = "select-v1 " + std::to_string(GetCurrentProcessId()) + " " + std::to_string(pipeline) +
            " 0 " + std::to_string(target) + " 2 0 6 3 0 2 7 1";
    }
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
        if (!selection.empty()) file << selection << '\n';
        file.close(); require(bool(file), "Module recorder config write");
        const GlassExperimentHost host { sizeof(host), GLASS_EXPERIMENT_ABI, GlassExperimentCapture | GlassExperimentCensus, device };
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
                require(CaptureQueueTest::install(device), "Install actual public submission observer");
                submissionReady.store(true);
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
            require(GlassFg::RegisterExperimentCensus(&census), "Module census registration");
        }
        return output;
    }
    std::filesystem::path replace() { return start(observedDevice, artifactsPath, compilerPath); }
    void requirePendingOldModule()
    {
        require(controlled && generations == 2, "In-flight test needs two controlled generations");
        const auto result = ExperimentRequest(controlDirectory, GetCurrentProcessId(), "status");
        require(result.at("ok") == "1" && result.at("loaded_modules") == "2" &&
                std::stoull(result.at("capture_pending")) > 0 && GetModuleHandleW(loadedPaths.front().c_str()),
                "In-flight captured recording lost its original module");
    }
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
        unsigned indexed = 0, missing = 0, direct = 0, indirect = 0;
        for (const auto& path : loadedPaths)
        {
            const auto output = path.parent_path() / "capture";
            std::ifstream done(output / "draw-census.done");
            const std::string status((std::istreambuf_iterator<char>(done)), {});
            require(status.find("contended=0\n") != std::string::npos &&
                    status.find("overflow=0\n") != std::string::npos, "Census dropped fixture draws");
            std::ifstream csv(output / "draw-census.csv"); std::string line;
            std::getline(csv, line); uint64_t prior = 0; unsigned rows = 0;
            while (std::getline(csv, line))
            {
                ++rows;
                std::istringstream values(line); std::vector<std::string> fields; std::string field;
                while (std::getline(values, field, ',')) fields.push_back(field);
                require(fields.size() == 57, "Census columns incomplete");
                const auto value = [&](unsigned i) { return std::stoull(fields.at(i)); };
                require(value(0) > prior && value(3) && value(4) && value(6) && value(8), "Census order/bindings absent");
                prior = value(0);
                require(value(56) == value(11), "Borrowed object entry copy incomplete");
                require(value(17) == 7 && value(18) == 1 && value(20) == 1 && value(21) && value(38) && value(54),
                        "Census lost actual raster/target resource bindings");
                if (value(1) == GlassCensusIndexed)
                {
                    ++indexed;
                    if (!value(11)) { ++missing; require(!value(2), "Missing packet invented frame"); }
                    else require(value(2) >= 1 && value(2) <= 8, "Mapped census frame absent");
                }
                else
                {
                    require(!value(2) && !value(11), "Non-indexed observation invented engine identity");
                    if (value(1) == GlassCensusInstanced) { ++direct; require(!value(12) && value(13) == 1, "Raw vertex args lost"); }
                    else
                    {
                        require(value(1) == GlassCensusIndirect && !value(12) && !value(13) && value(22) &&
                                value(23) == 1 && value(24), "Indirect upper bound lost or invented draw count");
                        ++indirect;
                    }
                }
            }
            require(std::filesystem::file_size(output / "draw-census.targets.bin") ==
                    rows * 9 * sizeof(GlassExperimentTarget) &&
                    status.find("rows=" + std::to_string(rows) + "\n") != std::string::npos,
                    "Census row/target count mismatch");
        }
        require(indexed == 40 && missing == 8 && direct == 8 && indirect == 8, "Census operation coverage mismatch");
        std::ifstream filter(loadedPaths.back().parent_path() / "capture" / "selection.status");
        const std::string filterStatus((std::istreambuf_iterator<char>(filter)), {});
        require(filterStatus.find("enabled=1\n") != std::string::npos && filterStatus.find("matched=0\n") == std::string::npos &&
                filterStatus.find("rejected=0\n") == std::string::npos, "Targeted module did not filter original draws");
        printf("PASS raw_census=56 indexed=40 missing_packets=8 nonindexed=8 indirect=8 gpu_copies=0 game_objects=0\n");
        if (controlled) require(CaptureQueueTest::submissions() >= 10, "Actual submissions not observed");
    }
    ~ModuleRecorderCheck()
    {
        if (controller.joinable()) { SetEvent(shutdown); controller.join(); }
        if (shutdown) CloseHandle(shutdown);
    }
};
