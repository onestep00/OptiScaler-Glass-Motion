# Direct source-slot handoff

- Created: 2026-09-11
- Updated: 2026-09-11
- Status: slot cache implemented; live array enqueue and differing upstream packing verified; lifetime/view adapter incomplete
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

### Simultaneous live producer and draw checkpoint

In PID 68908, the byte-guarded producer observer recorded 4,096 producer
calls / 28,249 source-index rows over engine frames 39322--39368. A concurrent
replaceable draw recorder captured the consumer census. Both recordings stopped;
the coverage module unloaded with all 133 cumulative jobs retired. The producer
forwarding module remains resident with recording disabled. No new FG input was
installed.

Excluding 839 non-global or invalid rows (including the UINT32_MAX global-start
sentinel), there were 24,428 distinct frame/mesh/global-slot keys with no conflicting
proxy/source-array/source-index candidates. All 236 consumer draws with any such
candidate had candidates for every instance. This includes the 40-instance cup
draw and other pipeline families; it is not an all-scene coverage result. Nine
observations of the selected cup draw matched its source indices 0--39. Source
array, owner slot and producer contexts were captured directly, without transform
matching or GPU buffer copying.

The same owner/source mapping can appear through different producer contexts.
The offline key deliberately collapses them only to test agreement; it does not
establish a production view/submission domain. Initial apparent slot conflicts
came from erroneously including the non-global sentinel and were removed by
correcting the analysis, not by selecting one of the conflicting owners.

Evidence: workspace `work/glass-producer-join-live-v1/all-source-joins.json`,
`join.json`, and `work/glass-instance-producer-live-v2/capture/`. The engine binary
hash matches the previously audited hash below. Local addresses are diagnostic
observations and must not become production constants.

The resident registry already exposes registration-generation tickets, and the
single-object draw adapter validates those tickets at consumption. Grouped draws
still expose only an anonymous global range. Next connect producer provenance to
that range with the registered owner generation, then establish source-array
element lifetime and actual view/submission ordering. Unchanged array addresses
or these successful offline joins alone do not authorize N-1 vertex history.

An explicit 40-instance vertex capture subsequently recorded 64 snapshots with
626 vertices per instance (1,602,560 valid tagged vertices). Frames 44910/44911
had matching producer source-index candidates for all 40 instances and 25,040
finite positive-W clip pairs. Their raw raster displacement median was 0.45483
pixels, maximum 0.49241, including camera jitter. This is actual VS output, not
image-estimated motion; it is not dense pixel MV or approved temporal ownership.
Only two snapshots overlapped the bounded census sufficiently for that exact
recording join. Vertex sidecars now also retain original pipeline/draw arguments,
including start-instance, so later joins do not depend on an earlier census row
remaining within its recording capacity. No resource data is added to the sidecar.
Evidence: `work/glass-array-vertices-live-v1/vertex-analysis.json` and
`consecutive-pair.json`. Generation 12 retired/unloaded all 64 jobs; the producer
observer is stopped. The new sidecar fields require rebuilding the diagnostic.

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

### Transform pool allocation is not persistent element identity

Further static tracing connects array allocation `0x3cc2e8`, converted writes
`0x3cc3c0`, raw 48-byte writes `0x3cc59c`, queueing `0x3cc658`, and release
`0x3cdeb8`. The pool allocator checks a 131,072-element endpoint. These are RVAs
for the audited executable, not production discovery signatures or live proof.

The raw writer is called from mesh preparation at `0x1e89bd`. Immediately before
it, the engine walks 16-bit source indices, gathers the corresponding entries
from proxy+0x108, and forms a contiguous span. Its destination begins at
proxy+0x114 plus a running packed offset; the group stores that destination at
+0x50. Therefore pool-allocation generation plus destination index alone cannot
authorize temporal correspondence when the grouping/order changes. Preserve the
original source-index mapping, even if pool lifecycle tracking is added.

The previously observed enqueue callers `0x1e5e60` and `0x1e5ec8` use leaf request
builder `0x1e5f0c`, which explicitly zeroes array spans +0x60..+0x78. The third
observed caller `0x237974` also zeroes those spans. Repeating only that bounded
sample is not a reliable way to observe array replacement. The true nonempty-span
producer and element lifetime remain unresolved. Local evidence is in
`outputs/glass-transform-pool-route-v1/` and the owned PE disassemblies;
`outputs/glass-array-enqueue-dispatch-v1/` contains unverified dispatch candidates.

### Native previous-transform alternative

An offline scalar-dependency trace of the already live-checked native VS
`7193f0d2...` separates its current output 4 from previous output 5. Current uses
per-instance input 6 rows 0--2. Previous instead uses fixed b7 rows 1--3 and
b1 rows 16--19; no per-instance input contributes to that previous output.
The translation components are integer-bitcast, origin-relative values, not
ordinary float matrix translations. This shader cannot simply replace the
40-instance cup VS while preserving distinct previous transforms for each cup.

The captured cup VS uses per-instance current transforms and b1 rows 28--31.
Both descriptors expose a single b7 CBV at root table slot 3, but this establishes
neither equal buffer contents nor valid previous data at the transparent draw.
These static data dependencies exclude control-flow/runtime validity claims.
Local evidence: `work/glass-native-input-dependencies-v1/{native,cups}.json`,
original shader disassembly and the prior `glass-bindings-live-v12` layouts.
Keep grouped source identity/history work active; a matching root layout alone
does not remove that requirement. No shader swap or game input change was made.

A current-cache audit then expanded to all 270 velocity techniques whose base
factory is MeshStatic, including Discarded/PreSkinned variants: 93 unique VS.
All expose three INSTANCE_TRANSFORM rows and none expose SV_InstanceID. Seven
shaders have dynamically indexed constant-buffer loads, all at b8/b9, not b7.
Those seven belong to skin-family and spline variants. Other resource reads still
require dataflow analysis; this inventory does not prove absence of a separate
previous-instance path. Non-MeshStatic factories are outside this audit. Cache
SHA-256 was rechecked before extraction. Local scripts and results are
`work/audit-static-velocity-variants.py` and
`work/glass-static-velocity-variants-v1/{audit,dynamic-reads}.json`.

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

The diagnostic now also exports `GlassInstanceStartArrays48`, taking the same
fresh absolute output-directory argument as Start. It only reserves a capture
row for a readable request header with a nonempty, ordered, user-range span at
+0x60/+0x68 whose byte length is divisible by 48. Empty/malformed counts are
reported separately. It does not dereference that span or certify its contents.
Default Start retains the original unfiltered behavior. Stop still pins the
forwarding hook until game exit; this is not a production per-frame observer.

The independent fixture passed 5,000 filtered empty requests followed by a
captured array request, malformed/reversed spans, unreadable headers, bounded
stop and unchanged original return values. The standalone DLL compiled with
`/W4 /WX`. Its later live deployment is recorded below; array-element identity and
the final FG correction remain incomplete. This filter prevents the previously
observed single-object calls from exhausting the array investigation's budget.

### Live array enqueue and upstream packing

PID 62100 loaded diagnostic SHA-256
`41d8dd8c12c6a29c6a1031f934efc7aed193c174c0248b975c3e9534aecb6107`.
StartArrays48 and Save returned zero. Capture-2 filled its 4,096-row bound with
4,096 distinct owner pointers, spans of 1--50 entries, and one return RVA,
`0x3cbd30`. It skipped 451,319 empty requests and reported no malformed headers.
The recording began before the user loaded the save. It cannot establish which
of these records occurred after loading, or identify a particular cup from count.

A second recording started after the user confirmed the save was loaded.
Capture-3 observed 648,984 empty requests but zero array requests. Both recordings
were saved with recording disabled. This is an observation interval, not proof
that arrays never update during gameplay. It rules out using this observed
event stream alone as a per-rendered-frame sample clock. No transform-array
contents or GPU buffers were copied and no FG inputs were changed.

Static inspection identifies `0x3cbca8` as the common wrapper. It copies the
caller's begin/end pair from RDX into request +0x60/+0x68, reads the owner from
the handle at RCX+0x10, and submits through the interface at RCX+8. Six validated
direct call sites reach this wrapper in the audited executable. This does not
claim complete indirect-call coverage. The caller bodies have different source
packing behavior:

- `0x579ab8` walks 32-byte input entries and appends only those enabled by its
  source-indexed bitset. A packed ordinal can therefore change when earlier
  entries become inactive.
- `0x57b114` walks 64-byte input entries. Its inactive branch appends a placeholder
  and sets the corresponding bit in a separate mask; this differs from the
  preceding compacting path. Source-array replacement still needs validation.
- `0x3a2038` builds 48-byte transforms from position/rotation/scale arrays in
  explicit source ranges before enqueueing each group.
- `0xa040c0` walks 32-byte inputs in order and appends transformed entries.
- `0x3c8508` and `0x2276c30` forward an externally supplied span; their local
  bodies do not establish its element lifetime.

These findings require preserving source provenance before packing. Neither the
common wrapper's packed ordinal nor unchanged count is a general object ID.
The next diagnostic must distinguish the wrapper's upstream caller and its
source-index domain, then connect that to the later render grouping. Do not add
CPU vertex copies or a full-screen mask per instance to solve this identity gap.
Local evidence: `work/glass-instance-updates-live-v2/capture-{2,3}/analysis.json`
and `work/glass-engine-identity-probe-v2/function-{3cbca8,579ab8,57b114,3a2038,a040c0,3c8508,2276c30}.txt`.
All RVAs describe the audited binary only; no production hook or signature was added.

### Upstream wrapper diagnostic

`ExperimentInstanceUpdates.cpp` also builds with `GLASS_ARRAY_WRAPPER`. This
separate diagnostic observes the audited three-argument wrapper before enqueue,
so the recorded caller distinguishes its upstream producers. It forwards all
three original arguments and the byte result unchanged. It reads only the
handle's owner field (8 bytes), begin/end span (16 bytes), and bounds (32 bytes).
It does not follow the span or infer original element identity from the wrapper.

The CSV uses normalized fields in this mode: q0 is the handle owner, q8--q11
are bounds, q12/q13 are span endpoints, context is the handle, and input is the
span-header address. These are not a copied 144-byte enqueue request. A distinct
profile magic `0x49555032` rejects ordinary enqueue profiles; full target-body
bytes still guard installation. The original mode retains `0x49555031` and its
two-argument ABI. Both modes stop recording and remain pinned until process exit.

`ArrayWrapper.cpp` passed argument/result forwarding, upstream return capture,
normalized fields, unreadable inputs, 5,000 empty-span skips, malformed spans,
capacity stop and an intentionally unmapped pointed-to span. The original
`InstanceUpdates.cpp` fixture also passed after the shared-source changes.
Both fixtures and the wrapper DLL compiled with MSVC `/W4 /WX`. These checks
establish diagnostic behavior, not engine source identity or production MV.

The wrapper DLL was then loaded by exact path into the still-running PID 62100,
with SHA-256 `83079a5e39c95965aaf62f69a33d8369b2d547cc0ace88765c1dab3014737847`.
Its full 149-byte wrapper profile passed installation; StartArrays48 and Save
returned zero. The saved observation interval contained no wrapper calls, empty
or nonempty. Recording is disabled and the process remained responding. This
proves installation/control only, not live argument capture; a later array supply
event is still required. Local output: `work/glass-array-wrapper-live-v1/capture/`.
The older enqueue observer remains separately pinned with recording disabled.
Neither diagnostic changes geometry, render commands or FG input resources.

The user then moved in the scene while capture-2 was armed. Save returned zero
and produced 1,053 records without exhausting capacity: 86 returns at 0x3a2559,
807 at 0x3c8657, and 160 at 0x579d30. Spans had 1--16 entries; all headers passed,
with no empty/malformed calls. The process base was rechecked as 0x7ff69d770000
and the process remained responding. The previous zero-call interval therefore
does not mean this is exclusively a loading-time path.

There were 760 owner addresses and 1,039 handle addresses. Among 234 repeated
owner addresses, 183 appeared with different counts, 93 with different upstream
callers, and all with different handle addresses. These are address repetitions,
not proven same-object updates: registration generations were not captured.
Do not infer persistent identity from these repetitions or from handle addresses.
Evidence: `work/glass-array-wrapper-live-v1/capture-2/{analysis,repeat-analysis}.json`.

Further static tracing of the dominant 0x3c8508 path found direct callers
0x25422c and 0x2277018. Both partition a source array into contiguous 48-byte
spans using cumulative per-group counts, with separate per-group bounds. This
locates the source group/range before the renderer handle is created. The next
identity observation should preserve that parent source and group range rather
than equating newly created renderer handles across frames. This does not yet
prove parent-source lifetime or moving-element stability. The owned function
disassemblies are under `work/glass-engine-identity-probe-v2/`.

### Typed source of the dominant grouped path

The checked cast at 0x3c8b08 references type global 0x342b160. A bounded read
of that global and its type metadata in PID 62100 identified name hash
15851563823148046153, `worldInstancedMeshNode`, with size 0xA0. The local
RED4ext SDK at commit ad7277714ad30d6885d7050c5ba24fa0102f6920 has matching
generated type size and `worldTransformsBuffer` at node+0x38. This verifies the
cast's target type, not the identity/lifetime of every observed source object.

The actual accessor at 0xaefd28 reads the shared buffer handle at range+0,
startIndex at +0x10 and numElements at +0x14. It computes begin as
`sharedBuffer.data + startIndex * 48`, with data read from sharedBuffer+0x30,
and end as `begin + numElements * 48`. The two partitioning callers use this
accessor on node+0x38 before applying their cumulative per-group offsets.
Therefore this path has a concrete upstream shared-buffer index domain;
the per-group renderer handle need not define that domain. The original node
instance, its lifetime, buffer replacement and group-to-renderer handoff still
must be captured together before admitting history. The checked-cast wrapper
loads the node handle from its input instance+0x60.

Local evidence: `work/glass-array-wrapper-live-v1/source-type.json`, owned
function disassemblies 0x3c8ae8/0x3c8b08/0x25422c/0x2277018 and the leaf
accessor bytes at 0xaefd28. This is a potential bounded scalar provenance route,
not a claim of static object motion, all-transparency coverage or FG integration.

### Shared-range accessor diagnostic

**Quarantined after PID 62100 exited during the reload investigation.** No new
matching crash dump or Windows Application Error record was found, so crash
attribution is not proven. However, caller 0x25422c keeps R10 across its call at
0x254263 and immediately dereferences it at 0x254268; the original leaf preserves
R10 but the C++ diagnostic does not guarantee that. Successful ordinary-ABI
fixtures did not cover this internal register contract. The install function now
unconditionally refuses this mode, including the cached-target path. The old
local binary has a QUARANTINED marker and must not be injected. Resolve this
through full call-site liveness analysis and an independently verified preserving
adapter before any further game deployment. The other retained observations do
not prove their hook contracts safe by association. The following paragraphs
describe the earlier, insufficient verification and are not deployment approval.

The same standalone source now supports `GLASS_TRANSFORM_RANGE`, mutually
exclusive with wrapper mode. This mode forwards the audited two-argument
accessor first and preserves its pointer return and output span. It reads the
24-byte original range, the resulting 16-byte span, and one 8-byte data pointer
from the shared buffer. It copies no transform contents. Its distinct profile
magic is 0x49555033. Normalized fields q0/q1 retain the shared handle, q2 packs
start/count as supplied, q3 is the shared data pointer, and q12/q13 are the
returned endpoints. Context is the range-header address; input is the output
span-header address. Neither is automatically a persistent object identity.

`TransformRange.cpp` passes pointer-return/output preservation, exact metadata,
unreadable inputs, intentionally unmapped transform data and bounded stop.
The wrapper and original enqueue fixtures were rebuilt and also passed.
All three use MSVC `/W4 /WX`. The runtime comparison must verify the observed
span arithmetic and distinguish shared-buffer/source-range provenance from
temporal owner lifetime; pointer containment alone does not authorize history.

The accessor DLL, SHA-256
`e191f78aac2d6fba80ff5235f10e11800f8b1b4f50b5ff09db8a83d3586dd4ce`,
loaded into PID 62100 without restarting. Its 51-byte audited leaf profile
passed installation and both Start and Save returned zero. The first interval
had zero accessor calls and the simultaneous wrapper interval also had zero
calls. This is control/installation evidence only. Raw local output is in
`work/glass-transform-range-live-v1/capture/` and
`work/glass-array-wrapper-live-v1/capture-3/`. No FG input was changed.

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
