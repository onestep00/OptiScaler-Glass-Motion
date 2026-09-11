# Direct source-slot handoff

- Created: 2026-09-11
- Updated: 2026-09-11
- Status: creation/destruction cache adapter passes independent checks; live lifetime/view adapter incomplete
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

## Source-owner lifetime cache

`GeometrySourceOwners.h` retains creation-source metadata separately from the
later renderer registry slot. Its caller must synchronously serialize creation,
destruction and queries, and observe destruction before engine memory release.
It does not infer lifetime from a pointer or add an engine hook. Every creation
gets a new nonwrapping serial, even when handle/proxy/source addresses match a
previous record. Cancellation checks the handle as well as the proxy, so a
different owner's delayed cancellation cannot delete the replacement. A malformed
new creation invalidates the old proxy association instead of preserving stale
history. Events delayed across reuse of the same handle address are not supported;
the required synchronous destruction seam must prevent that ordering.

Storage consists of fixed entries and hash buckets plus a free list. Creation,
lookup and removal allocate nothing and copy no engine resources. Lookup walks
one hash chain; unlike the direct source-slot table, it is not a guaranteed
single-index operation. Capacity exhaustion rejects new owners without evicting
live entries. The large production-capacity object must be heap-owned, never
placed on a draw callback's stack. Registry generations, source mutation, actual
producer indices and frame/GPU ordering remain independent admission checks.

The existing SourceSlots fixture now covers creation before registration, bounded
capacity, cancellation, reused addresses, wrong render mesh, malformed replacement
and source-range overflow. `/O2 /W4 /WX` build and execution passed. These tests
do not certify the engine callbacks or complete runtime MV integration.

The audited renderer-handle constructor at 0x296d3c installs vtable 0x2ac86b0 and
retains the original proxy at handle+0x10. Its first vtable entry, 0x29665c, calls
destructor body 0x296688 before optional deallocation. That body removes associated
state and releases handle+0x10. This supplies a concrete candidate cancellation
seam for pending-source metadata, subject to live hook and call-contract checks.
No new destructor hook has been installed. Local function disassemblies are under
`work/glass-engine-identity-probe-v2/`.

## Producer/consumer investigation

### Same-process follow-up and whole-array route

A subsequent bounded read-only check followed the recorded node candidates to
their current renderer handles and compared them with the linear producer tuples.
It accepted 2,254 current chains; 13 candidate records had changed node handles
and were rejected. Each accepted chain was read twice and included node/shared
handle controls, original source range, typed render mesh, renderer handle,
proxy, registry slot and current array count. This used 45,093 metadata reads /
559,200 bytes and copied no transform contents. The historical candidates only
bound the search; they do not prove temporal identity or safely bootstrap a cache
without concurrent lifecycle observation. No shader input changed.

Local reproducible checker: `work/check-glass-source-chain.py`. Result:
`work/glass-instance-producer-live-v3/capture-2/current-source-chains.json`.

The whole-array-only profile 0x49535033 was subsequently built and tested in
PID 70152 without restarting. It uses the extended header but installs only the
outer producer and packet hooks. It deliberately does not install the selection
leaf hook; that function's broader internal register contract needs separate
review. All three recorded bodies matched resident bytes before loading, and
Start/Save returned zero. The first interval recorded 20,987 linear source entries
over five frames, spanning 3,121 proxies and 273 meshes with 2--48 source elements.

A second interval overlapped hot census generation 4. Its 20,857 source entries
cover frames 52570--52574; the census covers 52568--52575. Matching exact
frame/render-mesh/global-slot keys resolved every instance in 3,230 anonymous
draw ranges (17,434 instance occurrences), with zero conflicting source keys.
Another 2,689 ranges had no source overlap; bounded recording intervals and the
grouped route remain limitations. No partial/ambiguous range occurred among the
overlapping keys. This is not a lifetime/view admission proof or all-transparency
coverage. The source observer stopped recording and stays pinned forwarding;
the census module unloaded with no pending GPU captures. The game remained
responding. No object MV or new FG input was produced.

Local evidence: `work/glass-instance-producer-live-v3/preflight.json`,
`capture/analysis.json`, `capture-2/source-indices.csv`, and
`work/glass-node-draw-census-v4/capture/linear-source-join.json`.

PID 70152 remained responding during a new census-only generation-2 load and
unload, with zero GPU captures pending or recorded. The new census has 32,768
draw rows, including 29,806 rows across seven known engine frames. Its 37,685
object entries contain 33,535 nonzero proxy/generation pairs and 4,038 anonymous
global-array entries. These are general draw observations, not verified transparent
object counts or complete-frame coverage; contention/overflow still truncate the
sample. The concurrent node-creation recording captured zero calls. Existing
objects therefore need an explicit live acquisition route as well as future
creation observation. No new MV or FG input was produced. Local evidence:
`work/glass-node-draw-census-v2/capture/analysis.json` and
`work/glass-node-groups-live-v1/capture-3/status.txt`.

Further inspection of the original producer 0x1E9B88 found a distinct call at
0x1EA1C9 (return 0x1EA1CE): its descriptor copies global start/count from
proxy+0x114/+0x110, uses first=0, and explicitly clears the span. This is separate
from the grouped/reordered call returning at 0x1E9DC1. The diagnostic now accepts
an optional profile version 0x49535032 with an additional explicit linear return
RVA after the seven-word header. Old version 0x49535031 retains grouped-only
behavior. Both still verify the three complete target bodies before installation.

`resolveLinear` requires this explicit route, exact owner global start/count,
zero first/span, and a valid endpoint. It maps ordinal to the original array
index without reading an index list. A missing grouped index never falls back to
linear order. CSV adds `linear_source`; group/source_indices are zero in this
mode. The producer fixture passed reversed grouped order, exact linear order,
missing-group rejection and malformed linear descriptors with `/O2 /W4 /WX`.
This source change is not deployed and does not establish element lifetime or
view/frame-to-FG ownership. No additional game hook was installed in this turn.

### Connected lifetime diagnostic (not deployed)

`SourceQueryConnection.cpp` subsequently loaded the independent fixture DLL
`SourceQueryProvider.cpp` and exercised the actual exported owner query through
the producer's `connectSource` and `observe` paths. Missing/relative provider paths
were rejected; an absent sidecar left lookup disabled. Creation reached the
producer, destruction removed it, and recreation returned a newer generation.
After releasing the test's original LoadLibrary reference, the pinned callback
remained callable. Both files compile with `/O2 /W4 /WX`, and the executable
passed. Fixtures never install engine hooks and must never be injected into a
game. Local executable/provider artifacts are in `work/glass-source-query-test-v1/`.
This closes independent module discovery/pinning coverage, not live engine
lifetime coverage, bootstrap or FG admission.

The producer now consumes the scalar owner-query ABI. An optional UTF-8 `.source`
sidecar contains one absolute path to an already-loaded owner-query DLL. Start
resolves its named export and pins the module; it neither loads a guessed DLL nor
reads this file per frame. Missing sidecar preserves source-index-only diagnosis.
A configured but unavailable provider makes Start fail before installing hooks.

Each outer producer scope queries at most once and validates ABI, count, range,
nonzero generation and identities. Its CSV retains the original local source
index and additionally writes source node/buffer/generation and absolute buffer
index. A missing/malformed result leaves those ownership fields unavailable;
exceptions do not escape into the engine. The fixture verifies one query for
multiple records in a scope, index translation, count mismatch and exception
containment. Build and execution passed. That fixture uses a callback substitute;
the separate connection test above covers discovery/pinning. The currently pinned game
producer predates this consumer; no new pair was deployed.

The lifetime diagnostic now exports `GlassSourceOwnerQuery` through
`ExperimentSourceAbi.h`. A synchronous same-process caller supplies its live
proxy, render mesh and original array count. The query returns only node/buffer
identities, original first/count and the creation serial; it never returns a
borrowed transform pointer or reads engine memory. Lookup takes one cache lock
per owner query. The caller can then translate the producer's local source index
within the returned range, subject to separate lifetime and frame validation.

The query refuses an uninstalled observer, incompatible ABI, missing owner or
count mismatch. A failed destructor metadata read permanently disables queries
for that observer, because an unobserved destruction could leave stale entries.
Stopping CSV capture still leaves lifecycle tracking active. `NodeLifetimes.cpp`
passed source query, ABI rejection, post-destruction rejection and sticky failure
checks using the actual exported function pointer. This is an independent callback
test; the new export is not loaded into the current game. The source producer
consumer above is not deployed. Returned scalar metadata is not a GPU lifetime lease or
proof of consecutive-frame correspondence.

`GLASS_NODE_LIFETIME` connects the actual node-creation adapter to the bounded
source-owner cache and invalidates its entry before forwarding renderer-handle
destruction. Typed CMesh+0x1F0 and proxy+0xD8 must agree before publication.
CSV q15 records the render mesh and q17 the creation serial. This is scalar
ownership metadata, not position history. No transform array or GPU buffer is
copied. Recording exhaustion or Save stops the CSV only; lifecycle tracking
continues, so stopping diagnostics cannot silently retain obsolete ownership.

The separate profile magic is 0x49555035. Its original creation-body record is
followed by destructor RVA/byte count and the complete expected destructor body.
Both targets are verified before a single detour transaction. Old profiles cannot
enable this mode. No profile or new hook was deployed in PID 70152, which already
has the preceding creation observer installed. Do not stack this on that hook.

`NodeLifetimes.cpp` checks publication, invalidation before original release,
same-address recreation with a new serial, continued tracking after CSV stop,
and mismatched render-resource rejection. The transform allocation is deliberately
unmapped. The new fixture, four existing ABI fixtures and standalone DLL built
with `/O2 /W4 /WX`; all five fixtures passed. These fixtures call adapters on
owned memory; they do not exercise the two-hook installation or certify engine
concurrency, complete mutation coverage, view/frame identity or FG integration.

Static direct-call analysis found one instruction-validated call to destructor
body 0x296688, in deleting destructor 0x29665C. That caller retains the handle and
delete flag in nonvolatile RDI/RBX and returns the handle; the shown normal return
does not use the quarantined leaf's R10 convention. This does not prove all
indirect callers. Local evidence is `work/glass-node-lifetime-audit-v1/`.
Source-array replacement, actual draw-source publication and N-1 GPU positions
remain unconnected. The diagnostic serial is not permission to reuse old motion.

### Node-owned renderer handles and skipped source groups

Red Hot Tools source at b4d341527bce19842d16a757028be901d4a2d6a8
defines `worldInstancedMeshNodeInstance` mesh at +0xB8 and its render-proxy
handle array at +0xE8 (`src/Red/WorldNode.hpp`). Its WorldNodeRegistry observes
Initialize/Attach/Detach and streaming-sector destruction. These are usable
upstream implementation references, not proof of current lifecycle integration.

The audited creation functions 0x3c8508 and 0x2276c30 append a 16-byte handle to
instance+0xE8 through 0x3c8f44 only after admission succeeds. Their enclosing
source loops can therefore skip a group without appending a placeholder.
Renderer array ordinal must not be used as the original source-group index.
In PID 70152, all 1,338 bytes of these three functions and their two enclosing
source loops matched the audited on-disk executable exactly. Read-only evidence:
workspace `outputs/glass-node-proxy-route-v1/runtime-code-audit.json`. No hook or
game input changed. This verifies resident code, not observed object instances.

`GeometrySourceSpan::resolve` computes the original element range from the
actual supplied span and parent allocation metadata in constant work. It never
scans group counts or copies transforms. It rejects misalignment, empty spans,
allocation/parent overrun and index overflow. `SourceSlots.cpp` checks a skipped
leading group retaining source index 107 despite renderer ordinal zero, reversed
observation order and malformed ranges. This is spatial provenance only: the
engine caller must still supply live metadata, successful creation correspondence,
node/buffer lifetime generations, and render-frame ordering. The helper neither
invents a generation nor admits vertex history. Runtime hookup remains absent.

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

The separate `GLASS_NODE_GROUP` mode observes the outer creation function with
its four original arguments: node instance, float distance parameter, source span
and bounds. It snapshots the original node/shared-buffer range before forwarding,
then records only a single successfully appended renderer handle and its owner.
The decoded source span remains independent of the compacted renderer ordinal.
It copies scalar headers only, with no transform-array or GPU readback. This
mode uses profile magic 0x49555034 and the same bounded Start/Save control.

`NodeGroups.cpp` passes exact mixed float/pointer forwarding, skipped creation,
reordered source spans, allocation-bound rejection and disabled observation.
The transform allocation in the fixture is deliberately unmapped. Existing
ArrayWrapper, InstanceUpdates and TransformRange fixtures also pass. MSVC
`/O2 /W4 /WX` builds passed. These are ordinary-ABI fixtures; audited caller
liveness remains necessary before runtime use. Neither source addresses nor
successful append events establish temporal generations by themselves.

The node-group DLL (SHA-256
`d9f2992f3485508e30a814b5296f48e448dfb22eb03e2c8fd2987a5c649fba46`)
was loaded into PID 70152 after the resident creation/parent bodies were checked
against disk. The two audited direct caller paths do not carry the leaf's R10
dependency across this outer call; the mixed float input is forwarded in XMM1.
The 401-byte creation profile installed, and Start/Save returned zero. The first
saved interval contained zero calls (zero rows and zero rejected observations).
Recording is stopped; the process remained responding. This is installation
evidence only, not live source/proxy data or broad stability proof. Local output:
`work/glass-node-groups-live-v1/capture/`. The quarantined leaf was not installed.

After the user reloaded the same save in PID 70152, capture-2 reached its 4,096-row
bound with zero rejected observations. All calls returned to 0x2542cf. Every
record's span endpoints matched the decoded original shared-buffer index/count,
and every renderer array grew by exactly one. The records cover 4,090 node
addresses, 4,069 definitions, 64 shared buffers and 4,082 proxy addresses.
In 4,084 records the original source index differs from the renderer ordinal.
Fourteen repeated proxy addresses each have different source tuples, reinforcing
that address-only temporal history is invalid. Recording is stopped.

A later read-only snapshot followed node handle -> node-owned renderer array ->
renderer handle -> proxy for 4,002 records; all their current array counts matched
the recorded source counts. A first exploratory check incorrectly required the
node's CMesh pointer to equal the proxy's CRenderMesh pointer and rejected all.
SDK CMesh+0x1F0 and the audited 0x3c93e0 preparation code establish the explicit
CMesh-to-render-resource link. Repeating with that typed link accepted 3,987
current chains across 426 render meshes. These are separate observation times,
not a claim that the difference of 15 proves destruction. One accepted group has
40 instances beginning at shared source index 496; its visual object identity
has not been established. Node field q7 is CMesh, not a draw's render-mesh key.

Evidence: `capture-2/analysis.json`, `chain-stages.json` and
`current-chain-typed.json` under the same workspace diagnostic directory.
`current-chain.json` retains the rejected untyped comparison for auditability.
Current address/handle agreement is not a lifetime-generation proof, and none of
these reads supplies previous vertices or changes FG input. Connecting the
captured provenance to registered generations and the draw consumer is next.

A subsequent census-only hot module saved 32,768 draw observations in PID 70152,
then unloaded with zero GPU captures/pending jobs. It deliberately selected no
capture pipeline. Of those rows, 4,430 share a render mesh with the preceding
typed node snapshot, spanning 169 meshes. This is mesh-resource correspondence,
not individual source-instance ownership; meshes are shared and the snapshots
are not simultaneous. The single 40-source-instance mesh appears in 152 rows
with several draw counts/chunks and unclassified pipeline IDs. It must not be
identified as a cup from the instance count. The census contains frame-zero
observations, 51,610 contended attempts and 1,296,834 overflow attempts, so it is
not a complete frame census. Local evidence:
`work/glass-node-draw-census-v1/capture/{draw-census.done,node-mesh-join.json}`.

Control requests in this run had to use MO2 overwrite/bin/x64/Glass for both
request and response. An older overwrite request shadowed a request written to
the physical game path; the first status call timed out but did not load a DLL.
The corrected status/load/disable requests completed and the module unloaded.

Further creation tracing shows 0x296cd0 can either call 0x297314 immediately or
append the renderer handle to a pending list when its 0x400 counter limit is
reached. Therefore a source association must not assume registration has already
completed at outer creation return. Pending-source publication and later registry
generation attachment need explicit ordering/lifetime handling. This call graph
alone does not establish the completion order of its virtual renderer calls.

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
