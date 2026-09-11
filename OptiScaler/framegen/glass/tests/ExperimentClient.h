#pragma once
#include <windows.h>
#include <atomic>
#include <filesystem>
#include <fstream>
#include <map>
#include <stdexcept>
#include <string>

// Diagnostic client only. The resident host has no dependency on this helper.
inline std::map<std::string, std::string> ExperimentRequest(const std::filesystem::path& directory, DWORD process,
                                                          const std::string& command,
                                                          const std::filesystem::path& module = {})
{
    struct Handle
    {
        HANDLE value = nullptr;
        ~Handle() { if (value) CloseHandle(value); }
    };
    const auto prefix = L"Local\\OptiScaler.Glass.Experiment." + std::to_wstring(process);
    Handle mutex { CreateMutexW(nullptr, FALSE, (prefix + L".Client").c_str()) };
    if (!mutex.value) throw std::runtime_error("Experiment client mutex unavailable");
    const auto locked = WaitForSingleObject(mutex.value, 10000);
    if (locked != WAIT_OBJECT_0 && locked != WAIT_ABANDONED) throw std::runtime_error("Experiment client busy");
    struct Unlock { HANDLE value; ~Unlock() { ReleaseMutex(value); } } unlock { mutex.value };
    Handle request { OpenEventW(EVENT_MODIFY_STATE, FALSE, (prefix + L".Request").c_str()) };
    Handle response { OpenEventW(SYNCHRONIZE | EVENT_MODIFY_STATE, FALSE, (prefix + L".Response").c_str()) };
    if (!request.value || !response.value) throw std::runtime_error("Experiment host is not ready");
    static std::atomic<unsigned> sequence = 0;
    const auto id = std::to_string(GetCurrentProcessId()) + "-" + std::to_string(GetTickCount64()) + "-" +
                    std::to_string(++sequence);
    const auto utf8 = module.u8string();
    if (command.find_first_of("\r\n") != std::string::npos || utf8.find_first_of(u8"\r\n") != std::u8string::npos)
        throw std::runtime_error("Invalid request line");
    std::ofstream file(directory / "experiment.control.request", std::ios::binary);
    file << id << '\n' << command << '\n';
    file.write(reinterpret_cast<const char*>(utf8.data()), std::streamsize(utf8.size())); file << '\n';
    file.close();
    if (!file) throw std::runtime_error("Experiment request write failed");
    ResetEvent(response.value);
    if (!SetEvent(request.value)) throw std::runtime_error("Experiment request notification failed");
    const auto deadline = GetTickCount64() + 30000;
    for (;;)
    {
        const auto now = GetTickCount64();
        if (now >= deadline) break;
        const auto remaining = deadline - now;
        if (WaitForSingleObject(response.value, DWORD(remaining)) != WAIT_OBJECT_0) break;
        std::ifstream input(directory / "experiment.control.response", std::ios::binary);
        std::map<std::string, std::string> result;
        for (std::string line; std::getline(input, line);)
        {
            const auto separator = line.find('=');
            if (separator != std::string::npos) result[line.substr(0, separator)] = line.substr(separator + 1);
        }
        if (result["request"] == id && result["process"] == std::to_string(process)) return result;
    }
    throw std::runtime_error("Experiment response timeout; completion is unknown");
}
