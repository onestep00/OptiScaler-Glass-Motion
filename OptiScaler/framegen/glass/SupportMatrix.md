# Support matrix and current limitations

- Created: 2026-09-14
- Status: implementation stage complete for the packed object-motion path; live FG verification pending
- Scope: every non-HUD transparent family named in the objective, the implemented evidence for each, and the items that are still unsupported
- Note: `Compatibility.md` is the 2026-09-11 evidence record and still says the capture owner is unimplemented. Read this file for the current state.

## Required behaviour and where it is implemented

| Requirement | Implementation | Evidence |
| --- | --- | --- |
| Object motion per transparent pixel | `PackedMotionCapture` admits a draw, the material PS is rewritten to write an 8-byte packed record (depth key, motion, opacity, object id) into a packed target | `PackedUavPreservation` fixture, `packed_material_gpu=1` |
| Boundary uses the object's motion | `ApplyObjectMotion`: `edge ? 1.0 : saturate(opacity * InteriorStrength)` | `PACKED_MOTION_GPU_OK exact_inner_edge=1` |
| Interior uses material transmittance with a user strength | same shader, `InteriorStrength` from the settings | `weighted_interior=1`, UI slider |
| Nearest surface wins on overlap | the rewrite uses `dx.op.atomicBinOp.i64` opcode 7 (unsigned max) on a depth-ordered 64-bit key, so the nearest surface keeps the record | `PackedMotionShader.h` packed store; needs a dedicated GPU fixture |
| Real object motion, not image estimation | keys come from engine proxy/mesh/slot/generation, view identity, array lifetime and original element index | `GlassMotionIdentity.cpp`, `PackedMotionCapture` rejection reasons |
| Same-frame original vs final comparison | `dump` writes composed and original MV/Depth as PPM plus paired samples | `PACKED_MOTION_DUMP_OK files=5`, `dump-1.txt` samples |
| Settings and status | the window shows edge width, interior strength, dispatch/substitute/trace/writeback/arraymap toggles, GPU ms, hook/join/capture/replacement counters, the coarse skip reasons, the N-1 history reuse counters, the identity reject split, the FG boundary misses and the array-mapping counters (published/hits/misses/out_of_range/evictions); the same values also come through the file channel `status` | `GlassSettings.cpp`, `SETTINGS_OK`, `glass-ctl.ps1 -Command status` |
| ~1 ms budget | compose pass measured offline at 0.2473 ms per 2560x1440 frame (copies included), 0.4167 ms with dump counters | `PACKED_MOTION_SCALE` |
| MO2/version.dll/DLSS-NR/MFG compatibility | mounted as the MO2 Root `dxgi.dll`; no driver-address or version pinning; MFG unlock untouched | deployment record `glass-module-deploy-20260914*` |

## Transparency families

| Family | Object motion source | Current state | Evidence / next check |
| --- | --- | --- | --- |
| Individual glass objects (cups, bottles, railings, windows) | proxy + mesh + slot generation + depth-target view | Implemented; live FG result unverified | `GEOMETRY_PACKED admitted>0`, `GEOMETRY_HISTORY hits` |
| The same objects inside an array (repeated cups) | the above plus array lifetime and original element index | Implemented: the module reuses the engine's original order when it is exposed, otherwise it consumes the plugin mapping | `GEOMETRY_IDENTITY no_element_index`, `GEOMETRY_PACKED_SPLIT unknown_resolve` |
| Grouped array updates (`ownerFlags & 0x2000`) | the render packet repacks element order, so the live plugin reads the engine's own element list (`proxy+0x70` owner scan, list at owner+0x18, count +0x3C, output start `proxy+0x114`) and publishes `GlassArrayMapping` | Implemented (plugin `31D25A0E`); live counters pending | `arraymap published/hits/misses/out_of_range`, plugin log `candidate_off=`/`entries=` |
| Liquid inside a glass | the container material's own draw | Implemented if its material produces a packed variant | `GEOMETRY_COMPILER` / `GEOMETRY_PACKED_ERROR` |
| Sunglasses and other skinned attachments | the attachment proxy transform; previous skinning reuses the original bone input | Implemented for rigid attachments; skinned detail unverified | packed variant presence per chunk |
| Particles, smoke, ribbons | particle element state | Not implemented | open task `particle-original-history` |
| Holograms, world icons, decals | their own material draws | Implemented if the material produces a packed variant | `GEOMETRY_CHUNKS missing=` |
| Vehicle glass | vehicle proxy and destruction state | Not verified | chunk histogram after driving |
| Garments, cloth deformation | original garment deformation input | Not implemented | `custom-deformation-input-review.json` |
| Destruction, procedural deformation | original deformation input | Not implemented | same |
| HUD | excluded by design | n/a | – |

## Which code gate decides each family

Coverage is decided per draw, not per scene: a transparent object receives object motion when its pipeline produces a packed variant and its draw resolves an identity. The gates are fixed in code, so a family is covered wherever it is drawn once those gates pass.

Pipeline gates (`GeometryPipeline.cpp` / `DxilVertexHistory.cpp`), per pixel shader:

| Gate | Effect |
| --- | --- |
| root signature conversion fails, or reserved capture registers collide | no packed variant |
| render-target count > 8, multisampling, geometry/hull/domain stages, stream output, non-triangle topology | no packed variant |
| material capture with a writable depth/stencil target | no packed variant |
| material blend unknown and blending disabled | no packed variant |
| shader model < 6.6 or no 64-bit shader ops (packed), no ROV (non-vertex-only) | no packed variant |
| rewriter rejections: branch-local final colour store, depth/stencil exports, multiple returns, side effects, missing required output | no packed variant |

Identity gates (`GlassMotionIdentity.cpp`), per draw:

| Gate | Counter | Effect |
| --- | --- | --- |
| owner proxy/slot missing | `no_owner` | element unresolved, no object motion |
| depth target, else first colour target, not resolvable | `no_view` (`no_view_state` / `no_view_unknown` / `no_view_descriptor`) | element unresolved |
| object lifetime serial missing | `no_lifetime` | element unresolved |
| span.count > 1 without the engine's original order and without a published array mapping | `no_element_index` | element unresolved |

Consequences per family: single glass, railings, windows, liquid, holograms, world icons, decals and vehicle glass only need the pipeline gates, so they are covered wherever their pixel shader passes. Repeated arrays additionally need the element index, which the engine's original order or the plugin mapping provides. Particles, smoke and ribbons are the family where the element-index gate fails today: their instanced draws have no original order and no published mapping, so they are rejected as `no_element_index` regardless of which scene shows them. Garments, cloth, destruction and procedural deformation pass both gates but their deformed vertices' previous-frame input is not connected to the vertex-history path, so their motion correctness is unverified.

## Current limitations

0. Offline full-population rewriter acceptance (2026-09-14, shader-level only): 680 of 688 transparent
   vertex shaders pass the vertex-history rewrite in both layouts (the other 8 are pixel shaders with a
   different shader model, not vertex candidates). Of 3592 unique transparent-route VS/PS pairs, 3212
   (89.4%) pass the production `packed-inplace-mapped` rewrite and validation; 380 are rejected:
   `Required material output missing` 209, `Branch-local final color store unsupported` 109,
   `Pixel depth/stencil exports require separate coverage validation` 62. Rejections cluster in
   `renderstage_distortion` 260, `renderstage_hair_alpha_accum` 62 and `renderstage_hologram_depth` 24.
   Evidence: `work/glass-decompile/rewrite-scan/`, `work/glass-decompile/pair-scan/`.
   The 380 rejects have 99 unique pixel shaders: 89 carry SV_Target0, four carry SV_Target2+, six have no
   output signature at all (50 pairs). By pair, 330 have a colour output and 50 are empty pixel shaders.
   `Required material output missing` 209 = colour 151 + empty 50 + target2+ 8; the 109 branch-local
   rejects are all `renderstage_distortion`; the 62 depth/stencil rejects are all
   `renderstage_hair_alpha_accum`. Evidence: `work/glass-decompile/reject-classification/`.
   This is shader acceptance, not identity resolution or live FG quality.
   With the coverage-only retry that shipped in `EA3956DC`, all 3592 pairs are accepted: 3212 keep the
   interior transmittance blend and 380 become coverage-only variants (boundary keeps the object's
   motion and depth, interior keeps the engine's own motion). The 380 fallbacks are exactly the set
   above. Evidence: `work/glass-decompile/pair-scan/pair-scan-summary.json`.
1. Grouped-array element order comes from the plugin's owner scan. A candidate is published only when its element count matches the object's array count, every entry is a valid source index, and no index repeats; otherwise the element stays unresolved instead of being guessed (`no_element_index`).
2. Pixel shaders whose final colour store sits in a branch are rejected by the rewriter; the rejection now logs the actual exit shape so the next session can classify it (`GEOMETRY_PACKED_ERROR`).
3. Materials without a packed pipeline variant are counted per draw chunk (`GEOMETRY_CHUNKS missing=`); the families behind the histogram are not yet enumerated.
4. The 2026-09-14 driver reset is not attributed yet. `probe` (dispatch only) and `apply` (input replacement) plus `trace=on` separate the two causes in one session.
5. `GlassMotion.dll` still reports `stage_seen=0`; families that need native declaration supply are not covered.
6. Live DLSS-G 2x/4x comparison, camera motion, object motion, static-object-with-moving-background, overlap, and spawn/despawn verification are pending.
7. The array element mapping has never been consumed in a live session. The plugin publishes it and the module counts `published/hits/misses/out_of_range/evictions`, but no run with the game attached has been recorded since the publication path was finished.
8. Plugin and module replacement require the game to be closed; the plugin auto-loads from the armed `plugin=load` request in the deploy folder. Unloading a loaded plugin in a live session is no longer used (it crashed twice before the vectored-handler removal and the in-flight drain were added, and the workflow is dropped).
9. Identity `no_view` rejects are split into `no_view_state`, `no_view_unknown` (render pass, bundle, more than 8 targets) and `no_view_descriptor`, but no fix for any branch is implemented yet.
10. The 18:36 session crashed inside the plugin probe when `proxy+0x70` was dereferenced as the group owner. Probes now validate readability first and the vectored handler is off by default; any future probe must keep that rule.
11. Live session 2026-09-14 19:43 (PID 50872, dxgi `7653DF77`): the module was healthy with
    `packed_ready=192`/`packed_rejected=23`, but `GEOMETRY_PARENT no_selection=73` left
    `pipeline_ready=0`, `object_capture=0` and `admitted=0`, `acquire_no_candidate=80`, `plugin loaded=0`.
    No draw was replaced, so no correction could be applied in that session. Earlier sessions reached
    `admitted=1012806` and `fg_frames=169`, but `host substitutions` stayed 0.
12. `OptiScaler.Glass.log` across 50 sessions: the pre-packed path substituted up to 1,573,140 FG inputs.
    In the packed sessions (27-47) `admitted` reached 1,012,806 and `fg_frames` 169, but `substitutions`
    stayed 0; only session 28 recorded one `Object MV inputs are reaching FG`. The packed FG input
    replacement has therefore never run in a live session. Sessions with `pipeline_ready=0` are short
    (`compiled` around 192) and look like menu/loading states, so the 19:43 snapshot must not be read as a
    world-scene admission failure.

## How to collect the pending evidence in one session

```
work\glass-live-tools\glass-ctl.ps1 -Command status
work\glass-live-tools\glass-ladder.ps1 -Stage map     # plugin load + arraymap publish/lookup checks
work\glass-live-tools\glass-ladder.ps1 -Stage gpu     # rows=1 -> writeback -> 240 -> 1440
work\glass-live-tools\glass-crash-report.ps1          # plugin log, module plugin lines, crash and GPU events
```

Read `dump-N.txt` for `dispatched/packed/edge/interior` pixels and the paired `mv` / `original_mv` samples, `GEOMETRY_HISTORY` for N-1 reuse, `GEOMETRY_CHUNKS missing=` for uncovered families, and the last `TRACE_*` line if the process disappears.

## Deployment state (2026-09-14)

| Artifact | Hash | Note |
| --- | --- | --- |
| `dxgi.dll` (resident module) | `7653DF77` | replaced only while the game is closed; compose copies are limited to the dispatched rows; window and `status` expose the N-1, identity and mapping counters |
| `Glass\glass-plugin.dll` | `5F84A6DA` | loaded on demand through the file channel (auto-load request armed); replacement requires the game to be closed (hot reload dropped) |
