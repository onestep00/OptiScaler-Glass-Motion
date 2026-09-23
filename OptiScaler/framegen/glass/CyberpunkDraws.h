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
// Every span carries its packet owner provenance (parent, single-object
// identity, element order); the draw is refused when an owner's lifetime
// changed after its append.
GeometryDrawView ReadCyberpunkGeometryDraw(const void* sourceReturnAddress, std::uint32_t indexCount,
                                           std::uint32_t instanceCount, std::uint32_t startIndex,
                                           std::int32_t baseVertex, std::uint32_t startInstance) noexcept;
// The same read. The owner provenance is completed here, at the draw, with one
// proxy header read per distinct owner, so a caller that only needs the frame,
// mesh and instance intervals passes owners=false: its spans carry no owner
// fields and no owner lifetime is checked.
GeometryDrawView ReadCyberpunkGeometryDraw(const void* sourceReturnAddress, std::uint32_t indexCount,
                                           std::uint32_t instanceCount, std::uint32_t startIndex,
                                           std::int32_t baseVertex, std::uint32_t startInstance,
                                           bool owners) noexcept;
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
// Per-reason counters for the rejections of the read above. Diagnostics only:
// the packed capture reports the aggregate as topology_rejected/shape_rejected,
// which cannot say which engine condition removed a draw from coverage. The
// counters are written on the rejection path only, so accepted reads add no
// store. Rejections returned by the other diagnostic callers are included.
struct CyberpunkShapeStats
{
    std::uint64_t rejected = 0;      // every empty return, the sum of the reasons below
    std::uint64_t noFlush = 0;       // no flush record in scope, or it is consumed/invalid
    std::uint64_t noBatch = 0;       // no current draw batch
    std::uint64_t emptyObjects = 0;  // the draw carries no object span
    std::uint64_t meshChunk = 0;     // mesh/chunk identity differs from the flush record
    std::uint64_t frame = 0;         // frame differs from the batch or the render tick
    std::uint64_t instances = 0;     // instance count differs from the flush record
    std::uint64_t records = 0;       // object storage is not the flush record's view
    std::uint64_t header = 0;        // mesh header unreadable or its fields invalid
    std::uint64_t buffers = 0;       // vertex/index buffer fields unreadable or invalid
    std::uint64_t chunkRead = 0;     // rendChunk bytes unreadable
    std::uint64_t unstableChunk = 0; // rendChunk bytes changed between the two reads
    std::uint64_t headerMoved = 0;   // mesh header changed between the two reads
    std::uint64_t buffersMoved = 0;  // buffer fields changed between the two reads
    std::uint64_t fields = 0;        // chunk field validation, the sum of the six below
    // Which chunk field condition refused the draw. The checks keep their
    // original short-circuit order, so exactly one below is charged per
    // rejection and their sum is `fields`.
    std::uint64_t fieldsVertices = 0;    // chunk vertex count is zero
    std::uint64_t fieldsIndices = 0;     // chunk index count differs from the flush record
    std::uint64_t fieldsStreams = 0;     // chunk stream count is zero
    std::uint64_t fieldsStreamRange = 0; // chunk stream count above the supported five
    std::uint64_t fieldsIndexType = 0;   // chunk index type outside 16/32 bit
    std::uint64_t fieldsIndexOffset = 0; // chunk index offset not aligned to the index size
};
CyberpunkShapeStats ReadCyberpunkShapeStats() noexcept;
// First few field rejections with the values that decided them. The counters
// above say which condition fired; these say whether the chunk or the flush
// record is the side that disagrees. Written only from rejection paths, and
// only until the storage is full.
struct CyberpunkShapeSample
{
    std::uint64_t mesh = 0;
    std::uint32_t chunk = 0, vertices = 0, indices = 0, flushIndices = 0;
    std::uint32_t streams = 0, indexOffset = 0, indexType = 0;
    std::uint32_t reason = 0; // 0..5 in the order of the six conditions
};
std::size_t ReadCyberpunkShapeSamples(CyberpunkShapeSample* out, std::size_t capacity) noexcept;
struct CyberpunkDrawStatus
{
    bool active = false;
    // `identities` counts spans admitted as single objects. Like the owner
    // counters below it is decided when a draw that reads owners consumes the
    // span, so it counts consumed spans, not appends.
    std::uint64_t batches = 0, appends = 0, identities = 0, draws = 0, rejected = 0;
    // Why an array span could not receive a verified owner (proxy) identity.
    // no_flag/no_entry/no_ticket and seeded are decided at the engine's append;
    // no_slot/no_mesh/no_header, the selection split below and the memo pair
    // when a draw that reads owners consumes the span. no_header then means the
    // owner's lifetime ticket changed between the append and the draw, which
    // refuses the draw. An array probe run decides everything at the append.
    std::uint64_t parentNoFlag = 0, parentNoEntry = 0, parentNoTicket = 0, parentNoSlot = 0;
    std::uint64_t parentNoMesh = 0, parentNoHeader = 0, parentNoSelection = 0;
    // Split of parentNoSelection: grouped update (0x2000), packet-local
    // transforms, or a range outside the owner's array.
    std::uint64_t parentNoSelectionGrouped = 0, parentNoSelectionNonGlobal = 0, parentNoSelectionRange = 0;
    // Split of parentNoSelectionNonGlobal by the packet's element count and
    // rigid/skinned destination. Only a non-global span with count > 1 gets
    // orderKind 3 or 4, so a span with count 1 or 0, and every skinned span,
    // can never reach the packet-local order probe. These counters decide
    // which of those cases is the reason the local probe reads zero. A span
    // with count 0 belongs to count_more (anything that is not count 1).
    std::uint64_t parentNonGlobalCount1 = 0, parentNonGlobalCountMore = 0,
                  parentNonGlobalCountMoreSkin = 0;
    std::uint64_t parentSeeded = 0;
    // Owner completion: spans that reused the header read of the previous
    // span's identical owner in the same draw, and header reads.
    std::uint64_t parentMemoHits = 0, parentMemoMisses = 0;
    // Grouped-array order probe: consecutive frames of the same array are
    // compared by element bytes. "permuted" counts frames where every element
    // byte pattern still exists in the packet but sits at a different ordinal,
    // which is the condition that invalidates a packet-ordinal element key.
    std::uint64_t arrayProbeGrouped = 0;
    std::uint64_t arrayProbeCompared = 0, arrayProbePermuted = 0, arrayProbeChanged = 0;
    // Same measurement for the packet-local instanced selection (particles and
    // other instanced transparency) that kind 3/4 spans come from. The
    // comparison reads the packet's own element array, which the audited
    // Append body consumes at first_argument + 48*i (see
    // batched-element-base.json). The three extra counters keep a base
    // that is not the grouped array address, an unreadable base and a base
    // that moved between two samples of the same key visible.
    std::uint64_t arrayProbeLocal = 0;
    std::uint64_t arrayProbeLocalCompared = 0, arrayProbeLocalPermuted = 0, arrayProbeLocalChanged = 0;
    std::uint64_t arrayProbeLocalSameAddress = 0, arrayProbeLocalDistinctAddress = 0;
    std::uint64_t arrayProbeLocalUnreadable = 0, arrayProbeLocalBaseMoved = 0;
    // Kind 3/4 spans that reached the probe call site: those that passed the
    // full gate, those dropped because the count was 1, and those dropped
    // because the destination is the skinned stream. gate_pass grows with
    // arrayProbeLocal when the local probe is reached at all. Only array probe
    // runs select the order kind at the append, so these count only then.
    std::uint64_t arrayProbeLocalGatePass = 0, arrayProbeLocalGateCount = 0,
                  arrayProbeLocalGateSkin = 0;
    std::uint64_t arrayProbeSameAddress = 0, arrayProbeDistinctAddress = 0;
};
CyberpunkDrawStatus GetCyberpunkDrawStatus() noexcept;
} // namespace GlassFg
