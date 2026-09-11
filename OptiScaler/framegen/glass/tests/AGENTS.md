# Standalone module tests

- Read the parent AGENTS.md and README.md before changes.
- ObjectMotion.cpp / check_object_motion.py validate synthetic geometry and per-object boundaries; read ObjectMotion.md. Fixture IDs/history are not engine data. Conflicting edges remain unresolved; no FG quality or production cost claim.
- These executables use an independent D3D12 device or a fresh test settings directory. Never attach to a game or point the settings test at an installed directory.
- GpuResources.cpp checks real GPU copies, cross-queue completion and timing. Its measured copy duration is not timer overhead or correction performance.
- Settings.cpp includes the production settings implementation with a test-only DLL-path provider. ImGui uses stb fonts in this headless test; production retains FreeType. Vertex generation is not visual acceptance of the game menu.
- All settings test directories must be new. Build artifacts belong outside source control.
- MaterialCaptureBlend.cpp checks source color, RGB transmission and surviving-pixel coverage for five blend families. It includes discarded pixels and unit transmission with nonzero source color. This is not game layer extraction or FG quality validation.
- LayerComposite.cpp runs the isolated compositor on caller-supplied float4 files. Build and contract checks are documented in replay/README.md. It never captures or changes a game, and it is not part of the default suite that has no Python dependency.
- The compositor runner accepts independent per-input dimensions and valid regions. Keep its 36-constant layout and normalized-offset interpretation synchronized with the HLSL. Resolution checks use analytic fields and preserve exact fallback; differing sizes alone never establish input admission.
- StageReadback.h and StageReadback.cpp test diagnostic color copies at actual allocation/region sizes. They never infer resource state, frame identity or color space. The caller must supply a live resource and prove submission, completion and recording discard. Uncertain lifetimes retain resources; do not use that bounded diagnostic policy for production streaming. Build separately with build_stage_readback.ps1.
- ComputeRecording.cpp checks missing observer coverage, stale Reset tickets and interleaved state rejection. It does not install hooks or prove live observer coverage.
- NativeSession.cpp runs synthetic inputs on independent DIRECT/COMPUTE queues. It checks rejected admission and unsubmitted/in-flight retirement, not glass quality. Its artificial GPU gate and CPU drains are test-only.
- TaggedInputs.cpp checks fresh phase admission and stale/mixed metadata rejection. StreamlineTagBridge.cpp tests the production registration/forwarding code with a mock parameter registry and independent D3D12 textures. It does not attach to or initialize the game's Streamline plugin.

- CommandLifetime.cpp verifies official destruction notifications on an independent device. ObservedSession.h drives the NativeSession tests through real production D3D12 hooks; it recognizes an explicit fixture surface, not a game signature.

## Document index

- [GeometryShaders.md](GeometryShaders.md): ObservedSession verifies paired pre/post submit queue/list identities with sticky failure state; independent GPU passed. No game history integration follows from this hook test.

- [GeometryShaders.md](GeometryShaders.md): Generation 9 measured retirement latency (64 jobs, 2-5 frames), unloaded. This rules out retired-readback delivery for consecutive-frame runtime MV; GPU history integration remains required.

- [GeometryShaders.md](GeometryShaders.md): Same-draw vertex/coverage diagnostic passed 64 live captures with exact contributing-reference masks and zero status. Visualization is vertex displacement plus material coverage, not dense boundary MV.

- [GeometryShaders.md](GeometryShaders.md): Optional bound-CB pair recording passed three recorded VS validators and live generation-7 capture (64 snapshots, 38 consecutive pairs). Candidate jitter subtraction is not final MV/FG proof.

- [GeometryShaders.md](GeometryShaders.md): Live replaceable vertex recorder captured 64 x 128 valid VS outputs and unloaded. 35 consecutive pairs retain one metadata tuple; jitter/view/topology admission and FG remain incomplete.

- [SourceSlots.md](SourceSlots.md): Live enqueue probe stopped after 4,096 updates; three associated recorded skinning VS accept history instrumentation. Cross-time owner overlaps are not admitted identity; live MV/FG integration remains absent.

- [Experiments.md](Experiments.md): Actual array setter identified; same pointer/count does not prove unchanged content. Secondary +0x158 array absent in 1,214 live owner snapshots and not admitted as previous history.

- [Experiments.md](Experiments.md): Live direct producer saved 31,860 source entries; 676 unique same-frame transform-range joins across 56 meshes. Pinned observer stopped; no lifetime/MV/FG proof.

- [Experiments.md](Experiments.md): Direct current-group/source-index callback preparation; bounded scalar reads and nested-scope fixture pass. Standalone observer not deployed; no object MV.

- [Experiments.md](Experiments.md): Close-angle 40-instance cups captured after raising diagnostic capacity to 64; actual overlap up to five slots. Union/reference exact; no new MV or FG correction.

- [Experiments.md](Experiments.md): Single visible instances of CPU/global clusters reject proxy-only history in production callback fixture. 19 appends/12 flushes remain forwarded; source guard not deployed.

- [Experiments.md](Experiments.md): Live 56 captures across two unloaded DLL generations. MO2 response redirection fixed in external client. Diagnostic ordinal coverage is not persistent object identity or MV.

- [Experiments.md](Experiments.md): Census analyzer validates CSV/target provenance; actual queue/in-flight capture/Reset/replacement preserve DLLs and pixels. Common host staged, no live game or FG proof.

- [GeometryTargets.md](GeometryTargets.md): GeometryTargetViews verifies public view/heap/copy hooks, OM snapshot stability and no texture retention. Controlled recorder saves exact actual test-draw targets. No game resource or FG validation.

- [Experiments.md](Experiments.md): `--controlled-recorder` adds event/file control, missing-observer rejection and duplicate-load rollback to two independently owned coverage generations. Original material/color and unload pass. Native queue observation and game/FG validation pending.

- [GeometryDraws.md](GeometryDraws.md): Mesh range decoder rejects malformed/stale callback inputs; owned-memory fixture only. Coverage sidecar preserves original draw/raster evidence without claiming engine history in the GPU fixture.

- [GeometryShaders.md](GeometryShaders.md): `--recorder` verifies two requests, same-draw references independent of missing object mapping, original shaders and submission/Reset gates on an independent device. Not deployed.

- [GeometryShaders.md](GeometryShaders.md): `--coverage` verifies per-object material bits without motion; `--capture-command` verifies production draw insertion/restoration with actual shader-history MV on a standalone device. Neither is game integration.

- [../Compatibility.md](../Compatibility.md): MRT preservation, public indirect resets, relocation/rejection and health tests. GeometryCompatibility reads an explicitly supplied PE into owned CPU memory; it never executes game code or attaches to a process.

- [GeometryDraws.md](GeometryDraws.md): Acquisition build 32054cc staged in MO2; live validation and FG substitution pending. Public draw/root and written-slot invalidation checks pass. Internal multi-instance arrays and non-mesh routes remain unverified.

- [GeometryObjects.md](GeometryObjects.md): Mesh lifetime generations, bounded pose candidates and callback layout/return tests. Synthetic owned memory only; no game hooks. Read the direct-provenance and unobserved-mutation limitations before using a candidate for a draw.

- [GeometryShaders.md](GeometryShaders.md): Original DXIL motion, separate contours and inactive-instance isolation/recovery pass eight-frame GPU checks through the public creation observer. The test hooks only its own device/process. Engine identity, draw ordering, boundary composition and game deployment remain incomplete.

- [../README.md](../README.md): Active module design, validation limits and standalone test instructions.
- [replay/README.md](replay/README.md): Standalone manifest-based FG replay, identity/input/output validation and recorded-input comparisons. Requires user-supplied local recordings and binaries; not part of the synthetic-only default suite.
- [ObjectMotion.md](ObjectMotion.md): Isolated geometry-motion and boundary-weight reference, verification and missing runtime inputs.
- [replay/BoundaryFG.md](replay/BoundaryFG.md): Subsequent actual FG tests on exact synthetic geometry and recorded Cyberpunk inputs. The latter has no per-object history/alpha; width changes are not a finished fix.
