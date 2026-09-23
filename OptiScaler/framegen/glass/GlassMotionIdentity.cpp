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
// Diagnostic counters. resolve counts into the draw's scratch and flush adds
// the totals once per draw, so the element loop executes no locked instruction
// for them. A locked instruction also drains the write-combined upload heap
// the capture fills.
enum Counter : unsigned
{
    Resolved,
    Rejected,
    NoOwner,
    NoView,
    NoLifetime,
    NoElementIndex,
    NoViewState,
    NoViewUnknown,
    NoViewDescriptor,
    NoElementParent,
    NoElementOrder,
    NoViewNoRecord,
    NoViewNullResource,
    ElementMapped,
    ElementUnmapped,
    CounterCount
};
static_assert(CounterCount <= std::tuple_size_v<decltype(PackedMotionIdentityScratch::counts)>);
std::atomic<std::uint64_t> counters[CounterCount] {};

// The draw's view, established by the first element that needs it. The
// command's raster state belongs to this recording thread and its view
// records are immutable, so the answer is the same for every element.
enum ViewState : std::uint32_t
{
    ViewUnset = 0,
    ViewReady,
    ViewNoState,
    ViewUnknown,
    ViewNoRecord,
    ViewNullResource
};

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

// Diagnostics only: counted in the draw's scratch, published by flush.
void reject(PackedMotionIdentityScratch& scratch, Counter reason) noexcept
{
    ++scratch.counts[Rejected];
    ++scratch.counts[reason];
}

// The command's tracked raster state already carries resolved view records, so
// this is a pointer read. Depth is preferred; a colour target is used only when
// the draw has no depth binding.
ViewState resolveView(ID3D12GraphicsCommandList* command, std::uint64_t& view) noexcept
{
    view = 0;
    const auto* raster = ReadGeometryRasterState(command);
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
    if (view)
        return ViewReady;
    if (!raster)
        return ViewNoState;
    if (!raster->targetsKnown)
        return ViewUnknown;
    // The targets are known but no view record gave a resource: either the
    // draw named no view record, or its records' resource field is still zero.
    return namedViewRecord ? ViewNullResource : ViewNoRecord;
}

bool resolveIdentity(const void*, PackedMotionIdentityScratch& scratch, ID3D12GraphicsCommandList* command,
                     const GeometryDrawView& draw, const CyberpunkMeshShape& shape,
                     const GeometryPipelineEntry& pipeline, std::uint32_t spanIndex, std::uint32_t ordinal,
                     VertexHistoryKey& key) noexcept
{
    key = {};
    try
    {
        if (!command || !shape || !pipeline.identity || spanIndex >= draw.objects.size())
        {
            reject(scratch, Rejected);
            return false;
        }
        const auto& span = draw.objects[spanIndex];
        if (!span.count || ordinal >= span.count)
        {
            reject(scratch, Rejected);
            return false;
        }
        const auto& owner = span.parent ? span.parent : span.identity;
        if (!owner)
        {
            reject(scratch, NoOwner);
            return false;
        }
        if (scratch.viewState == ViewUnset)
            scratch.viewState = resolveView(command, scratch.view);
        if (scratch.viewState != ViewReady)
        {
            switch (scratch.viewState)
            {
            case ViewNoState:
                ++scratch.counts[NoViewState];
                break;
            case ViewUnknown:
                ++scratch.counts[NoViewUnknown];
                break;
            default:
                // The aggregate hides whether the draw named no view record or
                // named records whose resource field is still zero.
                ++scratch.counts[NoViewDescriptor];
                ++scratch.counts[scratch.viewState == ViewNullResource ? NoViewNullResource : NoViewNoRecord];
                break;
            }
            reject(scratch, NoView);
            return false;
        }
        // Registration lifetime of the span's owner. Every element of a span has
        // the same owner, so the first verified value serves the rest of the
        // span; a zero (no such proxy, or mid-update) is read again by the next
        // element, exactly as before.
        std::uint32_t lifetime = scratch.span == spanIndex ? scratch.lifetime : 0;
        if (!lifetime)
        {
            // The registry is process-resident. Keep one reference instead of
            // copying a shared_ptr for every span of every draw.
            static const auto registry = GetCyberpunkObjects();
            if (!registry)
            {
                reject(scratch, NoLifetime);
                return false;
            }
            lifetime = registry->lifetime(owner.proxy, owner.slot);
            if (!lifetime)
            {
                reject(scratch, NoLifetime);
                return false;
            }
            scratch.span = spanIndex;
            scratch.lifetime = lifetime;
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
                reject(scratch, NoElementIndex);
                reject(scratch, NoElementParent);
                return false;
            }
            if (span.orderKind == 0)
            {
                // Parent known, but the engine's own source order for this
                // packet was not verified.
                reject(scratch, NoElementIndex);
                reject(scratch, NoElementOrder);
                return false;
            }
            if (!span.originalIndex(ordinal, sourceIndex))
            {
                reject(scratch, NoElementIndex);
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
                    ++scratch.counts[ElementMapped];
                }
                else
                    ++scratch.counts[ElementUnmapped];
            }
        }
        // The shape is the draw's, so its hash is the same for every element.
        if (!scratch.topology)
            scratch.topology = topologyHash(shape);
        key.object = {owner.proxy, owner.mesh, owner.slot, lifetime};
        key.view = scratch.view;
        key.pipeline = pipeline.identity;
        key.topology = scratch.topology;
        key.chunk = draw.chunk;
        key.vertexFactory = shape.vertexFactory;
        key.arrayGeneration = span.count != 1 ? lifetime : 0;
        key.sourceIndex = sourceIndex;
        if (!key)
        {
            reject(scratch, Rejected);
            return false;
        }
        ++scratch.counts[Resolved];
        return true;
    }
    catch (...)
    {
        key = {};
        return false;
    }
}

void flushIdentity(const void*, PackedMotionIdentityScratch& scratch) noexcept
{
    for (unsigned i = 0; i < CounterCount; ++i)
        if (scratch.counts[i])
            counters[i].fetch_add(scratch.counts[i], std::memory_order_relaxed);
    scratch.counts = {};
}
} // namespace

PackedMotionIdentityProvider MakeGlassMotionIdentityProvider() noexcept
{
    return {nullptr, &resolveIdentity, &flushIdentity};
}

GlassMotionIdentityStats ReadGlassMotionIdentityStats() noexcept
{
    const auto read = [](Counter counter) { return counters[counter].load(std::memory_order_relaxed); };
    GlassMotionIdentityStats value;
    value.resolved = read(Resolved);
    value.rejected = read(Rejected);
    value.noOwner = read(NoOwner);
    value.noView = read(NoView);
    value.noViewState = read(NoViewState);
    value.noViewUnknown = read(NoViewUnknown);
    value.noViewDescriptor = read(NoViewDescriptor);
    value.noViewNoRecord = read(NoViewNoRecord);
    value.noViewNullResource = read(NoViewNullResource);
    value.noLifetime = read(NoLifetime);
    value.noElementIndex = read(NoElementIndex);
    value.noElementParent = read(NoElementParent);
    value.noElementOrder = read(NoElementOrder);
    value.elementMapped = read(ElementMapped);
    value.elementUnmapped = read(ElementUnmapped);
    return value;
}
} // namespace GlassFg
