# Native array source domains

- Created: 2026-09-12
- Updated: 2026-09-12
- Status: source classification and bounded resident RTTI audit; runtime correspondence incomplete
- Deployment: no new game hook, DLL replacement, motion output or FG substitution
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
