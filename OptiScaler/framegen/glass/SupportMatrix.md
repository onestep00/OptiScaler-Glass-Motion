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
| Settings and status | the window shows edge width, interior strength, dispatch/substitute/trace/writeback/arraymap toggles, GPU ms, hook/join/capture/replacement counters and the coarse skip reasons; N-1 history and the array-mapping counters (published/hits/misses/out_of_range) are exposed through the file channel `status` and the log | `GlassSettings.cpp`, `SETTINGS_OK`, `glass-ctl.ps1 -Command status` |
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

## Current limitations

1. Grouped-array element order comes from the plugin's owner scan. A candidate is published only when its element count matches the object's array count, every entry is a valid source index, and no index repeats; otherwise the element stays unresolved instead of being guessed (`no_element_index`).
2. Pixel shaders whose final colour store sits in a branch are rejected by the rewriter; the rejection now logs the actual exit shape so the next session can classify it (`GEOMETRY_PACKED_ERROR`).
3. Materials without a packed pipeline variant are counted per draw chunk (`GEOMETRY_CHUNKS missing=`); the families behind the histogram are not yet enumerated.
4. The 2026-09-14 driver reset is not attributed yet. `probe` (dispatch only) and `apply` (input replacement) plus `trace=on` separate the two causes in one session.
5. `GlassMotion.dll` still reports `stage_seen=0`; families that need native declaration supply are not covered.
6. Live DLSS-G 2x/4x comparison, camera motion, object motion, static-object-with-moving-background, overlap, and spawn/despawn verification are pending.

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
| `dxgi.dll` (resident module) | `BABB5E53` | replaced only while the game is closed |
| `Glass\glass-plugin.dll` | `31D25A0E` | loaded on demand through the file channel; replacement requires the game to be closed (hot reload dropped) |
