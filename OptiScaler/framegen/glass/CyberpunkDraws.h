#pragma once
#include "GeometryDrawBatch.h"
#include <windows.h>

namespace GlassFg
{
// Startup-only acquisition of audited renderer batch/append/flush paths.
bool InitializeCyberpunkDraws(HMODULE executable) noexcept;
// Current engine render tick resolved through the validated relocatable layout.
// This labels non-mesh draws recorded in the same engine frame. It is still a
// CPU recording identity, not proof of GPU submission or FG consumption.
std::uint32_t ReadCyberpunkDrawFrame() noexcept;
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
    // Why an array span could not receive a verified owner (proxy) identity.
    std::uint64_t parentNoFlag = 0, parentNoEntry = 0, parentNoTicket = 0, parentNoSlot = 0;
    std::uint64_t parentNoMesh = 0, parentNoHeader = 0, parentNoSelection = 0;
    // Split of parentNoSelection: grouped update (0x2000), packet-local
    // transforms, or a range outside the owner's array.
    std::uint64_t parentNoSelectionGrouped = 0, parentNoSelectionNonGlobal = 0, parentNoSelectionRange = 0;
    std::uint64_t parentSeeded = 0;
    // Grouped-array order probe: consecutive frames of the same array are
    // compared by element bytes. "permuted" counts frames where every element
    // byte pattern still exists in the packet but sits at a different ordinal,
    // which is the condition that invalidates a packet-ordinal element key.
    std::uint64_t arrayProbeGrouped = 0;
    std::uint64_t arrayProbeCompared = 0, arrayProbePermuted = 0, arrayProbeChanged = 0;
    // Same measurement for the packet-local instanced selection (particles and
    // other instanced transparency) that kind 3/4 spans come from.
    std::uint64_t arrayProbeLocal = 0;
    std::uint64_t arrayProbeLocalCompared = 0, arrayProbeLocalPermuted = 0, arrayProbeLocalChanged = 0;
    std::uint64_t arrayProbeSameAddress = 0, arrayProbeDistinctAddress = 0;
};
CyberpunkDrawStatus GetCyberpunkDrawStatus() noexcept;
} // namespace GlassFg
