#pragma once
#include "ExperimentCaptureOwner.h"
#include <fstream>
#include <string>

namespace GlassFg
{
// Process-resident control thread. Request files are read only after an event.
// Clients serialize a request/response transaction with the named Client mutex.
// Rendering never loads DLLs, reads files, joins workers or waits for this loop.
class ExperimentControl
{
    struct Event
    {
        HANDLE value = nullptr;
        explicit Event(const std::wstring& name = {})
        {
            value = CreateEventW(nullptr, FALSE, FALSE, name.empty() ? nullptr : name.c_str());
            const auto error = GetLastError();
            if (!value || (!name.empty() && error == ERROR_ALREADY_EXISTS))
            {
                if (value) CloseHandle(value);
                throw std::runtime_error("Experiment control event unavailable or already owned");
            }
        }
        ~Event() { if (value) CloseHandle(value); }
        Event(const Event&) = delete;
    };
    std::filesystem::path directory;
    ExperimentRuntime runtime;
    Event requested, responded, changed;
    ExperimentCaptureOwner owner;
    GlassExperimentHost host;
    uint64_t unloaded = 0, commands = 0;
    bool (*submissionReady)() = nullptr;

    void collect()
    {
        owner.collect(); unloaded += runtime.collect();
    }
    void response(const std::string& id, const std::string& error)
    {
        const auto module = runtime.status();
        const auto capture = owner.status();
        std::ofstream file(directory / "experiment.control.response", std::ios::binary);
        file << "request=" << id << "\nok=" << error.empty() << "\nerror=" << error
             << "\nprocess=" << GetCurrentProcessId() << "\nactive_generation=" << module.active
             << "\nloaded_modules=" << module.loaded << "\nunloaded_modules=" << unloaded
             << "\ncapture_pending=" << capture.pending << "\ncapture_recorded=" << capture.recorded
             << "\ncapture_retired=" << capture.retired << "\naccepting=" << capture.accepting
             << "\nsubmission_observer_ready=" << (!submissionReady || submissionReady())
             << "\ncommands=" << commands << "\nfg_connected=0\n";
        file.close();
        if (!file) throw std::runtime_error("Experiment response write failed");
        SetEvent(responded.value);
    }
    void request() noexcept
    {
        std::string id, command, path, error;
        try
        {
            const auto input = directory / "experiment.control.request";
            if (std::filesystem::file_size(input) > 262144) throw std::runtime_error("Oversized experiment request");
            std::ifstream file(input, std::ios::binary);
            if (!std::getline(file, id) || !std::getline(file, command) || !std::getline(file, path))
                throw std::runtime_error("Incomplete experiment request");
            for (auto* line : { &id, &command, &path })
                if (!line->empty() && line->back() == '\r') line->pop_back();
            if (id.empty() || id.size() > 64 || id.find_first_not_of("0123456789abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ-_") != std::string::npos)
            { id.clear(); throw std::runtime_error("Invalid request identity"); }
            if (path.find('\0') != std::string::npos || file.peek() != std::char_traits<char>::eof())
                throw std::runtime_error("Unexpected request data");
            ++commands;
            if (command == "load")
            {
                if (submissionReady && !submissionReady())
                    throw std::runtime_error("Native GPU submission observer is not ready; capture not started");
                runtime.replace(std::filesystem::path(std::u8string(path.begin(), path.end())), host);
                owner.resume();
            }
            else if (command == "disable" && path.empty()) { owner.stop(); runtime.disable(); }
            else if (command != "status" || !path.empty()) throw std::runtime_error("Unknown experiment command");
            collect();
        }
        catch (const std::exception& failure) { error = failure.what(); }
        try { response(id, error); }
        catch (...) { SetEvent(responded.value); } // Client detects missing/mismatched response.
    }
  public:
    static std::wstring prefix()
    { return L"Local\\OptiScaler.Glass.Experiment." + std::to_wstring(GetCurrentProcessId()); }
    ExperimentControl(ID3D12Device* device, const std::filesystem::path& output, bool (*ready)() = nullptr)
        : directory(output), requested(prefix() + L".Request"), responded(prefix() + L".Response"),
          owner(runtime, device, changed.value),
          host { sizeof(host), GLASS_EXPERIMENT_ABI, GlassExperimentCapture, device }, submissionReady(ready)
    {
        if (!directory.is_absolute() || !std::filesystem::is_directory(directory))
            throw std::runtime_error("Invalid experiment control directory");
        owner.stop();
        if (!RegisterGeometryDrawCapture(&owner)) throw std::runtime_error("Another capture owner is already registered");
    }
    // Test shutdown leaves this registered process-resident object alive. No
    // module/owner destructor runs while the host can still forward callbacks.
    void run(HANDLE testShutdown = nullptr) noexcept
    {
        try
        {
            const HANDLE events[] { requested.value, changed.value, testShutdown };
            response("ready", {});
            for (;;)
            {
                const auto module = runtime.status();
                // A callback can temporarily defer collect even with no reserved
                // job. Keep draining a disabled module until its idle frame lease
                // is released, rather than sleeping forever after that race.
                const bool draining = owner.status().pending || (!module.active && module.loaded);
                const auto result = WaitForMultipleObjects(testShutdown ? 3 : 2, events, FALSE,
                                                           draining ? 100 : INFINITE);
                if (result == WAIT_OBJECT_0) request();
                else if (testShutdown && result == WAIT_OBJECT_0 + 2) break;
                else if (result != WAIT_OBJECT_0 + 1 && result != WAIT_TIMEOUT) break;
                collect();
            }
        }
        catch (...) {}
        owner.stop(); runtime.disable(); collect();
    }
};
} // namespace GlassFg
