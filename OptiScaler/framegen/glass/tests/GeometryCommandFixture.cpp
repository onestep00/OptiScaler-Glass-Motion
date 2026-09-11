#include "../GeometryCommands.h"
#include "../CyberpunkDraws.h"
// Owned test identity only. The real engine callbacks have a separate fixture;
// this supplies a borrowed packet during one actual independent GPU draw.
bool geometryFixturePacket = false;
std::uint32_t geometryFixtureFrame = 42;
namespace GlassFg
{
// This independent device has no engine mesh allocation. Do not manufacture
// production mesh provenance from the fixture's synthetic object IDs.
CyberpunkMeshShape ReadCyberpunkMeshShape(const GeometryDrawView&) noexcept { return {}; }
GeometryDrawView ReadCyberpunkGeometryDraw(const void*, std::uint32_t indices, std::uint32_t instances,
                                           std::uint32_t startIndex, std::int32_t baseVertex,
                                           std::uint32_t startInstance) noexcept
{
    static const GeometryBatchSpan object { { 1, 2, 3, 4 }, 0, 1, 0, false };
    static const GeometryBatchSpan batch[] { { { 1, 2, 3, 4 }, 0, 1, 0, false },
                                             { { 5, 2, 6, 7 }, 1, 1, 1, false },
                                             { { 8, 2, 9, 10 }, 2, 1, 2, false } };
    if (geometryFixturePacket && indices == 6 && instances == 3 && !startIndex && baseVertex == 2 && startInstance == 7)
        return { batch, 2, geometryFixtureFrame, 0, 48, startInstance, instances };
    if (!geometryFixturePacket || indices != 6 || instances != 1 || startIndex || baseVertex != 2 ||
        startInstance < 7 || startInstance > 9)
        return {};
    return { std::span(&object, 1), 2, 42, 0, 48, startInstance, instances };
}
} // namespace GlassFg
