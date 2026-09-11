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

- [tests/GeometryShaders.md](tests/GeometryShaders.md): Optional submission capability forwards actual queue/list/recording order to retained diagnostic modules. Independent GPU and legacy tests pass; not deployed, history/MV/FG incomplete.

- [tests/GeometryShaders.md](tests/GeometryShaders.md): Two-buffer GPU history passes 32 fixture frames without interframe CPU wait/copy; game ordering, dense boundary MV and FG remain incomplete.

- [tests/GeometryShaders.md](tests/GeometryShaders.md): Strict GPU N-1 guard rejects matching stale tags without copies or waits; independent original-rendering and mapped-capture checks pass. Not deployed; continuous game history and FG incomplete.

- [tests/SourceBootstrap.md](tests/SourceBootstrap.md): Diagnostic PSO cache avoids 63 repeated compilations in a 64-job live run. Continuous GPU history and FG remain incomplete.

- [tests/SourceBootstrap.md](tests/SourceBootstrap.md): Bounded two-slot shape sampling yielded two exact consecutive original-VS pairs. No continuous history, dense boundary MV or FG substitution.

- [tests/GeometryObservation.md](tests/GeometryObservation.md): Bounded observation and failed-material to vertex-only recovery pass independent checks without duplicate shader storage. Missing live mesh-chunk pipeline, host deployment and FG remain incomplete.

- [tests/SourceBootstrap.md](tests/SourceBootstrap.md): Mesh-scoped slots no longer all fill from the first chunk. Both chunks captured in 32 live frames; differing proxy identities remain separate. Complete outlines and temporal/FG remain incomplete.

- [tests/CoverageLayout.md](tests/CoverageLayout.md): Packed geometry rectangles preserve original-material coverage in independent GPU checks; runtime bounds producer and FG connection remain absent.

- [tests/Experiments.md](tests/Experiments.md): Explicit diagnostic selection accepts up to 64 instances, matching recorder capacity and enabling selected array-vertex capture.

- [tests/SourceSlots.md](tests/SourceSlots.md): Linked pair runs in current game; query healthy, 227 destructor events, zero new creations. Existing-object bootstrap and temporal MV/FG remain incomplete.

- [tests/GeometryShaders.md](tests/GeometryShaders.md): Native pixel capture produced 95 pixels/65 raster-edge pixels in a small partial chunk, visualized; original D32/color/occlusion regression passes. No full object silhouette or FG change.

- [tests/GeometryShaders.md](tests/GeometryShaders.md): Native PS clip-input motion passes independent perspective comparison; depth-writing preservation and live pixel capture still pending.

- [tests/GeometryShaders.md](tests/GeometryShaders.md): Live native pair captures 9,536 vertices; 47 consecutive pairs agree within 0.000153 px. No dense boundary raster or FG substitution; module unloaded.

- [tests/GeometryShaders.md](tests/GeometryShaders.md): Native clip-pair recorder preserves full current/previous W in 64-byte records; independent GPU/old regression pass. Built replaceable DLL is not deployed; no dense game boundary MV.

- [tests/GeometryObservation.md](tests/GeometryObservation.md): Replaceable DLL requests selected native vertex preparation on its worker; prepared entry reaches indexed capture owner in an independent test. No game deployment or native MV capture.

- [tests/GeometryShaders.md](tests/GeometryShaders.md): Vertex-only capture preserves depth-writing original PS/state in five independent GPU frames. Native cache/host admission and game previous inputs remain incomplete.

- [tests/GeometryObservation.md](tests/GeometryObservation.md): Exact original root serialization survives source mutation and DLL file roundtrip. Native auxiliary capture admission remains absent; no new game MV.

- [tests/GeometryObservation.md](tests/GeometryObservation.md): Census-only binding recorder DLL passes independent load/record/save/unload and address checks. Live engine buffer contents and boundary MV remain incomplete.

- [tests/GeometryObservation.md](tests/GeometryObservation.md): Module-facing root layout/slot callbacks pass real independent CBV binding checks. ABI draw 4/census 2/capture 3; live native MV inputs remain incomplete.

- [tests/GeometryShaders.md](tests/GeometryShaders.md): Native MV-target extraction validates on two recorded glass velocity PS variants and preserves arithmetic/discard. Native engine input binding, boundary selection and GPU/live rendering remain incomplete.

- [tests/GeometryShaders.md](tests/GeometryShaders.md): Pre-submit host seam forwards to geometry owner and passed 13 independent GPU callback pairs. Not deployed; GPU-resident N-1 history owner and dense MV remain incomplete.

- [tests/GeometryShaders.md](tests/GeometryShaders.md): Live retirement arrives 2-5 frames late; CPU-retired handoff is unsuitable for N-1 history. GPU-resident ordered history requires a production admission seam beyond the current post-submit diagnostic callbacks.

- [tests/GeometryShaders.md](tests/GeometryShaders.md): Generation 8 captured actual vertices and original-material coverage in the same draw (64 captures), visualized and unloaded. Dense boundary MV and FG integration remain incomplete.

- [tests/GeometryShaders.md](tests/GeometryShaders.md): Generation 7 captured actual VS positions plus draw-bound camera CB words in 64 snapshots and unloaded. Jitter semantics/view/topology and final MV/FG admission remain incomplete.

- [tests/GeometryShaders.md](tests/GeometryShaders.md): Original-PS vertex recorder captured 64 live frames and unloaded. Exact current VS outputs are available; jitter/view/history admission and object MV/FG remain incomplete.

- [tests/SourceSlots.md](tests/SourceSlots.md): Direct source-slot cache has bounded storage, constant-index lookup and CPU-verified domain/conflict rejection. Engine provenance and motion integration remain incomplete.

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
- [tests/GeometryObjects.md](tests/GeometryObjects.md): Runtime defaults to lifetime-only slots, eliminating unused pose-index storage/hashing. Registry and draw callback checks pass; reduction not deployed, new MV/FG incomplete.
- [tests/GeometryShaders.md](tests/GeometryShaders.md): Original shader history, separate masks, inactive-instance isolation/recovery and public PSO/root acquisition verified on an independent GPU. Worker/cache leases and mapped 54-DWORD linkage pass. Startup adapter added; draw replacement, deployment and FG application remain incomplete.

- [tests/AGENTS.md](tests/AGENTS.md): Standalone GPU ownership/timing and headless settings test boundaries.
- [tests/ObjectMotion.md](tests/ObjectMotion.md): Separate-object boundary weights and geometry MV reference. Synthetic inputs only; engine history and FG quality remain unverified.
- [tests/replay/BoundaryFG.md](tests/replay/BoundaryFG.md): Real FG boundary-width experiments, including unchanged captured Cyberpunk HUDless. No candidate passes full transparency quality requirements.
- `GlassLayerComposite.hlsl`: Experimental isolated source; moving layers and stationary transparency with independent background motion tested through actual FG. Explicit correction footprint preserves original output outside the admitted region. Runtime footprint/input acquisition, color-domain validation and integration remain incomplete; see README.md and tests/replay/README.md.
- The isolated compositor supports per-input valid regions and normalized endpoint offsets across different previous/current/intermediate/final sizes. Analytic GPU mapping tests and a controlled actual-FG comparison passed. This is not proof that arbitrary DLSS stages share a color domain or that the game host supplies these inputs.
- README.md also distinguishes installed static-world projection from possible engine object-motion inputs. Previous-transform and second-bone-state bytecode paths were found; validity at transparent draws remains unverified. Complete-material single-MV selectors failed the controlled 432-output FG comparison and were not adopted.
- A later read-only game trace established material and two NGX calls before the identified tone-map command in 22 submission batches. Pixel/color transfer remains unresolved. tests/StageReadback.h prepares native-size diagnostic copies, independently verified with padded regions and completion gates; it is not a game capture hook or production host integration.
