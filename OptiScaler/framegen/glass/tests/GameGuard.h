#pragma once
#include <windows.h>
#include <tlhelp32.h>
#include <cstdio>
#include <cstdlib>

// GPU fixtures must never run while the game is running: creating another
// D3D12 device and dispatching on the same GPU has repeatedly coincided with
// driver resets that kill the game session.
inline bool GlassGameRunning() noexcept
{
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE)
        return false;
    PROCESSENTRY32W entry {};
    entry.dwSize = sizeof(entry);
    bool found = false;
    if (Process32FirstW(snapshot, &entry))
        do
        {
            if (_wcsicmp(entry.szExeFile, L"Cyberpunk2077.exe") == 0)
            {
                found = true;
                break;
            }
        } while (Process32NextW(snapshot, &entry));
    CloseHandle(snapshot);
    return found;
}

inline void GlassRequireGameClosed()
{
    if (GlassGameRunning())
    {
        std::fprintf(stderr, "REFUSED Cyberpunk2077 is running; GPU fixtures must not run during a game session.\n");
        std::exit(3);
    }
}
