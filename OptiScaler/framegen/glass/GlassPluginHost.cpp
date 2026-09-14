#include "pch.h"
#include "GlassPluginHost.h"
#include "CyberpunkLayout.h"
#include <Util.h>
#include <windows.h>
#include <cstdio>
#include <cstring>
#include <string>

namespace GlassFg
{
namespace
{
using AttachFn = bool (*)(const GlassPluginApi*);
using DetachFn = void (*)();

HMODULE plugin = nullptr;
AttachFn attach = nullptr;
DetachFn detach = nullptr;
const std::wstring pluginName = L"glass-plugin.dll";

void copyMessage(char* message, unsigned bytes, const std::string& text) noexcept
{
    if (!message || !bytes)
        return;
    const auto count = (std::min)(size_t(bytes - 1), text.size());
    std::memcpy(message, text.data(), count);
    message[count] = 0;
}
} // namespace

bool GlassPluginLoaded() noexcept
{
    return plugin != nullptr;
}

bool LoadGlassPlugin(char* message, unsigned messageBytes) noexcept
{
    if (plugin)
    {
        copyMessage(message, messageBytes, "already loaded");
        return false;
    }
    const auto path = Util::DllPath().parent_path() / L"Glass" / pluginName;
    HMODULE module = LoadLibraryW(path.c_str());
    if (!module)
    {
        copyMessage(message, messageBytes, "LoadLibrary failed error=" + std::to_string(GetLastError()));
        return false;
    }
    const auto attachFn = reinterpret_cast<AttachFn>(GetProcAddress(module, "GlassPluginAttach"));
    const auto detachFn = reinterpret_cast<DetachFn>(GetProcAddress(module, "GlassPluginDetach"));
    GlassPluginApi api;
    api.log = nullptr;
    // Alive for the duration of attach; the plugin must copy it.
    const auto directory = Util::DllPath().parent_path().wstring();
    api.moduleDirectory = directory.c_str();
    api.engineUpdate = nullptr;
    if (const auto* layout = GetCyberpunkLayout(GetModuleHandleW(nullptr)))
        api.engineUpdate = reinterpret_cast<const void*>(
            reinterpret_cast<const std::byte*>(GetModuleHandleW(nullptr)) + layout->functions[CyberpunkLayout::Update]);
    api.trace = [](const char* text) noexcept
    {
        const auto path = Util::DllPath().parent_path() / L"OptiScaler.Glass.log";
        if (FILE* file = _wfopen(path.c_str(), L"a"))
        {
            std::fprintf(file, "GLASS_PLUGIN %s\n", text ? text : "");
            std::fclose(file);
        }
    };
    if (!attachFn || !detachFn || !attachFn(&api))
    {
        copyMessage(message, messageBytes, "attach failed error=" + std::to_string(GetLastError()));
        FreeLibrary(module);
        return false;
    }
    plugin = module;
    attach = attachFn;
    detach = detachFn;
    copyMessage(message, messageBytes, "loaded");
    return true;
}

bool UnloadGlassPlugin(char* message, unsigned messageBytes) noexcept
{
    if (!plugin)
    {
        copyMessage(message, messageBytes, "not loaded");
        return false;
    }
    if (detach)
        detach();
    FreeLibrary(plugin);
    plugin = nullptr;
    attach = nullptr;
    detach = nullptr;
    copyMessage(message, messageBytes, "unloaded");
    return true;
}
} // namespace GlassFg
