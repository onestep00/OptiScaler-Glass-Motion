#pragma once
#include "GeometryDrawBatch.h"
#include <windows.h>

namespace GlassFg
{
// Startup-only acquisition of audited renderer batch/append/flush paths.
bool InitializeCyberpunkDraws(HMODULE executable) noexcept;
// Called only from an actual public DrawIndexedInstanced observer. The source
// return address must be the audited engine call site. The borrowed result is
// valid only inside this callback; a GPU owner must copy admitted identities.
GeometryDrawView ReadCyberpunkGeometryDraw(const void* sourceReturnAddress, std::uint32_t indexCount,
                                           std::uint32_t instanceCount, std::uint32_t startIndex,
                                           std::int32_t baseVertex, std::uint32_t startInstance) noexcept;
struct CyberpunkMeshShape
{
    std::uint64_t chunkAddress = 0;
    std::uint32_t vertexBuffer = 0, indexBuffer = 0, vertices = 0, indices = 0;
    std::array<std::uint32_t, 5> streamOffsets {};
    std::uint32_t streams = 0, indexOffset = 0;
    std::uint8_t indexType = 0, vertexFactory = 0;
    explicit operator bool() const { return chunkAddress && vertices && indices; }
};
// Only inside the admitted engine draw callback. Reads the actual rendChunk
// allocation range; this does not prove GPU buffer lifetime or unchanged topology.
CyberpunkMeshShape ReadCyberpunkMeshShape(const GeometryDrawView& draw) noexcept;
struct CyberpunkDrawStatus
{
    bool active = false;
    std::uint64_t batches = 0, appends = 0, identities = 0, draws = 0, rejected = 0;
};
CyberpunkDrawStatus GetCyberpunkDrawStatus() noexcept;
} // namespace GlassFg
