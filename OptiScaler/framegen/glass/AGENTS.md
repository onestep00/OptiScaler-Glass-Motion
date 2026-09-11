# Glass FG module rules

- Read this file and README.md completely before changing this module.
- Keep algorithms, shaders, game-specific identification and queue bookkeeping in this directory. Keep upstream host changes limited to explicit integration calls and the project import.
- Preserve existing ASI loading, MFG unlock, kernel selection and interpolation-count behavior. Do not duplicate those responsibilities here.
- Never identify transparent depth from dimensions or a resource address alone. Reject unsupported executable fingerprints, ambiguous candidates and stale or unordered snapshots.
- Resource ownership, GPU completion, command-list state restoration and resize/feature teardown must be verified before enabling runtime integration.
- Preserve the repository's PCH conventions. Do not add dependencies to pch.h for this module.
- Distinguish compilation, recorded-input replay, live runtime integration and visual acceptance. Do not label the current candidate a finished ghosting fix.
- Target all in-world transparency, including vehicle glass, eyewear, icons and particles. Identify geometry/coverage from verified rendering inputs, not an object/material whitelist. Do not classify world displays as HUD from names. Read EngineGeometry.md for current evidence and missing routes before engine-input changes.
- Update document metadata and this index when adding or changing documentation status.

## Document index

- [tests/Experiments.md](tests/Experiments.md): Read-only live evidence shows count=1 can belong to a 40-instance proxy. Source rejects array proxies from proxy-only history admission; not deployed. Original group indices remain the next acquisition task.

- [tests/Experiments.md](tests/Experiments.md): Live same-process DLL load/capture/unload verified. Draw-instance regions match original material references, including cups/railings. Persistent multi-instance identity, actual MV and FG remain incomplete.

- [tests/Experiments.md](tests/Experiments.md): Raw census, selection and actual queue/in-flight replacement pass independent checks. Common host fc151fc staged in MO2 with backups; user launch/live validation pending. Game contours/MV/FG remain incomplete.

- [tests/GeometryTargets.md](tests/GeometryTargets.md): Public RTV/DSV creation/copy and OM-time snapshots preserve resource/subresource metadata without retaining textures. Independent hooks/ABI/module outputs pass; not deployed. Full draw census and live contours/MV/FG remain incomplete.

- [tests/Experiments.md](tests/Experiments.md): Event control, two independently prepared DLL generations, original color, admission rejection and unload pass GPU checks. Opt-in game startup compiles, not deployed. Live contour/MV/FG and in-flight owner replacement remain incomplete.

- [tests/GeometryDraws.md](tests/GeometryDraws.md): Actual mesh chunk range decoder added. Diagnostic draw metadata records counts/bindings, without claiming topology or view identity. New metadata is not deployed.
- [tests/GeometryShaders.md](tests/GeometryShaders.md): Same-process requests and same-draw surviving/contributing references detect deliberate missing mapping on an independent GPU. Original shader/recording artifacts retained locally; live target coverage pending.

- [tests/GeometryShaders.md](tests/GeometryShaders.md): Live recorder saved seven nearly empty captures (four pixels total). Readback works; usable target coverage, object MV and FG substitution remain unverified/incomplete.

- [tests/GeometryShaders.md](tests/GeometryShaders.md): Latest source adds object bit coverage and single-draw hook insertion, independently GPU verified. Live capture owner, lifetime/frame linkage and FG substitution are still absent.

- [Compatibility.md](Compatibility.md): Relocatable engine discovery, indirect signatures, MRT admission and UI/log evidence pass independent checks. Fresh-process validation pending; production object capture/FG substitution remain absent.

- [tests/GeometryDraws.md](tests/GeometryDraws.md): 32054cc live packet/public draw observation verified; active MRT/root joins failed and new FG input remains absent. Source fixes and relocatable discovery pass independent checks; fresh-process validation pending.
- [README.md](README.md): Active experimental module. MO2 deployment, controls and timer verified; cup ghosts remain. Name-independent PSO census identified missing variants. Complete 143-draw material-span F/T/U reconstructs actual early HDR color within 0.219% relative error, with exact outside coverage. B retains refraction. Later color/temporal transport, runtime integration and visual acceptance remain incomplete.
- [EngineGeometry.md](EngineGeometry.md): All-world 32-route inventory and engine evidence. CPU lifetime/index and startup observer now implemented, but unique pose candidates do not prove draw ownership. Live frame/instance linkage, missing mutation routes and procedural/particle coverage remain incomplete.
- [tests/GeometryObjects.md](tests/GeometryObjects.md): Generation/lifetime index and production callbacks pass independent tests; source startup hook compiles. No game deployment, verified draw ownership or new FG input replacement.
- [tests/GeometryShaders.md](tests/GeometryShaders.md): Original shader history, separate masks, inactive-instance isolation/recovery and public PSO/root acquisition verified on an independent GPU. Worker/cache leases and mapped 54-DWORD linkage pass. Startup adapter added; draw replacement, deployment and FG application remain incomplete.

- [tests/AGENTS.md](tests/AGENTS.md): Standalone GPU ownership/timing and headless settings test boundaries.
- [tests/ObjectMotion.md](tests/ObjectMotion.md): Separate-object boundary weights and geometry MV reference. Synthetic inputs only; engine history and FG quality remain unverified.
- [tests/replay/BoundaryFG.md](tests/replay/BoundaryFG.md): Real FG boundary-width experiments, including unchanged captured Cyberpunk HUDless. No candidate passes full transparency quality requirements.
- `GlassLayerComposite.hlsl`: Experimental isolated source; moving layers and stationary transparency with independent background motion tested through actual FG. Explicit correction footprint preserves original output outside the admitted region. Runtime footprint/input acquisition, color-domain validation and integration remain incomplete; see README.md and tests/replay/README.md.
- The isolated compositor supports per-input valid regions and normalized endpoint offsets across different previous/current/intermediate/final sizes. Analytic GPU mapping tests and a controlled actual-FG comparison passed. This is not proof that arbitrary DLSS stages share a color domain or that the game host supplies these inputs.
- README.md also distinguishes installed static-world projection from possible engine object-motion inputs. Previous-transform and second-bone-state bytecode paths were found; validity at transparent draws remains unverified. Complete-material single-MV selectors failed the controlled 432-output FG comparison and were not adopted.
- A later read-only game trace established material and two NGX calls before the identified tone-map command in 22 submission batches. Pixel/color transfer remains unresolved. tests/StageReadback.h prepares native-size diagnostic copies, independently verified with padded regions and completion gates; it is not a game capture hook or production host integration.
