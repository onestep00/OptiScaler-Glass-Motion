# Native array source domains

- Created: 2026-09-12
- Updated: 2026-09-13
- Status: bounded source-range capture and dynamic placeholder layout decoder; temporal correspondence incomplete
- Deployment: new diagnostic loaded in PID 24404; record/save succeeded with zero array calls; stopped, no MV or FG substitution
- Deprecated: no
- Scope: upstream identities of instanced render arrays; not a material whitelist or complete world coverage

## New resident evidence

The compacting producer at RVA `0x579ab8` obtains its definition through
`0x57a1d4` and checked cast `0x57a1f4`. The cast reads type global `0x342de00`.
In PID 21760 this global identifies `worldInstancedDestructibleMeshNode`, size
0xE8. Both the entire cast body and the normal leaf accessor bytes matched the
owned executable. Repeated type-pointer and metadata reads agreed. The audit
read 480 bytes, wrote none and installed no hook. This is a current type/code
observation, not evidence of a captured node instance or temporal identity.

The definition accessor `0x579d78` reads the shared handle at definition+0x68,
first/count at +0x78/+0x7C, and shared data at buffer+0x30. Its normal path forms
32-byte source entries. The SDK identifies this range as `cookedInstanceTransforms`
with a `worldTransformBuffer` layout. The empty-range cold branch is not certified
by the normal-leaf comparison and must not be hooked from this evidence.

Local evidence lives in `work/glass-native-material-v1/`:
`audit_array_source_identity.py`, `array-source-identity-342de00.json`,
`callers-3cab98-3cbde0-579ab8-57b114.json` and
`callers-3a2038-3c8508-2276c30.json`. Function disassemblies remain local in
`work/glass-engine-identity-probe-v2/`; game bytecode is not distributed here.
The audited executable SHA-256 is
`a7de82945c03e041fc7339fcf9066224d98db2f5d80fea50f7947bb350a60991`.
RVAs are diagnostic evidence for that build, not portable production signatures.

## Packing occurs before the observed render indices

The six instruction-validated direct callers of the common array wrapper have
three source families. Indirect callers are not exhaustively certified.

| Producer RVAs | Source evidence | Required identity translation |
| --- | --- | --- |
| `0x3c8508`, `0x2276c30` | Previously typed `worldInstancedMeshNode`; parent loops partition its 48-byte shared transform range. Successful renderer handles can omit rejected groups. | Actual supplied span to parent source index; never renderer-handle ordinal. |
| `0x579ab8`, `0x57b114`, `0xa040c0` | All call the now typed destructible-definition accessor chain. `0x579ab8` compacts entries according to a source-indexed active bitset; `0x57b114` consumes dynamic 64-byte records with placeholders; `0xa040c0` converts baked 32-byte entries in order. | Preserve the baked/source domain before packing. Static and dynamic ordering must not be treated as interchangeable. Dynamic 64-byte record identity remains unproven. |
| `0x3a2038` | Its caller `0x36a7f4` obtains split ranges through `0x36a91c`/`0x36a9ec`. Fields +0x40/+0x44/+0x48/+0x50 match SDK `worldFoliageCompiledResource` version/populationCount/bucketCount/dataBuffer; the four-word span matches `worldFoliagePopulationSpanInfo`. | Preserve resource lifetime and population-range index. This type is inferred from layout and callers, unlike the resident RTTI-confirmed destructible type. Wind/deformation and live mutation remain separate checks. |

`CyberpunkInstanceSelection::originalIndex` currently refers to the renderer's
input array. It does not automatically mean the baked world's original element
after an upstream compaction. The source-slot history adapter must receive the
translated source domain, or invalidate the affected array history. Reusing the
same packed ordinal after an active-mask change would join different objects.

## The setter does not provide identity or automatic previous transforms

At `0x3cbde0`, the old count is retained for allocation decisions. Equal count
can reuse the allocation at proxy+0x108. The conversion at `0x3cc114` writes
each 48-byte output from the newly supplied span. Therefore unchanged allocation
address and count cannot establish unchanged element identity.

The optional proxy+0x158 branch also calls `0x3cc114` with that same new span.
This setter does not move the old +0x108 array into +0x158 as N-1 history. Other
writers/consumers have not all been certified; +0x158 must not be admitted as
previous motion on the basis of its existence or this update path.

## Implementation consequence

`GeometrySourceIndexMap` now composes the renderer's packed selection with a
linear or bitset-compacted original source domain. The default map occupies
524 bytes; lookup does not scan the mask, allocate storage or copy transforms.
The caller constructs it when the applied source layout changes. It does not
discover update application or owner lifetime. The same-count replacement test
also passes the translated IDs through the existing source slots and history
allocator: surviving elements keep their history while a new member receives
separate storage. The diagnostic decoder uses this map for mask/count validation.
`build_array_source_trace.ps1` now runs five tests, including `SourceIndexMap`.
This source change is not installed in the pinned game wrapper observer.

The subsequent moving capture and the still-missing array-owner bridge are
recorded in [Timeline.md](Timeline.md). Source addresses are reused with different
source tuples; recorded source metadata alone is not temporal identity.

Retain source identity metadata at the upstream update/creation event, before
enqueue and compaction. Compose it with the later render selection once in its
proven frame/view domain. Position changes alone must not reset a surviving
element's identity. Composition replacement, missing provenance or an unverified
static-to-dynamic transition must not reuse old history.

The bounded `GeometrySourceSlots` and `VertexHistoryCache` join remains usable,
but neither creates native lifetime evidence. A node ID alone still merges all
members of that node. Particle birth/death/reordering is a separate unsolved
source domain; these mesh-array findings do not cover particles.

The next native diagnostic should preserve the destructible source range and
selection before enqueue, distinguish its static/dynamic producer, and correlate
the resulting renderer indices under the same source lifetime. It must validate
the actual internal calling contract before installing a hook. Do not reenable
the quarantined range-accessor hook. Do not add per-frame full-array transform
copies, per-object screen textures or image matching to establish identity.

## Bounded caller-context source trace

`GLASS_ARRAY_SOURCE_TRACE` extends the existing standalone array-wrapper
diagnostic. A distinct profile magic, 0x49555036, requires the wrapper and all
eight caller/type/accessor bodies to match before installing its single wrapper
hook. It never installs the quarantined accessor hook. The diagnostic is not
linked into OptiScaler's production build.

`DiagnosticCallerContext.h` uses documented Windows `RtlCaptureContext` and
`RtlVirtualUnwind` on its own thread, with an eight-frame limit and stack bounds.
Only reconstructed nonvolatile registers are interpreted. It does not infer
volatile arguments. `DiagnosticArraySource.h` uses the audited call site to
select the original node register, reads bounded scalar range/handle/mask data,
and repeats those headers. It does not read pointed-to transform contents or
publish temporal generations. Foliage and dynamic-placeholder identities remain
explicitly incomplete. This stack walk is diagnostic-only, not the proposed
per-draw production acquisition cost.

Independent assembly fixtures preserve known caller registers across real C++
frames. Optimized and unoptimized builds recover all 300 register observations
in two unwind steps. The actual wrapper callback also captures changed selection
masks while retaining its original result. Other cases check unknown callers,
changed metadata, misaligned shared ranges, wrong handles and deliberately
unmapped transform storage. `build_array_source_trace.ps1 -OutputDirectory <path>`
builds four independent tests and the diagnostic DLL from an MSVC shell. It never
loads that DLL into a game.

The current game preflight matched 6,260 resident code bytes across nine ranges.
The staged observer SHA-256 is
`f0e7ab7cefcf91edf14f0ca262b6941c43b480cc6fbb16f0ddb63418d757819c`.
Load, Start and Save succeeded without a game restart. The third recording
captured 697 wrapper calls: 540 shared-range records and 102 baked destructible
selection records resolve to 2,686 source-element occurrences; 55 foliage records
provide only node provenance. All recovered caller contexts took two steps.
No malformed wrapper header occurred. The bounded sample contained no non-prefix
active mask, so it does not demonstrate an actual shifted compaction in the game.

Local files: `work/glass-array-source-live-v1/preflight.json` and
`capture-3/{array-sources.csv,updates.csv,source-index-mapping.json,analysis.json}`.
The reusable local analyzer is `work/glass-native-material-v1/analyze_array_source_trace.py`.
Address tuples and same-invocation source ranges do not prove cross-frame
lifetime, producer-to-draw frame matching or MV. Recording is stopped and the
forwarding DLL remains pinned; the process was responding after Save. No new
geometry MV was supplied to FG.

## Dynamic input ordinal layout, 2026-09-13

The dynamic producer `0x57b114` retains its original input count in RSI and its
node in RBX. Its loop advances the 64-byte input and source bit index together.
An active bit appends a converted transform; an inactive bit appends a placeholder
at `0x57b6bf`. Both branches advance to the same next ordinal. Thus this output
keeps holes, unlike the compacted baked producer `0x579ab8`. The callback thunk
`0x57b97c` computes the count from the supplied span divided by 64. The destruction
event path `0x8e8288` also indexes that span with the same event element index
used for the node's state bitsets. These are original instruction observations,
not a position-matching heuristic or cross-frame lifetime proof.

`ReadDiagnosticArraySource` now recognizes the dynamic ordinal layout only when
the recovered RSI count, definition range count and emitted 48-byte span count
agree, the count is at most 256, and an in-range active bit exists. It retains the
entire original mask. It performs at most four mask-word checks without allocating
or copying transforms. A compacted span, mismatched count, empty/inapplicable mask
or changing headers stays unadmitted. The local analyzer emits placeholder indices
separately; those entries must not acquire visible-object or N-1 history admission.
Definition lifetime, applied update ordering and renderer selection still need
verification before these ordinals can supply motion.

The actual diagnostic callback fixture now recovers RBX/RSI across two real
unwind steps. Correct counts pass; a changed caller count is rejected. Additional
scalar tests cover changed masks, compacted-count confusion, the 64/65-bit boundary,
out-of-range active bits and the 256-element bound. All five independent build
checks pass, including the original wrapper/compact/shared-range regressions.

Process 24404's nine required native function ranges matched 6,260 bytes. The
prepared diagnostic SHA `aaadecc3f2a01f735e20ba6b4b40c6b40c847726432c834fa6d1ebf3f209db5e`
loaded at its full path without restart. Start/Save returned zero, but the short
capture recorded zero calls, including zero filtered calls. `status.txt` confirms
recording=0; the forwarding hook stays pinned. This is no live dynamic-layout,
history or MV validation. A subsequent source-only optimization removed the
temporary index-map construction from the mask check; its final build also
passes and has not replaced the resident diagnostic. The production correction
and MO2 files remain unchanged.

Local evidence: `work/glass-array-source-live-v1/dynamic-live-p24404/preflight.json`,
`capture-1/{status.txt,analysis.json}` beneath that directory, and
`work/glass-array-source-live-v1/dynamic-layout-final-build/`.
