# Standalone module tests

- Read the parent AGENTS.md and README.md before changes.
- ObjectMotion.cpp / check_object_motion.py validate synthetic geometry and per-object boundaries; read ObjectMotion.md. Fixture IDs/history are not engine data. Conflicting edges remain unresolved; no FG quality or production cost claim.
- These executables use an independent D3D12 device or a fresh test settings directory. Never attach to a game or point the settings test at an installed directory.
- GpuResources.cpp checks real GPU copies, cross-queue completion and timing. Its measured copy duration is not timer overhead or correction performance.
- Settings.cpp includes the production settings implementation with a test-only DLL-path provider. ImGui uses stb fonts in this headless test; production retains FreeType. Vertex generation is not visual acceptance of the game menu.
- All settings test directories must be new. Build artifacts belong outside source control.
- MaterialCaptureBlend.cpp checks source color, RGB transmission and surviving-pixel coverage for five blend families. It includes discarded pixels and unit transmission with nonzero source color. This is not game layer extraction or FG quality validation.
- PackedMotion.cpp/HLSL checks SM 6.6 root-UAV 64-bit atomic nearest-layer arbitration on an independent NVIDIA device. GeometryInstances `--packed` and `--packed-mrt` check the rewritten material in real single/independent-MRT graphics draws while preserving original color/depth/discard. Neither measures game cost or connects the packed result to FG.
- LayerComposite.cpp runs the isolated compositor on caller-supplied float4 files. Build and contract checks are documented in replay/README.md. It never captures or changes a game, and it is not part of the default suite that has no Python dependency.
- The compositor runner accepts independent per-input dimensions and valid regions. Keep its 36-constant layout and normalized-offset interpretation synchronized with the HLSL. Resolution checks use analytic fields and preserve exact fallback; differing sizes alone never establish input admission.
- StageReadback.h and StageReadback.cpp test diagnostic color copies at actual allocation/region sizes. They never infer resource state, frame identity or color space. The caller must supply a live resource and prove submission, completion and recording discard. Uncertain lifetimes retain resources; do not use that bounded diagnostic policy for production streaming. Build separately with build_stage_readback.ps1.
- ComputeRecording.cpp checks missing observer coverage, stale Reset tickets and interleaved state rejection. It does not install hooks or prove live observer coverage.
- NativeSession.cpp runs synthetic inputs on independent DIRECT/COMPUTE queues. It checks rejected admission and unsubmitted/in-flight retirement, not glass quality. Its artificial GPU gate and CPU drains are test-only.
- TaggedInputs.cpp checks fresh phase admission and stale/mixed metadata rejection. StreamlineTagBridge.cpp tests the production registration/forwarding code with a mock parameter registry and independent D3D12 textures. It does not attach to or initialize the game's Streamline plugin.

- CommandLifetime.cpp verifies official destruction notifications on an independent device. ObservedSession.h drives the NativeSession tests through real production D3D12 hooks; it recognizes an explicit fixture surface, not a game signature.

## Document index

- [Timeline.md](Timeline.md): Live diagnostic parent lookup and 8,000 concurrent-event checks pass. Neighbor-frame mismatches remain; no GPU replay or MV/FG proof.

- [GeometryDraws.md](GeometryDraws.md): Same-flush packet parent capture verified live; exact global source decoder resolves all 6,094 recorded array spans. build_packet_parents.ps1 performs owned callbacks/optional recorded decoding, never injection. Child lifetime/MV remains incomplete.

- [ArraySourceDomains.md](ArraySourceDomains.md): Caller-context diagnostics captured 697 game updates and resolved 2,686 source-element occurrences. Four independent tests cover real unwind/callback and scalar decoding. Cross-frame lifetime, foliage/particle identity and MV/FG remain incomplete.

- [SourceSlots.md](SourceSlots.md): Source-slot to history lookup checks original index, owner, view and frame. Node IDs identify the parent setup; native array generations and game connection remain incomplete.

- [VertexHistoryCache.md](VertexHistoryCache.md): Original source-index lookup joins the sealed source table to bounded history; reorder/replacement and 300 churn frames pass CPU checks. Native lifetime/update and game integration remain incomplete.

- [PackedUav.md](PackedUav.md): In-place packed PS preserves original writes/color through 22,528 GPU samples, rejects out-of-range MV/IDs and reused history. Default production admission stays off; depth exports, live coverage and FG remain incomplete.

- [../NativeMaterialMotion.md](../NativeMaterialMotion.md): `build_native_material.ps1` tests transparent native clip capture, original blended color/depth/discard/transmission and opaque/raw-input regressions on an independent device. Never attach it to the game.

- [GeometryObservation.md](GeometryObservation.md): Stream reconstruction tests use Microsoft's public parser and an independent D3D12 device. They do not prove a fresh game captured every live PSO.

- [GeometryShaders.md](GeometryShaders.md): Raw unused uint inputs verified on GPU and live. Three N-1 pairs have current Z equal to previous X; bone contents/previous geometry and dense MV/FG remain unverified.

- [GeometryShaders.md](GeometryShaders.md): Optional pre-submit module bridge passes actual queue callbacks and legacy capture/unload; unsupported hosts reject it. No live history reuse or FG integration.

- [GeometryShaders.md](GeometryShaders.md): 32-frame single-list GPU history test reuses two buffers without interframe CPU waits/copies; 558 previous vertices match. Game queue admission and dense MV/FG remain incomplete.

- [GeometryShaders.md](GeometryShaders.md): Matching N-2 history is now GPU-rejected; original rendering and mapped capture regression pass. Continuous game history and FG remain incomplete.

- [SourceBootstrap.md](SourceBootstrap.md): Worker-owned PSO reuse verified live (one build, 63 reuses); buffers still per-capture and continuous history/FG absent.

- [SourceBootstrap.md](SourceBootstrap.md): Two slots per diagnostic shape produced two exact N-1 pairs (310 original vertices); gaps excluded. Continuous history/view identity and dense MV/FG remain incomplete.

- [GeometryObservation.md](GeometryObservation.md): Depth-independent observation and one-time failed-material to vertex-only recovery pass independent D3D12 checks; retained payload is reused. Live missing-chunk descriptor and FG remain incomplete.

- [SourceBootstrap.md](SourceBootstrap.md): Fixed mesh-slot monopolization; 32 live frames capture both chunks with exact reference coverage. Different proxy identities prohibit merging them into one object. Full contours/MV/FG remain incomplete.

- [CoverageLayout.md](CoverageLayout.md): Packed mask layout passes CPU and original-material GPU checks, including inactive entries and escaped bounds. Runtime bounds/MV integration remains absent.

- [Experiments.md](Experiments.md): Explicit capture selection now matches the recorder's 64-instance capacity; prior limit rejected the 40-instance draw.

- [SourceSlots.md](SourceSlots.md): DetourChain verifies bounded chaining/restoration; linked game pair is healthy but cache empty without creation events. Bootstrap and MV/FG incomplete; never inject test fixtures.

- [GeometryShaders.md](GeometryShaders.md): Mapped native pixel capture preserves color/depth and matches invocation counts. Live 95-pixel partial geometry image exists; complete boundary extraction remains absent.

- [GeometryShaders.md](GeometryShaders.md): NativePairGpu adds analytic native pixel-MV checks; depth remains disabled in that fixture, so live pixel deployment is not approved by this result.

- [GeometryShaders.md](GeometryShaders.md): Native-pair live capture and N-1 vertex comparison verified for one draw; screen boundary and FG input still absent.

- [GeometryShaders.md](GeometryShaders.md): NativePairGpu verifies selected native outputs, perspective W, jitter distinction, tags and guards; recorder builds. Actual game pair/boundary capture remains incomplete.

- [GeometryObservation.md](GeometryObservation.md): DXIL native-request fixture verifies DLL-worker request, deduplication, prepared indexed-owner dispatch and stopped rejection. Recorded draw is not submitted.

- [GeometryShaders.md](GeometryShaders.md): Depth-writing vertex-capture comparison preserves 122,880 color and depth samples; material rewrite still rejects writable depth. No live native-pass capture.

- [GeometryObservation.md](GeometryObservation.md): Original root serialization ownership and actual recorder file equality pass. Native-pass GPU capture still requires a separate admission path.

- [GeometryObservation.md](GeometryObservation.md): Actual binding recorder DLL passes independent host event/file roundtrip and unload. No GPU buffer contents or game MV captured.

- [GeometryObservation.md](GeometryObservation.md): Module-facing root layout and actual CBV address match in independent draw census. ABI versions advanced; no live game previous-transform capture yet.

- [GeometryShaders.md](GeometryShaders.md): Native target-3 extraction validates and preserves two recorded velocity PS instruction streams; absent target rejected. Assembly audit only, no native-pass GPU execution.

- [GeometryShaders.md](GeometryShaders.md): ObservedSession verifies paired pre/post submit queue/list identities with sticky failure state; independent GPU passed. No game history integration follows from this hook test.

- [GeometryShaders.md](GeometryShaders.md): Generation 9 measured retirement latency (64 jobs, 2-5 frames), unloaded. This rules out retired-readback delivery for consecutive-frame runtime MV; GPU history integration remains required.

- [GeometryShaders.md](GeometryShaders.md): Same-draw vertex/coverage diagnostic passed 64 live captures with exact contributing-reference masks and zero status. Visualization is vertex displacement plus material coverage, not dense boundary MV.

- [GeometryShaders.md](GeometryShaders.md): Packed material rewriting preserves the original draw and passes independent GPU execution. Eight currently observed live PSOs compiled without submitting replacement draws. Continuous history, frame clearing and FG composition remain incomplete.

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

- [GeometryObjects.md](GeometryObjects.md): Lifetime-only runtime default and optional diagnostic pose index pass three executables. Synthetic callback checks only; latest reduction not deployed, direct-provenance and MV gaps remain.

- [GeometryShaders.md](GeometryShaders.md): Original DXIL motion, separate contours and inactive-instance isolation/recovery pass eight-frame GPU checks through the public creation observer. The test hooks only its own device/process. Engine identity, draw ordering, boundary composition and game deployment remain incomplete.

- [../README.md](../README.md): Active module design, validation limits and standalone test instructions.
- [replay/README.md](replay/README.md): Standalone manifest-based FG replay, identity/input/output validation and recorded-input comparisons. Requires user-supplied local recordings and binaries; not part of the synthetic-only default suite.
- [ObjectMotion.md](ObjectMotion.md): Isolated geometry-motion and boundary-weight reference, verification and missing runtime inputs.
- [replay/BoundaryFG.md](replay/BoundaryFG.md): Subsequent actual FG tests on exact synthetic geometry and recorded Cyberpunk inputs. The latter has no per-object history/alpha; width changes are not a finished fix.
