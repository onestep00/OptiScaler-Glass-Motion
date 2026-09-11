#include "ExperimentClient.h"
#include <iostream>
int wmain(int argc, wchar_t** argv)
{
    try
    {
        if (argc < 4 || argc > 5) throw std::runtime_error("Usage: ExperimentRequest.exe PID Glass-directory load|disable|status [absolute-DLL]");
        size_t consumed = 0;
        const std::wstring processText = argv[1];
        const auto parsed = std::stoull(processText, &consumed);
        if (!parsed || parsed > MAXDWORD || consumed != processText.size())
            throw std::runtime_error("Invalid process identity");
        const std::wstring wideCommand = argv[3];
        const std::string command = wideCommand == L"load" ? "load" : wideCommand == L"disable" ? "disable" :
                                    wideCommand == L"status" ? "status" : "";
        if (command.empty()) throw std::runtime_error("Unknown experiment command");
        const auto result = ExperimentRequest(std::filesystem::absolute(argv[2]), DWORD(parsed), command,
                                              argc == 5 ? std::filesystem::path(argv[4]) : std::filesystem::path {});
        for (const auto& [key, value] : result) std::cout << key << '=' << value << '\n';
        return result.at("ok") == "1" ? 0 : 2;
    }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
