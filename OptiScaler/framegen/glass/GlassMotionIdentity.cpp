#include "pch.h"
#include "GlassMotionIdentity.h"
#include "GlassArrayMapping.h"
#include "CyberpunkDraws.h"
#include "CyberpunkObjects.h"
#include "GeometryCommands.h"
#include "GeometryDrawBatch.h"
#include "GeometryPipelineCache.h"
#include "GeometryRasterState.h"
#include "GlassControls.h"
#include "VertexHistoryCache.h"
#include <atomic>

namespace GlassFg
{
namespace
{
std::atomic<std::uint64_t> resolvedCount = 0, rejectedCount = 0, noOwnerCount = 0, noViewCount = 0,
                           noLifetimeCount = 0, noElementIndexCount = 0, noViewStateCount = 0,
                           noViewUnknownCount = 0, noViewDescriptorCount = 0, noElementParentCount = 0,
                           noElementOrderCount = 0, noViewNoRecordCount = 0, noViewNullResourceCount = 0,
                           elementMappedCount = 0, elementUnmappedCount = 0;

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
        bool namedViewRecord = false;
        if (raster && raster->targetsKnown)
        {
            if (raster->depthView)
            {
                namedViewRecord = true;
                if (raster->depthView->resource)
                    view = raster->depthView->resource;
            }
            for (UINT i = 0; i < raster->targetCount && !view; ++i)
            {
                if (!raster->targetViews[i])
                    continue;
                namedViewRecord = true;
                if (raster->targetViews[i]->resource)
                    view = raster->targetViews[i]->resource;
            }
        }
        if (!view)
        {
            if (!raster)
                noViewStateCount.fetch_add(1, std::memory_order_relaxed);
            else if (!raster->targetsKnown)
                noViewUnknownCount.fetch_add(1, std::memory_order_relaxed);
            else
            {
                noViewDescriptorCount.fetch_add(1, std::memory_order_relaxed);
                // The aggregate above hides whether the draw named no view
                // record or named records whose resource field is still zero.
                if (namedViewRecord)
                    noViewNullResourceCount.fetch_add(1, std::memory_order_relaxed);
                else
                    noViewNoRecordCount.fetch_add(1, std::memory_order_relaxed);
            }
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
            // Instance arrays keep the engine's own logic. Only the order the
            // engine itself exposes is used; a grouped element whose original
            // source index is not available stays unresolved and keeps the
            // engine's original motion. The extra per-element mapping a live
            // plugin used to publish was dropped on 2026-09-14: its owner scan
            // cost far more CPU than the correction was worth.
            if (!span.parent)
            {
                // The parent packet identity never resolved, so no element of
                // this array can be indexed.
                reject(noElementIndexCount);
                reject(noElementParentCount);
                return false;
            }
            if (span.orderKind == 0)
            {
                // Parent known, but the engine's own source order for this
                // packet was not verified.
                reject(noElementIndexCount);
                reject(noElementOrderCount);
                return false;
            }
            if (!span.originalIndex(ordinal, sourceIndex))
            {
                reject(noElementIndexCount);
                return false;
            }
            if (span.orderKind == 2)
            {
                // Grouped update. The packet ordinal is a position inside this
                // update, not an object identity: the engine rebuilds the pool
                // from a selected 16-bit source-index list every frame. The
                // grouped hook publishes that list item for each pool ordinal it
                // appended, so the history key follows the object when the
                // selection changes. A miss keeps the ordinal (previous
                // behaviour) and is counted, so a live session shows whether the
                // engine mapping actually reached this proxy and range.
                const auto member = LookupArrayMapping(span.parent.proxy, span.transformIndex + ordinal,
                                                       span.transformIndex, span.count);
                if (member != UINT32_MAX)
                {
                    sourceIndex = member;
                    elementMappedCount.fetch_add(1, std::memory_order_relaxed);
                }
                else
                    elementUnmappedCount.fetch_add(1, std::memory_order_relaxed);
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
    value.noViewState = noViewStateCount.load(std::memory_order_relaxed);
    value.noViewUnknown = noViewUnknownCount.load(std::memory_order_relaxed);
    value.noViewDescriptor = noViewDescriptorCount.load(std::memory_order_relaxed);
    value.noViewNoRecord = noViewNoRecordCount.load(std::memory_order_relaxed);
    value.noViewNullResource = noViewNullResourceCount.load(std::memory_order_relaxed);
    value.noLifetime = noLifetimeCount.load(std::memory_order_relaxed);
    value.noElementIndex = noElementIndexCount.load(std::memory_order_relaxed);
    value.noElementParent = noElementParentCount.load(std::memory_order_relaxed);
    value.noElementOrder = noElementOrderCount.load(std::memory_order_relaxed);
    value.elementMapped = elementMappedCount.load(std::memory_order_relaxed);
    value.elementUnmapped = elementUnmappedCount.load(std::memory_order_relaxed);
    return value;
}
} // namespace GlassFg
