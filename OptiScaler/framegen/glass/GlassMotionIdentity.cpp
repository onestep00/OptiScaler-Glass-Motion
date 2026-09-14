#include "pch.h"
#include "GlassMotionIdentity.h"
#include "CyberpunkDraws.h"
#include "CyberpunkObjects.h"
#include "GeometryCommands.h"
#include "GeometryDrawBatch.h"
#include "GeometryPipelineCache.h"
#include "GeometryRasterState.h"
#include "GlassArrayMapping.h"
#include "GlassControls.h"
#include "VertexHistoryCache.h"
#include <atomic>

namespace GlassFg
{
namespace
{
std::atomic<std::uint64_t> resolvedCount = 0, rejectedCount = 0, noOwnerCount = 0, noViewCount = 0,
                           noLifetimeCount = 0, noElementIndexCount = 0;

// Deterministic mesh-topology identity. The shape has no topology id of its own
// and VertexHistoryKey requires a nonzero topology value.
std::uint64_t topologyHash(const CyberpunkMeshShape& shape) noexcept
{
    std::uint64_t h = 0x9e3779b97f4a7c15ull;
    const auto mix = [&h](std::uint64_t value)
    { h ^= value + 0x9e3779b97f4a7c15ull + (h << 6) + (h >> 2); };
    mix(shape.vertexBuffer);
    mix(shape.indexBuffer);
    mix(shape.vertices);
    mix(shape.indices);
    mix(shape.indexOffset);
    mix((std::uint64_t(shape.indexType) << 8) | shape.vertexFactory);
    mix(shape.streams);
    for (const auto offset : shape.streamOffsets)
        mix(offset);
    return h ? h : 1;
}

// Diagnostics only: relaxed atomics, no allocation and no lock.
void reject(std::atomic<std::uint64_t>& reason) noexcept
{
    rejectedCount.fetch_add(1, std::memory_order_relaxed);
    reason.fetch_add(1, std::memory_order_relaxed);
}

bool resolveIdentity(const void*, ID3D12GraphicsCommandList* command, const GeometryDrawView& draw,
                     const CyberpunkMeshShape& shape, const GeometryPipelineEntry& pipeline,
                     std::uint32_t spanIndex, std::uint32_t ordinal, VertexHistoryKey& key) noexcept
{
    key = {};
    try
    {
        if (!command || !shape || !pipeline.identity || spanIndex >= draw.objects.size())
        {
            reject(rejectedCount);
            return false;
        }
        const auto& span = draw.objects[spanIndex];
        if (!span.count || ordinal >= span.count)
        {
            reject(rejectedCount);
            return false;
        }
        const auto& owner = span.parent ? span.parent : span.identity;
        if (!owner)
        {
            reject(noOwnerCount);
            return false;
        }
        // The command's tracked raster state already carries resolved view
        // records, so this is a pointer read. Depth is preferred; a colour target
        // is used only when the draw has no depth binding.
        const auto* raster = ReadGeometryRasterState(command);
        std::uint64_t view = 0;
        if (raster && raster->targetsKnown)
        {
            if (raster->depthView && raster->depthView->resource)
                view = raster->depthView->resource;
            else
                for (UINT i = 0; i < raster->targetCount; ++i)
                    if (raster->targetViews[i] && raster->targetViews[i]->resource)
                    { view = raster->targetViews[i]->resource; break; }
        }
        if (!view)
        {
            reject(noViewCount);
            return false;
        }
        // The registry is process-resident. Keep one reference instead of copying
        // a shared_ptr for every element of every draw.
        static const auto registry = GetCyberpunkObjects();
        if (!registry)
        {
            reject(noLifetimeCount);
            return false;
        }
        const auto lifetime = registry->lifetime(owner.proxy, owner.slot);
        if (!lifetime)
        {
            reject(noLifetimeCount);
            return false;
        }
        std::uint32_t sourceIndex = 0;
        if (span.count != 1)
        {
            // Element identity uses the original source index; a missing or
            // unverified original order must not be guessed from the ordinal.
            const bool linear = span.parent && span.originalOrder && span.originalIndex(ordinal, sourceIndex);
            if (!linear)
            {
                // Grouped arrays: the live plugin published the engine's own
                // element -> source-index list while the draw order was built.
                const auto mapped = span.parent && ReadControls().arrayMapping?
                                    LookupArrayMapping(span.parent.proxy, span.transformIndex + ordinal) : UINT32_MAX;
                if (mapped == UINT32_MAX)
                {
                    reject(noElementIndexCount);
                    return false;
                }
                sourceIndex = mapped;
            }
        }
        key.object = {owner.proxy, owner.mesh, owner.slot, lifetime};
        key.view = view;
        key.pipeline = pipeline.identity;
        key.topology = topologyHash(shape);
        key.chunk = draw.chunk;
        key.vertexFactory = shape.vertexFactory;
        key.arrayGeneration = span.count != 1 ? lifetime : 0;
        key.sourceIndex = sourceIndex;
        if (!key)
        {
            reject(rejectedCount);
            return false;
        }
        resolvedCount.fetch_add(1, std::memory_order_relaxed);
        return true;
    }
    catch (...)
    {
        key = {};
        return false;
    }
}
} // namespace

PackedMotionIdentityProvider MakeGlassMotionIdentityProvider() noexcept
{
    return {nullptr, &resolveIdentity};
}

GlassMotionIdentityStats ReadGlassMotionIdentityStats() noexcept
{
    GlassMotionIdentityStats value;
    value.resolved = resolvedCount.load(std::memory_order_relaxed);
    value.rejected = rejectedCount.load(std::memory_order_relaxed);
    value.noOwner = noOwnerCount.load(std::memory_order_relaxed);
    value.noView = noViewCount.load(std::memory_order_relaxed);
    value.noLifetime = noLifetimeCount.load(std::memory_order_relaxed);
    value.noElementIndex = noElementIndexCount.load(std::memory_order_relaxed);
    return value;
}
} // namespace GlassFg
