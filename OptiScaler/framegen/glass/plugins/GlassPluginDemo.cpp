// Demonstration plugin for the live hook boundary. It attaches to an engine
// function the resident module already validated and writes a heartbeat, so a
// new DLL can be loaded into a running game without a restart.
#include <windows.h>
#include <detours.h>
#include <cstdio>
#include <cstring>
#include <atomic>

namespace
{
struct GlassPluginApi
{
    unsigned version = 1;
    FILE* log = nullptr;
    const wchar_t* moduleDirectory = nullptr;
    const void* engineUpdate = nullptr;
    void (*trace)(const char* text) noexcept = nullptr;
};

using UpdateFn = void (*)(void*);
UpdateFn originalUpdate = nullptr;
const GlassPluginApi* host = nullptr;
std::atomic<unsigned> calls { 0 };

void hookedUpdate(void* self)
{
    const auto count = calls.fetch_add(1, std::memory_order_relaxed) + 1;
    if (host && host->trace && count % 120 == 0)
    {
        char text[96] {};
        std::snprintf(text, sizeof(text), "demo heartbeat calls=%u self=%p", count, self);
        host->trace(text);
    }
    if (originalUpdate)
        originalUpdate(self);
}
} // namespace

extern "C" __declspec(dllexport) bool GlassPluginAttach(const GlassPluginApi* api)
{
    if (!api || api->version != 1 || !api->engineUpdate || !api->trace)
        return false;
    host = api;
    originalUpdate = reinterpret_cast<UpdateFn>(const_cast<void*>(api->engineUpdate));
    void* replacement = reinterpret_cast<void*>(&hookedUpdate);
    // The resident module uses Detours for the same reason; the trampoline keeps
    // the original body reachable without a byte-level patcher.
    return DetourAttach(reinterpret_cast<PVOID*>(&originalUpdate), replacement) == NO_ERROR && originalUpdate;
}

extern "C" __declspec(dllexport) void GlassPluginDetach()
{
    if (originalUpdate)
        DetourDetach(reinterpret_cast<PVOID*>(&originalUpdate), reinterpret_cast<PVOID>(&hookedUpdate));
    originalUpdate = nullptr;
    host = nullptr;
}

BOOL WINAPI DllMain(HINSTANCE, DWORD, LPVOID)
{
    return TRUE;
}
