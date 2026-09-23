# Support matrix by vertex factory and material family

- Created: 2026-09-14
- Updated: 2026-09-23
- Status: shader-level coverage of the transparent VS inventory by the graft catalog, plus the in-game observations recorded in the workspace `research/ACTIVE.md`. A validated record means the grafted VS passed the offline checks; it is not in-game quality evidence unless a result is cited.
- Applied: catalog `GGRAFT02` with 641 records, `index.bin` SHA-256 `52e45006ddb8ce809619a73c59a1876192044171cc756617175127fa3c211265`, for builds from `592de369b`. Default `GraftClassMask` 1: class 2 records are off unless enabled (`research/ACTIVE.md:128`).
- Deprecated: no. This replaces the 2026-09-14…17 matrix (vertex history, grouped-array mapping and the plugin owner scan); that version is in git history.
- Scope: transparent vertex shaders of the base shader cache: 250 with a native velocity-VS twin and 445 without. HUD excluded.

## How a draw is covered

The route is decided per pipeline from the SHA-256 of its original VS, and per draw from its instance layout. See [README.md](README.md#2-capture-the-graft-catalog).

| Route | Previous clip | When |
| --- | --- | --- |
| Root graft | engine MotionMatrix (b7 rows 24..26), filled by the engine for declared materials | VS has a validated root graft, single-instance draw, supply class admitted by `GraftClassMask` |
| Camera variant of a root graft | native previous view-projection × the VS's current world position | the same pipeline drawn as an array, grouped span or multi-instance draw |
| Camera-only record | same camera-only form | VS without a native twin, a twin refused by the factory rule, or a twin whose root graft did not validate |
| Not covered | engine MV and depth kept | no record (`GRAFT missing`), class not admitted (`class_disabled`), or packed rewrite rejected (`rejected`) |

Supply class is bit 0 root-only, bit 1 skinned (t10 bones), bit 2 preskinned (t9/b3). The catalog has class 1 and class 2 records only. The default mask 1 admits the 374 class 1 records (82 root grafts, 24 twin camera-only, 268 generic camera-only). The 267 class 2 records are off by default. A default of 3 was tried on DLL `0c5bb34e` and withdrawn: skinned NPC hair and glasses delivered about 77 px where the engine had about 3.5 px (`research/ACTIVE.md:128`).

## Coverage by vertex factory

Counts are unique VS per factory. "c1" and "c2" give the supply class. Sources: `glass-native-material-v1/native-grafted/index.json` (fields `status`, `camera_status`, `families`, `camera_errors`) joined with the exported `artifacts/glass-grafts/Glass/grafts/index.bin` (supply class, root/camera outputs). They were cross-checked against the per-family totals reported for commits `d01e62d67` (generic camera-only) and `2bef442ff` (factory rule). Twin rows carry one factory each. Eight unsupported generic VS list 2 to 5 factories and are counted under each, so the unsupported column sums to 76 for 54 VS.

| Vertex factory | Root graft (+ camera variant) | Camera-only: twin refused or root not validated | Camera-only: no twin | Unsupported | Unsupported reasons |
| --- | --- | --- | --- | --- | --- |
| MeshStatic | 50 c1 | 0 | 54 c1 | 17 | no `SV_Position` 4, multiple stores to one output 4, two-stage projection 3, vertex collapse reads current VP 3, no current VP rows 2, other non-VP clip 1 |
| MeshStaticVehicle | 19 c1 | 0 | 17 c1 | 9 | no `SV_Position` 4, two-stage projection 3, multiple stores 2 |
| MeshSkinned | 42 c2 | 5 c1 (factory) | 3 c1, 25 c2 | 9 | no `SV_Position` 4, two-stage projection 3, multiple stores 2 |
| MeshExtSkinned | 1 c1, 35 c2 | 5 c1 (factory) | 3 c1, 21 c2 | 9 | no `SV_Position` 4, two-stage projection 3, multiple stores 2 |
| MeshSkinnedVehicle | 22 c2 | 3 c1 (factory) | 2 c1, 17 c2 | 9 | no `SV_Position` 4, two-stage projection 3, multiple stores 2 |
| MeshSkinnedSingleBone | 9 c2 | 1 c1 (factory) | 3 c2 | 0 | |
| MeshSkinnedLightBlockers | 1 c2 | 0 | 0 | 0 | |
| MeshExtSkinnedLightBlockers | 1 c2 | 0 | 0 | 0 | |
| GarmentMeshSkinned | 1 c1, 17 c2 | 5 c1 (root not validated) | 3 c1, 26 c2 | 1 | two-stage projection (`flat_fog_masked`) |
| GarmentMeshExtSkinned | 1 c1, 17 c2 | 5 c1 (root not validated) | 3 c1, 27 c2 | 1 | two-stage projection (`flat_fog_masked`) |
| GarmentMeshSkinnedLightBlockers | 0 | 0 | 2 c1, 2 c2 | 0 | |
| GarmentMeshExtSkinnedLightBlockers | 0 | 0 | 2 c1, 2 c2 | 0 | |
| MeshDestructible | 2 c1 | 0 | 9 c1 | 0 | |
| MeshDestructibleSkinned | 1 c1 | 0 | 1 c1 | 0 | |
| MeshProcedural | 0 | 0 | 9 c1 | 4 | multiple stores 3 (`cyberparticles`), no current VP rows 1 |
| MeshSpeedTree | 0 | 0 | 1 c1 | 0 | |
| MeshProxy, MeshWindowProxy | 1 c1 each | 0 | 0 | 0 | |
| Debug | 2 c1 | 0 | 0 | 0 | |
| Decal | 0 | 0 | 1 c1 | 0 | |
| DrawBuffer | 3 c1 | 0 | 2 c1 (`ui_panel`, `ui_text_element`) | 0 | |
| Fullscreen | 0 | 0 | 0 | 2 | screen space: clip from the b1[48] viewport terms only |
| ParticleBilboard | 0 | 0 | 29 c1 | 2 | non-VP clip (`blackwall_horizon_cover`) |
| ParticleScreen | 0 | 0 | 1 c1 | 13 | screen space 9, non-VP clip 4 |
| ParticleBeam, ParticleFacingBeam | 0 | 0 | 13 c1 each | 0 | |
| ParticleFacingTrail, ParticleTrail, ParticleSphereAligned | 0 | 0 | 16 c1 each | 0 | |
| ParticleMotionBlur | 0 | 0 | 17 c1 | 0 | |
| ParticleParallel | 0 | 0 | 20 c1 | 0 | |
| ParticleVerticalFixed | 0 | 0 | 15 c1 | 0 | |
| **Total (unique VS)** | **226** (82 c1, 144 c2) | **24** (c1) | **391** (268 c1, 123 c2) | **54** | screen space 11, two-stage projection 17, no `SV_Position` 6, multiple stores 7, collapse on current VP 3, no current VP rows 3, other non-VP clip 7 |

Notes on the columns:

- Factory rule (`factory_mismatch`, 14 VS): a skinned-factory target refuses a twin of another factory whose previous graph is root-only. All 14 are `cloak`, `cloak_v2`, `optical_camouflage` and `cloak_*_forward` materials. Before the rule, 18 skinned or garment targets took root grafts from MeshStatic twins; NPC glasses then showed 6 px of wrong motion with a still camera (`research/ACTIVE.md:114`). Four of the 18 keep a root graft from a same-factory twin: three `hair_hideable` VS (`175cf87d`, `17a6c38a`, `78de9939`) and `c079a2ca` (MeshDestructibleSkinned, twin `361e9c9b`) (`native-grafted/index.json` fields `status`, `native_sha256`).
- Root not validated (10 VS, GarmentMeshSkinned 5 and GarmentMeshExtSkinned 5, also `cloak`/`optical_camouflage` materials): all fail with `missing native resource contract`; the missing pair is the preskinned t9 SRV and b3 constant buffer (`native-grafted/index.json` field `errors`, `EngineMotionSupply.md:639-640`). Their camera variants validate.
- The 8 multi-factory unsupported VS come from `all-transparent-vs` .ll files (`native-grafted/index.json` field `source`). Their factories are read from `scope-shader-catalog.json` (`tools/graft_native_motion.py:749-752`); they are `glass_scope` variants and are not in the base technique catalog. Six have no `SV_Position` output and two have multiple stores to one output component.

## Coverage by material family

Material names come from `glass-native-material-v1/all-cache-techniques.json` (transparency-route techniques of each VS). A VS used by several materials counts in each row, so the rows overlap. "Camera-only" includes both camera-only columns above.

| Material family | VS | Root graft | Camera-only | Unsupported |
| --- | --- | --- | --- | --- |
| `glass` | 26 | 11 (2 c1, 9 c2) | 15 (2 c1, 13 c2) | 0 |
| `glass_onesided` | 13 | 11 (2 c1, 9 c2) | 2 c2 | 0 |
| `glass_flat_twosided` | 13 | 11 (2 c1, 9 c2) | 2 c2 | 0 |
| `glass_blendable`, `glass_spread`, `glass_window_rain` | 10 | 6 (3 c1, 3 c2) | 4 (1 c1, 3 c2) | 0 |
| `frosted_glass`, `frosted_glass_curtain`, `frosted_glass_transition` | 17 | 15 (4 c1, 11 c2) | 2 c2 | 0 |
| `ver_mov_glass` | 3 | 2 c1 | 1 c1 | 0 |
| `alphablend_glass` | 13 | 3 (1 c1, 2 c2) | 9 c1 | 1 (screen space) |
| `vehicle_glass*` | 16 | 8 (4 c1, 4 c2) | 8 (4 c1, 4 c2) | 0 |
| `glass_scope` | 10 (+8 uncatalogued) | 0 | 0 | 10 two-stage projection; the 8 uncatalogued: no `SV_Position` 6, multiple stores 2 |
| `transparent_liquid`, `transparent_liquid_notxaa` | 35 | 14 (4 c1, 10 c2) | 19 c1 | 2 (screen space) |
| `fillable_fluid_vertex`, `fluid_mov`, `ice_fluid_mov`, `sq021_glass_fluid` | 7 | 2 c1 | 5 (3 c1, 2 c2) | 0 |
| `global_water_patch`, `water_plane` | 4 | 1 c1 | 3 c1 | 0 |
| `hologram*`, `holo_*`, `simple_hologram`, `holographic_waterfall` | 59 | 9 (4 c1, 5 c2) | 50 (23 c1, 27 c2) | 0 |
| `particles_*`, `presimulated_particles`, `particle_noise3d`, `beam_particles` | 41 | 10 (4 c1, 6 c2) | 30 (25 c1, 5 c2) | 1 (screen space) |
| `cyberparticles_*` | 12 | 0 | 5 c1 | 7 (multiple stores 5, no current VP rows 2) |
| `simple_fog`, `fog_laser`, `scan_fog`, `braindance_fog` | 31 | 8 (3 c1, 5 c2) | 21 (17 c1, 4 c2) | 2 (non-VP clip 1, no current VP rows 1) |
| `flat_fog_masked`, `flat_fog_masked_notxaa` | 7 | 0 | 0 | 7 (two-stage projection) |
| decals (`decal*`, `mesh_decal*`, `simple_emissive_decals`) | 17 | 2 c1 | 14 (11 c1, 3 c2) | 1 (screen space) |
| world UI (`ui_panel`, `ui_text_element`, `ui_default_*`) | 2 | 0 | 2 c1 | 0 |
| `cloak*`, `optical_camouflage` | 62 | 36 (9 c1, 27 c2) | 26 (24 c1, 2 c2) | 0 |
| `hair*`, `eye_shadow*`, `blackwall_blendable_eye_wet` | 38 | 34 (12 c1, 22 c2) | 4 c2 | 0 |
| screen effects (`screen_*`, `*screen_glitch`, `world_to_screen_glitch`, `cybermask*`) | 56 | 12 (4 c1, 8 c2) | 41 (28 c1, 13 c2) | 3 (screen space) |

## Observed in game

All results are from 2026-09-23 and come from the workspace `research/ACTIVE.md`. The README table gives the raw-data paths.

| Object | Route | Result | Source |
| --- | --- | --- | --- |
| Glass/cup table (instanced arrays) | camera variant | before the camera variant: 0 substitutions; after: all array draws (`array_draws=51,718`); still: delivered p50 0.28 px vs engine 0.33 px; moving: 67 vs 67 px | ACTIVE.md:52, 78 |
| Mezzanine glass railing, far background | twin grafts (`9754c134`, before the generic camera-only records) | generated frames A-B-A-B: mod off smears the glass pattern over the truss and doubles pillar edges; mod on keeps them in place and sharp | ACTIVE.md:95 |
| Quest icon "!" (DrawBuffer `ui_panel`), with railing slats and a lamp | icon: camera-only record (no twin); slats and lamp: not recorded per object | generated frames A-B-A-B (`9364be81`): mod off doubles the icon and smears or doubles slats and lamp; mod on keeps all three single and sharp | ACTIVE.md:113 |
| Glass family, decals, railing panes after the generic camera-only grafts | camera-only | `GRAFT camera_only=183`; substituted area 127k → 508k px; still residual 0.04–0.08 px, >1 px ≤0.7% | ACTIVE.md:112 |
| Songbird ceiling and chandelier | twin grafts | fast rotation up to 127 px/frame (`585b8df5`): residual mean 0.08–0.25 px; slow rotation 18–25 px/frame (`b9cf9e82`): 0.06–0.13 px | ACTIVE.md:51, 87 |
| NPC eyewear, hair and head attachments | root graft from a MeshStatic twin (before the factory rule); skinned class 2 root graft (mask 3) | 6–82 px wrong motion (`585b8df5`); 6 px (`9364be81`); ≈77 px vs engine ≈3.5 px with mask 3 (`0c5bb34e`); region gone with mask 1 (`580b24ad`, `class_disabled=348~367`) | ACTIVE.md:53, 114, 128 |
| Glass in front of opaque characters | any, with the occlusion test | NPC pixels in front of glass keep the engine value; residual mean 0.8–4.4 → 0.3–1.1 px | ACTIVE.md:79 |

Remaining misses: in the session-7 reclassification (`9364be81`), 8 of 16 VS had class 2 grafts blocked by mask 1 and 8 were outside the transparent inventory (`research/ACTIVE.md:122`). In the integration build (mask 1), 14 VS stayed unsubstituted: 13 outside the transparent inventory (blended decal 11, debugdraw 1, unclassified 1) and one screen-space particle (`42531526`) without generic support; no further graft candidates (`research/ACTIVE.md:129`).

## Runtime limits that apply to every family

- Motion beyond ±128 px per frame is not recorded (11-bit, 1/8 px). The pixel keeps the engine value. A fast 360° pan at 128 px/frame fell back for most substituted pixels (`research/ACTIVE.md:87`).
- Camera-only records carry camera motion only: independently moving array elements, camera-facing rotation of billboards and particles, and icon anchor motion are not included. This is the engine's own convention for those draws (`research/ACTIVE.md:77`, `108`).
- Coverage-only PS variants record opacity 0. Their interior keeps the engine value unless the threshold is 0; only the boundary takes the object's motion.
- An opaque surface nearer than the record keeps the engine value (occlusion test).
- `SkipFartherThanMeters` (default 0) and `ComposeRows` (default 240, must cover the render height) can exclude pixels by configuration.

## Not covered and why

| Gap | Size | Reason | Source |
| --- | --- | --- | --- |
| Generic VS without a camera-only graft | 54 VS | clip not a per-vertex VP multiply of a world position: screen space, two-stage projection, no `SV_Position`, multiple stores, collapse on current VP, other | `native-grafted/index.json` `generic_camera.errors`; `research/ACTIVE.md:107` |
| Preskinned root supply (t9/b3) | 10 garment VS | no validated preskinned graft; camera-only instead | `native-grafted/index.json` `status=unsupported` |
| Non-inventory VS seen in game | 8 VS | decal highlights, terrain, wireframe, UI depth composition, uncatalogued | `research/ACTIVE.md:122` |
| Per-element motion of arrays | all array draws | the engine keeps no per-element previous transform | `research/ACTIVE.md:77`; `EngineMotionSupply.md:547-554` |
| Skinned (class 2) records | 267 records, off by default | [INFERENCE, ACTIVE.md:128] the transparent pass lacks the velocity pass's previous skinning supply (previous `INSTANCE_SKINNING_DATA` offset, previous t10 bones) | `research/ACTIVE.md:128` |
| Not yet observed in game | vehicle glass, particles, smoke, holograms, liquids, destruction, procedural deformation | records exist for most; no in-game result | `research/requirements-and-evidence-20260923.md` §5 |
