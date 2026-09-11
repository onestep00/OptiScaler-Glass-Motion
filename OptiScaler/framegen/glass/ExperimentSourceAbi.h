#pragma once
#include <cstdint>

// Synchronous scalar lookup for replaceable diagnostics in the same process.
// The caller owns the live producer argument. This is not a lifetime lease,
// previous-frame guarantee, transform pointer, or permission to generate MV.
struct GlassExperimentSourceOwner
{
    std::uint32_t size = sizeof(GlassExperimentSourceOwner), version = 1;
    std::uint64_t node = 0, buffer = 0;
    std::uint32_t first = 0, count = 0, generation = 0, reserved = 0;
};
using GlassExperimentSourceQuery = std::int32_t (*)(std::uint64_t proxy, std::uint64_t mesh,
                                                  std::uint32_t originalCount,
                                                  GlassExperimentSourceOwner* output);
