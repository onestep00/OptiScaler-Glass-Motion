#pragma once
#include <array>
#include <cstdint>
#include <span>

namespace GlassFg
{
struct GeometryDrawIdentity
{
    std::uint64_t proxy = 0, mesh = 0;
    std::uint32_t slot = 0, generation = 0;
    explicit operator bool() const { return proxy && mesh && generation; }
};
struct GeometryBatchSpan
{
    GeometryDrawIdentity identity;
    std::uint32_t first = 0, count = 0, transformIndex = 0;
    bool global = false;
    // Packet owner provenance. An array parent is never a child-history key.
    GeometryDrawIdentity parent;
};
// One span per actual engine append, including rejected identities. Dropping
// an unknown span would shift every following object onto the wrong instance.
class GeometryDrawBatch
{
    static constexpr std::size_t Capacity = 2048;
    std::array<GeometryBatchSpan, Capacity> spans {};
    std::uint32_t used = 0, instances = 0;
    bool complete = true;

  public:
    void clear()
    {
        used = instances = 0;
        complete = true;
    }
    void invalidate() { complete = false; }
    void append(std::uint32_t before, std::uint32_t after, GeometryBatchSpan span)
    {
        if (!complete || !span.count || span.count > 32767 || before != instances ||
            std::uint64_t(before) + span.count != after || used == Capacity)
        {
            invalidate();
            return;
        }
        span.first = before;
        // An engine proxy may own a reorderable array of instances. A packet
        // ordinal is not persistent sub-object identity. Keep the interval,
        // but do not authorize any of those instances until that route is known.
        if (span.count != 1)
            span.identity = {};
        spans[used++] = span;
        instances = after;
    }
    std::span<const GeometryBatchSpan> view(std::uint32_t count) const
    {
        return complete && instances == count && count ? std::span(spans.data(), used)
                                                       : std::span<const GeometryBatchSpan> {};
    }
    bool globalRange(std::uint32_t count, std::uint32_t first) const
    {
        const auto records = view(count);
        if (records.empty())
            return false;
        for (const auto& record : records)
            if (!record.global || std::uint64_t(first) + record.first != record.transformIndex)
                return false;
        return true;
    }
};
struct GeometryDrawView
{
    std::span<const GeometryBatchSpan> objects;
    std::uint64_t mesh = 0;
    // IA buffer offset. This is not the shader's SV_InstanceID origin.
    std::uint32_t frame = 0, chunk = 0, stride = 0, startInstanceLocation = 0, instances = 0;
};
} // namespace GlassFg
