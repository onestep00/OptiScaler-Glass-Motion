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

    bool active() const { return enabled && strength > 0; }
    float coverage() const { return std::min(strength, 100u) / 100.f; }
    uint32_t packed() const
    {
        return (std::min(strength, 100u) << 1) | (enabled ? 1u : 0u) | (measureGpuTime ? 256u : 0u);
    }
    static Controls unpack(uint32_t value)
    {
        return { (value & 1u) != 0, std::min((value >> 1) & 127u, 100u), (value & 256u) != 0 };
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
