#pragma once
#include <atomic>

namespace GlassFg
{
// File-driven live control channel. The game process polls Glass\glass-debug.request
// once per second from the existing health refresh and answers in
// Glass\glass-debug.response. Commands are one per line:
//   status | packed=on|off | rows=N | edge=N | strength=N | reload-shader
// No new thread, no per-draw cost, no restart.
void PollGlassDebugControl() noexcept;
bool TakeShaderReloadRequest() noexcept;
}
