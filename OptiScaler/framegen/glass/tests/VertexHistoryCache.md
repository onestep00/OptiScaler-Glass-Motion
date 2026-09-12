# Bounded vertex history ownership

- Created: 2026-09-12
- Updated: 2026-09-13
- Status: independent CPU identity, retirement and bounded-churn checks passed
- Deployment: bounded mapping wrapper connected to the local capture prototype; not deployed or enabled in game
- Deprecated: no
- Scope: fixed GPU arena ownership metadata; no motion estimation or GPU synchronization implementation

`VertexHistoryCache.h` separates history by registered object lifetime, mesh chunk,
vertex factory, pipeline, verified view and topology identity. The adapter must
supply those identities. A resource address, packet ordinal, matching vertex count,
or pipeline identity alone cannot prove them. The object ID used to extract a
screen-space boundary must remain separate so different chunks of one object do
not create false interior edges.

The default metadata table has 4,096 sets with four candidates per lookup. Its
arena represents 524,288 vertices in 128-vertex pages. A fixed buddy tree finds
and coalesces aligned blocks without a heap allocation or whole-table scan.
Power-of-two allocation introduces internal fragmentation; a full set or arena
rejects admission and increments a separate counter. This bounded cache is not
proof of sufficient coverage in every scene.

At most 64 entries are inspected for retirement per admitted frame by default.
Entries used in N or N-1 remain protected. Older entries require an explicit
contiguous GPU-completed **and recording-discarded** watermark before reclamation.
The caller must account for all views, command lists, queues and consumers. This
class does not infer completion from the engine render counter. It has no locks;
the capture owner serializes its calls.

Reclaimed storage receives a new nonzero allocation generation. That generation,
not the engine object's generation alone, must be stored in shader history tags.
The existing exact N-1 shader frame check remains necessary. Missing history is
rejected. Frame-counter or allocation-generation exhaustion requires a drained
new owner; neither silently wraps.

`VertexHistoryCache.cpp` exercises coincident object/chunk/factory/view separation,
same-key topology rejection, N-1 protection, long GPU lag, exact arena exhaustion,
coalescing and 300 frames of allocation churn. It checks all live ranges for
overlap and enforces the lookup/sweep limits. These are CPU ownership checks,
not game vertex correspondence, GPU ordering, frame-time or FG-quality evidence.

The current run admitted 7,073 requests and rejected 7,327 under deliberately
small arena pressure across 300 frames. No still-live ranges overlapped. Default
metadata size is 1,474,664 bytes in the tested x64 build, separate from GPU vertex
storage. Four lookup candidates and seven sweep inspections per test frame were
enforced; this is an operation bound, not a measured render-time cost.

Build from a Visual Studio x64 developer shell with `/std:c++20 /O2 /W4 /WX` and
place the executable and object file in the repository's `artifacts` directory.
Live integration still needs proven view/topology keys, exact producer/FG frame
correspondence, separate boundary IDs and a valid retirement watermark.

## Packed mapping and object boundaries

`PackedMotionMappings.h` now combines the existing bounded vertex allocator
with a separate four-way frame-local boundary-ID table. Different chunks,
pipelines and topology keys keep separate vertex histories while sharing the
same object boundary ID. Original array elements and owner lifetimes remain
distinct. IDs are limited to the packed shader's 15-bit domain. No table-wide
clear or unbounded lookup is required each frame.

`PackedMotionMappings.cpp` verifies multi-chunk separation, common boundary IDs,
different array elements, reordered draws, lifetime replacement and reclaimed
storage receiving new GPU generation tags. These are CPU mapping checks.

The local `PackedMotionCapture.cpp` prototype now uses this wrapper instead of
the monotonic object-only arena. It passes the allocation generation into shader
history, admits each explicitly resolved source element and derives retirement
only from its retained recording/GPU slots. The prototype compiles, but the
full source set remains uncommitted and runtime integration is not approved.
Its new `PackedMotionIdentityProvider` requires explicit verified view/topology
and original-source generations. The native host does not supply that provider
yet; missing provenance rejects initialization instead of inventing a view or
array lifetime. Queue ordering, continuous game history and FG quality remain
unfinished. The installed host and live FG inputs are unchanged.

## Original source-index lookup

Array histories additionally distinguish source-array generation and original
element index. `GeometrySourceSlots::resolveHistoryKey` validates the sealed
producer ticket against the requested view/frame, then verifies owner proxy,
mesh and generation before composing the history key. The single-object domain
stays distinct from array element zero. It does not discover an array generation.

`SourceSlots.cpp` now drives the actual source table and history cache together.
Two instances exchange packed slots and one moves to a noncontiguous slot; their
history allocations and GPU generation tags remain unchanged. A source-array
generation change obtains a separate allocation while the old N-1 allocation
is retained. Missing generations and mismatched owner/mesh/view/frame reject.
These are independent CPU tests; the native producer's lifetime/update adapter
and game draw-to-history connection remain incomplete.
