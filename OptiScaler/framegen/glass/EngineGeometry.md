# Engine geometry acquisition and transparency coverage

- Created: 2026-09-11
- Updated: 2026-09-11
- Status: shader/creation and CPU lifetime components implemented; live geometry producer incomplete
- Deployment: acquisition build 32054cc staged in MO2 Root; fresh-process validation pending. Its FG correction still uses static-world depth projection
- Deprecated: no
- Scope: all in-world transparent rendering in Cyberpunk 2077; HUD excluded by verified rendering provenance

## Coverage requirement

Cups and railings are test scenes, not an object whitelist. Vehicle windows, eyewear, attachments, moving world icons, holograms, animated meshes, particles and effects belong to the target scope. Material names, object names and a static/dynamic label cannot decide whether correction applies. A world-space display can use a texture produced by a screen-space/UI pass. The final world surface must not be excluded merely because a shader name contains `ui`.

The base-cache inventory found 32 vertex-factory routes among 7,506 candidate techniques joined to known RGB-blended template passes. This is an overinclusive investigation set, not a count of transparent objects or proof of complete support. Lighting and auxiliary passes require rejection based on their actual destination and contribution to FG color. Another 39 composition-element records remain unresolved. Active overrides and runtime-only shader variants require separate observation.

| Geometry family | Observed route names or variants | Required motion source |
| --- | --- | --- |
| Rigid meshes | MeshStatic, MeshStaticVehicle, MeshProxy, MeshWindowProxy | Current/previous object transform, matching camera state and stable render-instance identity |
| Skinning and clothing | MeshSkinned, MeshExtSkinned, garment variants, MeshSkinnedSingleBone, MeshSkinnedVehicle | Current/previous bone or deformed-vertex data in addition to object/camera motion |
| Destruction | MeshDestructible, MeshDestructibleSkinned | Per-piece identity, previous transform/deformation and creation/removal handling |
| Procedural geometry | MeshProcedural, MeshSpeedTree, any PreSkinned variant | Previous actual generated positions or all time-dependent inputs needed to reproduce them |
| Particles | Billboard, parallel, motion-blur, sphere-aligned, vertical-fixed, screen, trail and beam families | Per-particle identity/history, camera-facing basis, size/rotation and topology/birth/death changes; emitter motion alone is insufficient |
| World displays and projection | Decal, DrawBuffer, Fullscreen, Debug and composition elements | Proven world contribution plus element/projection/texture-animation correspondence; a quad boundary is not the visible icon contour |

The names in this table describe investigation routes. They are not a production name-based admission list. `MeshStatic; PreSkinned` must not be treated as rigid. Morph targets, wind, procedural displacement, cloth, UV animation and time-dependent material coverage must be included where the actual shader uses them. No route is marked runtime complete yet.

## Evidence acquired in the current game process

An observer verified actual rigid-transform history for 2,050 mesh proxies. All 15,746 valid previous-transform slots matched the pre-update packed transform exactly. Sequential updates agreed in 13,860 pairs. The observed engine counter advanced once per history cycle; it is not yet correlated with the FG input token. Absent history was not filled with an assumed identity transform.

Matching current packed transforms and mesh quantization was strengthened by the real mesh registry and GPU buffer/chunk metadata. Eighteen selected draw samples matched their observed vertex-stream addresses and index counts. There were 20 candidate chunks because two draws had two possible chunks; the index-buffer address is still needed to disambiguate those cases. A resource address, draw order or matching location alone is not object identity. The registered slot must be protected against destruction/reuse, and the geometry must belong to the same rendered frame.

The engine's skinning state retains current and previous byte offsets with a 48-byte bone stride. Its GPU offset includes an initial identity matrix that is absent from the CPU staging allocation. Ignoring this 48-byte difference caused an earlier diagnostic mismatch. After correcting the translation, five sampled skinning objects produced 285 consecutive pairs whose previous bytes exactly matched the prior current bytes. All 290 sampled current and previous states also matched the GPU buffer's CPU mapping. Ten snapshots with changing headers/counters were rejected. This is CPU snapshot evidence, not GPU-execution or FG-frame proof.

A bounded, read-only descriptor observer traced the engine bone SRV through actual descriptor copies to VS t10 in 9,996 draws using 12 audited transparent PSOs. It added no GPU command and changed no render input, then disabled its hooks. This establishes binding provenance for that set; it does not verify every instance offset, bone influence, garment correction or moving transparent object. A provider-export observer recorded no FG calls in this window, so it did not establish FG frame correlation or prove that FG was off.

Raw engine memory, shader bytecode and diagnostic DLLs remain local and are not included in this repository. The local evidence is indexed by workspace `docs/glass-engine-object-motion.md` and the corrected skinning, mesh GPU binding and geometry-link probe reports.

## Original shader output reuse

`DxilVertexHistory.cpp` now preserves the original VS output while recording actual current positions and reading prior positions with generation/frame guards. The separate material rewriter preserves original material discard/alpha and emits geometry MV. Independent GPU checks passed for deformation, camera motion, perspective, changing generations, and stale frames; original position/color/coverage are preserved. See [the detailed test contract](tests/GeometryShaders.md).

This avoids reimplementing skinning/wind/material deformation math for an admitted shader, but stable object/vertex identity and live frame ordering are still required. It does not make particle indices stable or infer an invisible object's prior vertices. The rewriter has been added to the module build; no production host call or game deployment of this path exists yet.

## Remaining runtime contract

The [object-lifetime implementation](tests/GeometryObjects.md) now records registration/removal generations and bounded recent pose candidates. Its production callbacks preserve original return values and pass independent layout/lifetime tests. A source startup call installs the audited observation paths after executable checks; this has not been deployed. A matching mesh/pose is not sufficient draw ownership proof. The transform-only virtual-slot `0x90` path and procedural/particle families remain outside the current updater observation. Direct render-instance provenance is still required before this index can supply shader history identities.

[Direct render-packet acquisition](tests/GeometryDraws.md) now follows the engine's proxy-slot and transform-array indices through instance append/upload/flush. Production callback tests preserve distinct coincident objects and their order through rigid and skinned batches. The public draw/root consumer passes independent GPU tests and is staged with the startup adapter in build 32054cc. Live game validation is pending. This direct route does not need a complete pose-mutation search to identify a mesh packet; it still needs registered lifetime validity, stable sub-instance/topology identity and the actual shader/GPU/FG connection.

1. Resolve each draw instance to a live engine object/generation and mesh chunk, including several chunks per object and several instances per draw.
2. Associate its current/previous transforms and deformation data with the exact color/MV frame consumed by FG. Reject missing history, reused slots, camera cuts, topology changes and ambiguous associations.
3. Retain the actual material's surviving coverage before combining objects. Respect alpha/discard and time-varying texture contours; do not turn a billboard quad or an opaque occlusion cut into the object's silhouette.
4. Determine visible boundaries for each transparent object, including objects behind another transparent layer. A merged mask or nearest ID alone loses those outlines.
5. At an admitted edge, select that object's full motion. Inside, use `saturate(alpha * gain + bias)` to blend with background motion. The alpha here must be the verified material composition coefficient, not an invented opacity estimate. Preserve independent transmitted-background motion.
6. Map each input's own valid region/resolution to FG coordinates. Do not independently filter IDs, depth and motion across objects. Conflicting motions at one output sample remain unresolved unless an explicit layer policy handles them.

The existing `GlassObjectMotion.hlsli` and independent raster reference cover geometry arithmetic and scalar weights. They do not implement the live producer above. New controls must not claim to operate on engine geometry until that path is connected and observed at FG entry.

## Cost and integration boundaries

Reuse verified GPU bone/vertex inputs in the same ordered rendering path where possible. Diagnostic CPU reads of mapped GPU data, full descriptor tracing, registry scans and per-object full-screen targets are not the proposed continuous runtime implementation. Avoid CPU/GPU waits and image-wide optical flow as a default acquisition path. Actual resource ownership, state, queue ordering and frame correspondence must be established before reuse; an existing binding alone does not provide those guarantees.

The acquired history can reduce duplicated estimation work, but it does not establish a 1 ms bound or complete coverage. Mesh motion, original material coverage, procedural/particle history and input substitution must each be verified. Keep the game adapter within this module and preserve upstream ASI/MFG unlock behavior.
