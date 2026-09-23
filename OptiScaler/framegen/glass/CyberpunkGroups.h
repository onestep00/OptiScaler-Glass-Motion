#pragma once
#include <windows.h>
#include <cstdint>

namespace GlassFg
{
// Grouped instance-array element identity, read from the engine's own grouped
// update. The update walks a 16-bit source-index list and copies each element's
// 48-byte transform into the global pool; the append of every element passes
// the source pointer, so the element's original array index is recovered
// without estimation. The published table turns a pool ordinal back into that
// source index, which is what the motion history has to be keyed by.
struct GroupedArrayStats
{
    std::uint64_t groupedCalls = 0, staged = 0, published = 0, aborted = 0;
    std::uint64_t appendCalls = 0, appendInRange = 0, appendOutside = 0, appendDropped = 0;
    std::uint64_t sampleCalls = 0, sampleCycles = 0, sampleMaxCycles = 0;
};

GroupedArrayStats ReadGroupedArrayStats() noexcept;
bool InitializeCyberpunkGroups(HMODULE executable) noexcept;
} // namespace GlassFg
