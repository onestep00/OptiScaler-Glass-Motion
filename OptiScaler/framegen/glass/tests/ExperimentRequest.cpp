#include "ExperimentClient.h"
#include <iostream>
int wmain(int argc, wchar_t** argv)
{
    try
    {
        if (argc < 4 || argc > 7) throw std::runtime_error("Usage: ExperimentRequest.exe PID request-directory load|disable|status [absolute-DLL] [--response-directory directory]");
        size_t consumed = 0;
        const std::wstring processText = argv[1];
        const auto parsed = std::stoull(processText, &consumed);
        if (!parsed || parsed > MAXDWORD || consumed != processText.size())
            throw std::runtime_error("Invalid process identity");
        const std::wstring wideCommand = argv[3];
        const std::string command = wideCommand == L"load" ? "load" : wideCommand == L"disable" ? "disable" :
                                    wideCommand == L"status" ? "status" : "";
        if (command.empty()) throw std::runtime_error("Unknown experiment command");
        int next = 4;
        std::filesystem::path module, response;
        if (command == "load")
        {
            if (next >= argc) throw std::runtime_error("Load requires an absolute DLL path");
            module = argv[next++];
        }
        if (next < argc)
        {
            if (next + 2 != argc || std::wstring(argv[next]) != L"--response-directory")
                throw std::runtime_error("Unexpected request arguments");
            response = std::filesystem::absolute(argv[next + 1]);
        }
        const auto result = ExperimentRequest(std::filesystem::absolute(argv[2]), DWORD(parsed), command,
                                              module, response);
        for (const auto& [key, value] : result) std::cout << key << '=' << value << '\n';
        return result.at("ok") == "1" ? 0 : 2;
    }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
