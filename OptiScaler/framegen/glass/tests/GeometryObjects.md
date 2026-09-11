# Engine object lifetime and pose candidates

- Created: 2026-09-11
- Updated: 2026-09-11
- Status: lifetime-only default passes registry and production callback checks; live geometry MV incomplete
- Deployment: latest cache reduction is source-only; installed host remains unchanged
- Deprecated: no
- Scope: audited Cyberpunk mesh-proxy registration, removal and one transform-update route

## Implemented behavior

The runtime registry now defaults to lifetime-only storage. A current call-site
search found that production draw consumers use `ticket`, while pose `find` is
used only by standalone diagnostics. The default therefore allocates no pose
nodes or pose hash buckets, and performs no pose insertion/hash updates. Explicit
`indexPoses=true` retains the old diagnostic candidate index described below.
Neither mode supplies previous vertices to FG or permits stale-frame motion.

The eight-slot fixture measured 192 bytes of vector storage in lifetime mode
versus 3,072 bytes with the pose index. At the configured 131,072 slots this
corresponds to 3 MiB versus 48 MiB (45 MiB less), excluding vector/lock/allocator
overhead. This is structural CPU storage accounting, not measured process memory
or frame time. Existing engine callback snapshot reads and its update hook remain;
they have not been removed by this change.

Lifetime-only tests cover update, generation invalidation, stale callback rejection,
removal and pending registration recovery without pose lookup. The production
draw callback fixture now runs with that default and passes identity reuse,
coincident objects, batching and unknown-array rejection. All three executables
in `build_geometry_objects.ps1` passed. No full DLL rebuild or game deployment of
this reduction has occurred yet.

`GeometryObjectRegistry` associates an engine registry slot with its proxy, mesh and generation. Reusing a slot/address or replacing its mesh cannot inherit the previous geometry generation. A repeated registration of the same live identity does not change its generation. Unregistration removes all lookup entries before the engine frees the slot. An update must present the generation sampled before its original call; an old callback cannot modify a newer occupant.

Four recent packed poses and bounds are retained per slot. The lookup compares the full mesh identity and all 48 transform bytes. The transform's W lanes contain fixed-point position bits and are never treated as floating-point numbers. Bounds size storage only; they do not determine a silhouette or calculate MV. The actual motion path still obtains original VS output on the GPU.

Pose lookup is a **candidate index**, not proof of a draw's owner. If two indexed live objects match, lookup rejects both. A unique indexed match is also insufficient when another mutation path or object is missing from observation. The draw adapter must establish direct proxy-to-instance provenance, or otherwise prove complete input/mutation coverage, before writing a history mapping. No draw consumes this index yet.

The index uses preallocated slots, four intrusive nodes per slot, and a power-of-two bucket array. Transform updates perform no allocation or registry scan. A short shared mutex protects CPU copies; it adds no GPU command, readback or wait. The default capacity follows the inspected 131,072-slot engine registry. This is a bounded CPU structure, not a measured 1 ms guarantee.

`CyberpunkObjects` observes the inspected registration/removal functions and the three-argument transform update. A successful registration is copied after the original method; removal invalidates before the original; a pose update is copied after the original while preserving its return value. Two copies and engine-counter comparison reject changing snapshots. Invalidated poses advance generation but retain enough lifetime state to recover on a later valid update. A successful registration with unreadable/changing pose can remain pending. Callback exceptions disable further observation rather than escape into the engine.

Startup checks the executable identity plus complete small-function fingerprints at RVAs `3a1ca4`, `294724`, and `1da094`. These are game-build-specific paths. They do not use driver instruction signatures, material names or fixed heap addresses. The observer and CPU storage remain process-resident. It does not acquire engine references or read game pointers from a later lookup.

## Validation

Run `build_geometry_objects.ps1` in an x64 Visual Studio developer PowerShell. The tests use synthetic owned state only.

- Registration tests cover exact packed-bit handling, repeated registration, coincident candidates, removal, address/slot reuse, stale callback rejection, mesh replacement, frame ordering and four-pose eviction.
- Thirty-two threads perform 4,096 updates/queries with no cross-object result, then remove all objects.
- `CyberpunkObjectCallbacks.cpp` calls the production callbacks with original-function substitutes and owned proxy layouts. It verifies preserved true/false results, actual post-update field copies, mixed-frame invalidation, recovery, retirement before the original removal, pointer reuse and rejection of an unreadable memory page.
- No game hook is installed by these tests. The production installer and its executable fingerprints compile in the test translation unit, but are not invoked.
- A complete Release x64 OptiScaler build including the source startup adapter passed. It has not been installed in the running game.

## Remaining acquisition work

The mesh registry covers a renderer family, not every in-world transparency route. The latest read-only inspection also found a transform-only path through mesh virtual slot `0x90`, then `0x29380ec` and `0x2906ec0`, which does not call the observed three-argument updater. That path is not yet instrumented. Other writes, procedural/particle identity, birth/death, mesh revision, instance upload provenance, actual camera/FG frame association and GPU ownership remain required. The index cannot authorize a draw while these gaps are unresolved.

Original per-object material capture, opaque visibility and intrinsic silhouette selection are separate requirements. In particular, a post-depth coverage cut is not automatically the object's own moving outline. See [GeometryShaders.md](GeometryShaders.md), [ObjectMotion.md](ObjectMotion.md) and [EngineGeometry.md](../EngineGeometry.md).
