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
        for (std::uint32_t i = 0; i < instances; ++i)
            if (!mapping[mappingBase + i].valid(historyCapacity, capturePixels))
                return false;
        return true;
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
