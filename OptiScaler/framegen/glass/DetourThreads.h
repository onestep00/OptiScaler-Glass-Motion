#pragma once
#include <windows.h>
#include <tlhelp32.h>
#include <detours/detours.h>
#include <array>

namespace GlassFg
{
// Gather handles before a transaction suspends any thread. Enlistment failure
// must abort the whole transaction; partial API observation is not admission.
class DetourThreads
{
    std::array<HANDLE, 512> handles {};
    unsigned count = 0;

  public:
    ~DetourThreads()
    {
        for (unsigned i = 0; i < count; ++i)
            CloseHandle(handles[i]);
    }
    bool gather()
    {
        HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
        if (snapshot == INVALID_HANDLE_VALUE)
            return false;
        THREADENTRY32 entry {};
        entry.dwSize = sizeof(entry);
        bool okay = Thread32First(snapshot, &entry) != FALSE;
        if (okay)
            do
            {
                if (entry.th32OwnerProcessID != GetCurrentProcessId() || entry.th32ThreadID == GetCurrentThreadId())
                    continue;
                if (count == handles.size())
                {
                    okay = false;
                    break;
                }
                auto handle = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_SET_CONTEXT |
                                             THREAD_QUERY_INFORMATION,
                                         FALSE, entry.th32ThreadID);
                if (handle)
                    handles[count++] = handle;
                else if (GetLastError() != ERROR_INVALID_PARAMETER)
                {
                    okay = false;
                    break;
                }
            } while (Thread32Next(snapshot, &entry));
        CloseHandle(snapshot);
        return okay;
    }
    bool enlist()
    {
        if (DetourUpdateThread(GetCurrentThread()) != NO_ERROR)
            return false;
        for (unsigned i = 0; i < count; ++i)
        {
            DWORD code = 0;
            if (!GetExitCodeThread(handles[i], &code) ||
                (code == STILL_ACTIVE && DetourUpdateThread(handles[i]) != NO_ERROR))
                return false;
        }
        return true;
    }
};
} // namespace GlassFg
