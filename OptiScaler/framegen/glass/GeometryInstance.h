#pragma once
#include <cstdint>
#include <span>

namespace GlassFg
{
enum class GeometryLayout
{
    Contiguous,
    PerInstance
};

// Immutable for the lifetime of a recorded draw. The adapter resolves these
// records from live engine identity; neither draw order nor this index is an ID.
// One object can keep its history allocation while its place in a batch changes.
struct GeometryInstance
{
    std::uint32_t historyBase, vertices, vertexOrigin, generation;
    std::uint32_t left, top, width, height;
    std::uint32_t pixelBase, stride, pixelCapacity, statusIndex;
    std::uint32_t reserved[4] {};

    // An exact zero record preserves an unknown instance's place in the draw
    // without authorizing any history/capture/status address.
    bool inactive() const
    {
        return !(historyBase | vertices | vertexOrigin | generation | left | top | width | height | pixelBase | stride |
                 pixelCapacity | statusIndex | reserved[0] | reserved[1] | reserved[2] | reserved[3]);
    }

    bool valid(std::uint64_t historyCapacity, std::uint64_t captureCapacity) const
    {
        return generation && vertices && historyCapacity <= UINT32_MAX / 32 &&
               std::uint64_t(historyBase) + vertices <= historyCapacity &&
               std::uint64_t(vertexOrigin) + vertices <= (std::uint64_t(1) << 32) && width && height &&
               stride >= width && pixelCapacity && pixelCapacity <= UINT32_MAX / 32 &&
               pixelCapacity <= captureCapacity && std::uint64_t(left) + width <= 32768 &&
               std::uint64_t(top) + height <= 32768 && statusIndex < pixelBase &&
               std::uint64_t(pixelBase) + std::uint64_t(height - 1) * stride + width <= pixelCapacity;
    }

    // Diagnostic bit coverage uses bit addresses for pixels and word addresses
    // for status. It deliberately cannot authorize vertex-history access.
    bool validCoverage(std::uint64_t words) const
    {
        return generation && !historyBase && !vertices && !vertexOrigin && width && height && stride >= width &&
               words <= UINT32_MAX / 4 && pixelCapacity && std::uint64_t(pixelCapacity) <= words * 32 &&
               !(pixelBase & 31) && std::uint64_t(statusIndex) * 32 < pixelBase &&
               std::uint64_t(pixelBase) + std::uint64_t(height - 1) * stride + width <= pixelCapacity &&
               std::uint64_t(left) + width <= 32768 && std::uint64_t(top) + height <= 32768 &&
               !(reserved[0] | reserved[1] | reserved[2] | reserved[3]);
    }
};
static_assert(sizeof(GeometryInstance) == 64);

struct InstanceHistoryConstants
{
    std::uint32_t mappingBase, mappingCapacity, historyCapacity, instanceOrigin;
    std::uint32_t instances, reserved, frame, previousFrame;
    bool valid(std::span<const GeometryInstance> mapping, std::uint64_t historyVertices,
               std::uint64_t capturePixels) const
    {
        if (!instances || !frame || previousFrame == frame || !historyCapacity || historyCapacity > historyVertices ||
            mappingCapacity > mapping.size() || mappingCapacity > UINT32_MAX / sizeof(GeometryInstance) ||
            std::uint64_t(mappingBase) + instances > mappingCapacity ||
            std::uint64_t(instanceOrigin) + instances > (std::uint64_t(1) << 32))
            return false;
        bool active = false;
        for (std::uint32_t i = 0; i < instances; ++i)
        {
            const auto& entry = mapping[mappingBase + i];
            if (entry.inactive())
                continue;
            if (!entry.valid(historyCapacity, capturePixels))
                return false;
            active = true;
        }
        return active;
    }
};
static_assert(sizeof(InstanceHistoryConstants) == 32);

// One 32-byte status record precedes each object's owned pixels. Clear statuses
// once before that frame's draws. Atomic OR is necessary: ROV pixel ordering
// alone does not order different screen pixels writing a shared object flag.
enum GeometryCaptureStatus : std::uint32_t
{
    GeometryEscapedBounds = 1,
    GeometryMissingHistory = 2,
    GeometryNonfiniteMotion = 4
};
} // namespace GlassFg
