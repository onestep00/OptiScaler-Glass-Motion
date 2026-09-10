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

- [../README.md](../README.md): Active module design, validation limits and standalone test instructions.
- [replay/README.md](replay/README.md): Standalone manifest-based FG replay, identity/input/output validation and recorded-input comparisons. Requires user-supplied local recordings and binaries; not part of the synthetic-only default suite.
- [ObjectMotion.md](ObjectMotion.md): Isolated geometry-motion and boundary-weight reference, verification and missing runtime inputs.
