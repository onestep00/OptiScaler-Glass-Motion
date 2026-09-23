#pragma once
#include <cstdint>

namespace GlassFg
{
// Native motion declarations. The engine's shader-cache provider metadata
// getter returns, for the declared material keys, an immutable copy of the
// native declaration with MatMod_MotionMatrix added at row 24, so the engine's
// own evaluator fills rows 24..26 of the material constant buffer that the
// grafted vertex shaders read (EngineMotionSupply.md "Startup consumption").
// The copy is returned only inside the native vertex-stage resolution of a
// declared VS/PS pair; every other call, and every record that differs from its
// plan, keeps the engine's own result.
//
// Data beside the module: Glass/motion-declarations.bin ("GMDPLAN1": u32 plan
// count, u32 name count, plans, names) and Glass/motion-shader-pairs.bin
// ("GMSPAIR1": u32 pair count, u32 reserved 0, pairs sorted by VS then PS).
//
// Layouts the engine compiled before the hook keep their declaration, so it is
// installed at process attach, before the provider builds any layout.
//
// Coexistence: this replaces the RED4ext GlassMotion plugin
// (red4ext/plugins/GlassMotion), which detours the same two engine entries.
// Both must not be active in one process: two detours on one entry would both
// rewrite the same declaration. The signature profiles hash the entry bytes, so
// whichever installs second normally finds a patched entry and stops with
// native_layout_rejected; remove the plugin instead of relying on that.
struct NativeMotionDeclarationStats
{
    // Provider results seen, augmented declarations returned, declared keys
    // whose native record did not match its plan, native stage resolutions and
    // vertex stages of a declared VS/PS pair.
    std::uint64_t seen = 0, matched = 0, rejected = 0, stageSeen = 0, stageSelected = 0;
    // not_started until installation ran, then the step that stopped it:
    // not_cyberpunk, native_layout_rejected (provider or stage profile not
    // unique), declaration_file_missing (a data file absent or unreadable),
    // declaration_profile_rejected (malformed data or a pair outside the plans),
    // hook_attach_failed (module pin, thread enlistment or Detours) or installed.
    const char* status = "not_started";
};

// Call once from DLL_PROCESS_ATTACH (loader lock): no logging, no thread
// creation, no game object access. Later calls do nothing. Silent: a failure
// leaves the engine untouched and is reported only through the status token.
void InstallNativeMotionDeclarations() noexcept;
// Fills stats; true when both hooks are installed.
bool TryNativeMotionDeclarationCounters(NativeMotionDeclarationStats& stats) noexcept;
} // namespace GlassFg
