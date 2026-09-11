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

- [Experiments.md](Experiments.md): `--capture-module` drives capture Prepare/Recorded/Retired through the resident owner for eight real test draws. Submissions are forwarded by the fixture; Reset is observed. Original color/MV pass; game/FG integration pending.

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
