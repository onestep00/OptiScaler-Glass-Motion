#pragma once
#include "GeometryObjectRegistry.h"
#include <windows.h>
#include <memory>

namespace GlassFg
{
// Startup only. This observes the audited mesh registry and transform function;
// it does not enumerate object/material names or claim particle-route coverage.
// Public D3D12 shader observation and engine draw-to-mesh matching are separate.
bool InitializeCyberpunkObjects(HMODULE executable) noexcept;
std::shared_ptr<GeometryObjectRegistry> GetCyberpunkObjects() noexcept;
struct CyberpunkObjectStatus
{
    bool active = false;
    std::uint64_t snapshotsRejected = 0;
    GeometryObjectStats registry;
};
CyberpunkObjectStatus GetCyberpunkObjectStatus();
} // namespace GlassFg
