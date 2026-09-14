#pragma once
#include <algorithm>
#include <cstdint>

namespace GlassFg
{
struct Controls
{
    bool enabled = false;
    unsigned strength = 100;
    bool measureGpuTime = true;
    unsigned edgeWidth = 2;
    // Safety staging for the full-screen packed object-motion dispatch.
    bool packedDispatch = true;
    unsigned packedRows = 240;
    // Isolation staging: run the packed dispatch without swapping the FG
    // inputs, so a driver reset can be attributed to the new GPU work or to
    // the NGX input replacement instead of both at once.
    bool packedSubstitute = false;
    // Diagnostic stage: per-step trace lines for attribution. Costs one
    // fprintf/fflush per step and is off by default.
    bool trace = false;

    bool active() const { return enabled && strength > 0; }
    float coverage() const { return std::min(strength, 100u) / 100.f; }
    uint32_t packed() const
    {
        return (std::min(strength, 100u) << 1) | (enabled ? 1u : 0u) | (measureGpuTime ? 256u : 0u) |
               (std::clamp(edgeWidth, 1u, 4u) << 9) | (packedDispatch ? (1u << 12) : 0u) |
               ((std::min)(packedRows, 0xffffu) << 13) | (packedSubstitute ? (1u << 29) : 0u) |
               (trace ? (1u << 30) : 0u);
    }
    static Controls unpack(uint32_t value)
    {
        const auto edge = (value >> 9) & 7u;
        return { (value & 1u) != 0, std::min((value >> 1) & 127u, 100u), (value & 256u) != 0,
                 std::clamp(edge ? edge : 2u, 1u, 4u), (value & (1u << 12)) != 0,
                 (value >> 13) & 0xffffu, (value & (1u << 29)) != 0, (value & (1u << 30)) != 0 };
    }
};

// UI and the native host share one atomic snapshot. No INI reads per FG call.
Controls ReadControls();
void WriteControls(Controls value);
void RenderSettings();
void PublishGpuMilliseconds(double milliseconds);
enum class RuntimeStatus : unsigned { Waiting, Correcting, Unavailable, Retiring, Stopped };
void PublishRuntimeStatus(RuntimeStatus status);
} // namespace GlassFg
