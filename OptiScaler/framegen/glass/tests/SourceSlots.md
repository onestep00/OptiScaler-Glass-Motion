# Direct source-slot handoff

- Created: 2026-09-11
- Updated: 2026-09-11
- Status: implemented; standalone CPU checks passed; engine adapter incomplete
- Deployment: none; no motion or FG input changes
- Deprecated: no
- Scope: bounded CPU correspondence within one proven producer/consumer domain

`GeometrySourceSlots.h` stores source instance identities at transform-slot indices.
Capacity is selected by its owner at compile time. Publication and lookup perform
no allocation, table search or GPU operation. Starting a new domain changes an
epoch instead of clearing entries; only 64-bit epoch wrap clears the storage.
Storage must be owned outside a draw callback's stack in production.

The caller must supply the actual view, frame, owner lifetime generation and
source-array identity generation. None is inferred from an address, matching
transform, unchanged array header or draw order. Array content updates need not
change element identity; the engine adapter must establish that distinction.
The class is not a lifetime observer or motion producer.

Publication must finish before sealing and consumer lookup. Conflicting or
invalid source entries poison the individual slot for that domain. Later matching
publication cannot undo a conflict. Every begin creates a new ticket even when
view/frame values are repeated. Previous tickets, absent slots, out-of-range
slots, incomplete domains and missing view/frame are rejected. Access must be
externally ordered; concurrent producer/consumer access is unsupported. A cache
cannot be recycled while consumers still use its ticket.

`SourceSlots.cpp` checks reordered instances, stale same-frame submissions,
sticky ambiguity, invalid entries, owner/array generation passthrough and range
rejection. MSVC C++20 `/O2 /W4 /WX` build and executable passed on 2026-09-11.
These are owned CPU fixtures, not engine lifetime or GPU/FG evidence.

Before runtime use, connect actual producer view/submission provenance and
registered owner lifetime plus verified source-array identity to this handoff.
Do not use the prior offline same-frame range matches alone to populate admitted
vertex history. No production call site has been added yet.

## Producer/consumer investigation

The current executable (SHA-256
`a7de82945c03e041fc7339fcf9066224d98db2f5d80fea50f7947bb350a60991`)
has four statically validated direct callers of the observed draw-run function.
The run function copies each 16-byte packet to its stack before calling append.
Consequently the append packet address cannot identify the original producer
allocation. The caller can also partition a packet array among jobs; neither a
worker stack address nor a packet ordinal is a persistent object identifier.

The standalone producer diagnostic now records the outer context, the producer
context address and its first three pointer-sized fields with a read-valid flag.
The audited constructor in one producer caller supplies renderer, destination
family and scene context in these fields. Other routes still need corroboration.
The CPU fixture verifies header capture and missing-header handling. This source
change has not been loaded into the game: the older observer is pinned and cannot
be replaced through the coverage-module unload mechanism. No view identity or
runtime cache admission follows from these raw fields yet.

## Array update boundary

Static direct-call tracing located the array setter's caller in an engine update
worker. The worker passes an owner, bounds and a span of 48-byte transforms from
an update record, then consumes the setter's AL result. This establishes a
three-argument, byte-result observation seam for this audited call path; it does
not establish the meaning of every indirect invocation.

The setter converts entries in source order and may reuse the same allocation
when the count is unchanged. The update span provides transforms, not an observed
per-element persistent ID. Thus unchanged pointer/count and source-order copying
do not prove unchanged element identity between updates. The upstream enqueue
path still needs inspection before moving array instances can retain history
across updates. Invalidating every update would avoid false association but would
also lose motion history for continuously updated arrays; that is not accepted as
the complete moving-object solution. No array mutation hook was installed.

Further static tracing identified the enqueue implementation itself. It copies
the source transform span into an owned update record and retains the owner;
the update worker subsequently reads that record. Its address is installed at
offset 0x80 in a constructor-proven virtual table in this executable. There are
no validated direct calls to the enqueue implementation, so tracing only direct
calls would stop before the source owner. The table has no usable standard MSVC
RTTI locator immediately before it; nearby strings must not be used as a type
name. The next observation point is this virtual enqueue call and its input
span producer, not another scan of the destination transform array. The call
path is static evidence only and has not been hooked or correlated live.

## Bounded live enqueue observation

`ExperimentInstanceUpdates.cpp` is a separate CPU diagnostic, not part of the
OptiScaler build. It records at most 4,096 calls with the caller, context, input
address and 144-byte input header. It copies no pointed-to transform arrays or
GPU buffers. Complete expected enqueue function bytes guard installation from a
local profile. Stop disables recording and retains the forwarding hook/module
until game exit. This is not the unloadable coverage experiment ABI.

MSVC `/W4 /WX` DLL build and the owned-memory `InstanceUpdates.cpp` checks passed.
The fixture covers header capture, missing input, bounded stop and return-value
preservation. The live DLL hash was
`f3c24a0cc3cd64c739185057c9173a7714e8bb25cb206732c6472f21b6488939`.
In game PID 56340, explicit Start and Save returned zero and Save reported
`recording=0`. All 4,096 captured headers were readable, across 3,196 owner
pointers. Three actual return sites were observed: 1,016 at RVA 0x1e5f04,
2,521 at 0x237a1b and 559 at 0x1e5ebe. Their caller bodies were then inspected.
The sampled input spans contained no 48-byte array updates. This observation
does not establish array element identity or array-update frequency generally;
the capture stopped at its bound. No geometry MV or FG inputs were changed.

An exploratory comparison with the earlier draw census found 548 overlapping
owner addresses across 318 meshes. Only 148 entries had known prepared pipeline
identities (854, 859, 931). These are different-time address overlaps without
lifetime verification, not admitted object correspondences. The earlier captured
VS for each of those three pipelines was disassembled: all use skinning inputs;
931 has two sets of blend indices/weights. Existing `GeometryShaderTool`
`rewrite-mapped` successfully assembled and DXIL-validated all three recorded VS
with the actual-position history instrumentation. This establishes shader-format
compatibility only. It does not prove same-frame ownership, GPU history ordering,
complete material coverage or FG input substitution. The next runtime capture
must connect registered single-object identities and consecutive original VS
outputs, while grouped-array identity work remains in scope.
