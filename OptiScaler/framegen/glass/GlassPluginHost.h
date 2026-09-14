#pragma once
#include <cstdio>
#include "GlassArrayMapping.h"

namespace GlassFg
{
// Live hook boundary. The resident module may load, unload and reload an
// auxiliary DLL at runtime; that DLL installs its own hooks in Attach and
// removes them in Detach. No process restart is required for plugin code.
//
// Plugin ABI (v1), exported by the plugin:
//   extern "C" __declspec(dllexport) bool GlassPluginAttach(const GlassPluginApi*);
//   extern "C" __declspec(dllexport) void GlassPluginDetach();
struct GlassPluginApi
{
    unsigned version = 1;
    FILE* log = nullptr;
    const wchar_t* moduleDirectory = nullptr;
    // Engine functions the resident module already resolved. NULL when the
    // layout was not validated in this process.
    const void* engineUpdate = nullptr;
    // Grouped-array element mapping sink (GlassArrayMapping). NULL is allowed.
    void (*publishArrayMapping)(const GlassArrayMappingEntry*) noexcept = nullptr;
    // Called by the plugin for its own trace lines; never NULL.
    void (*trace)(const char* text) noexcept = nullptr;
};

// Handles plugin=load|unload|reload requests. Called from the live poll.
bool LoadGlassPlugin(char* message, unsigned messageBytes) noexcept;
bool UnloadGlassPlugin(char* message, unsigned messageBytes) noexcept;
bool GlassPluginLoaded() noexcept;
} // namespace GlassFg
