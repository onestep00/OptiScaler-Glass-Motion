# Experimental transparent-surface motion correction

- Created: 2026-09-10
- Updated: 2026-09-10
- Status: experimental; native host connected and bounded runtime execution verified
- Deployment: source connected; production module exercised by a bounded game probe; full OptiScaler DLL and startup bridge not yet installed through MO2
- Deprecated: no
- Scope: Cyberpunk 2077 native D3D12 FG, recorded 2x/4x conventions
- Upstream base: `7b7220bbb4994a9c8ae60cfc75a44cb67995efb8` from `y4my4my4m/OptiScaler_DLSSNR_Multipass_MFG`

This directory owns the correction. `OptiScaler.vcxproj` imports `GlassFg.props` once. The existing menu has one include and one render call. The common Streamline plugin hook has one include and two integration calls for tag metadata. The native FG Evaluate branch now calls `NativeHost`; native creation/release/shutdown provide lifecycle notifications. ASI/MFG unlock behavior remains upstream-owned. Correction defaults off and requires the two HLSL assets beside the DLL in `Glass/`.

## Boundaries

| Component | Responsibility |
| --- | --- |
| `GlassSurface.hlsl`, `GlassSurfaceGpu.h` | Surface-motion candidate and image correspondence |
| `GlassRegion.hlsl`, `GlassRegionGpu.h` | Bounded region selection, excluding broad flat glass and near-surface candidates |
| `GlassFgPass.h` | Typed NGX input validation, owned correction resources, one execution per rendered frame, scoped MV/depth substitution and restoration |
| `CyberpunkSurfacePass.h` | Version-specific executable signature and depth-transition identification; no fixed resource addresses |
| `SurfaceQueueLink.h` | Bounded CPU bookkeeping of observed submissions and fence dependencies; no GPU commands or waits |
| `ComputeRecording.h` | Fresh COMPUTE recording admission, complete callback coverage and single-use epoch tickets |
| `NativeSession.h` | Per-feature capture, queue provenance, state admission, correction phases, completion and timer coordination |
| `NativeHost.cpp`, `D3D12Observer.cpp` | Existing NGX dispatch integration, real COM-method observation, feature retirement and UI telemetry |
| `CommandLifetime.h` | Official destruction notifications without retaining command lists or guessing final Release counts |
| `StreamlineTagBridge.cpp`, `TaggedInputs.h` | Named common-plugin callback registration and bounded clone-state metadata admission |
| `SurfaceSnapshotPool.h` | Four owned depth copies, producer/consumer completion and command-reset tracking, no wait on pool exhaustion |
| `GlassControls.h`, `GlassSettings.cpp` | Atomic controls, separate INI persistence and OptiScaler menu widgets |
| `GlassGpuTimer.h` | Sparse GPU timestamps, existing host completion fence and nonblocking readback |
| `GlassFgCompile.cpp`, `GlassFg.props` | Native SDK compilation and isolated MSBuild registration |

The host must supply an unambiguous surface snapshot, its generation and actual resource state. It must establish GPU ordering, preserve command-list state, and drain GPU use before resource release or recreation. `SurfaceQueueLink` records observed queue dependencies; it does not own resources or prove GPU completion. `CyberpunkSurfacePass` identifies an observed consumer transition, not the shader that wrote the depth.

The host seam is the native FrameGeneration branch of `NVNGX_DLSS_Dx12.cpp`, using `HandleToFeature`. Stale parameter keys alone do not classify an evaluation as FG. `NativeHost` prepares a per-feature session and scopes MV/depth substitution around the original native call. It publishes runtime status and completed timing samples. Selection preview remains disabled until a valid runtime texture can be displayed.

## Native session callback contract

Initialize one session per feature/size and bind its actual COMPUTE command list only after observer coverage is established. The platform adapter retains synchronization identities used by queue provenance, serializes the real queue call together with its after-callback, and excludes only this module's nested commands from observers. Call `captureIdentifiedSurface` immediately after a uniquely identified depth transition. A missing snapshot, multiple candidates between phase-1 evaluations, a missing native fence dependency or a non-fresh compute recording bypasses correction and invalidates history.

Successful Reset clears discarded surface recordings from queue provenance, discards tracked producer-command recordings and updates snapshot/timer bookkeeping. `afterSubmit` tracks all recorded uses of corrected outputs, including later MFG phases. Its monotonic completion fence also supplies sparse timing, so enabling the timer adds no extra signal. The snapshot pool separately signals producer/read completion. These ownership signals never substitute for a native producer-to-consumer dependency; the module inserts no waits.

The platform adapter calls `stop` on feature retirement and keeps forwarding submission/Reset callbacks while draining outstanding use. `readyToRelease` requires discarded command recordings and completed GPU work, not just one of those conditions. A Reset with a fresh allocator while the GPU is busy is not enough to release resources. `CommandLifetime` queries the official `ID3DDestructionNotifier` interface. Its callback only publishes a destruction flag and releases a small CPU token; it never dereferences the dying object, takes the host lock or releases GPU resources. Discarded recordings still require GPU completion. Resize/feature retirement preserves old sessions in a bounded two-entry retirement array and bypasses new correction until they drain. Shutdown stops admission; successful native FG creation can reopen it.

## Reusing the OptiScaler host

Reuse the existing native NGX feature map, parameter ABI and Evaluate dispatch. The module must not detour the private FG provider or reproduce MFG unlock. Resolve D3D12 methods from the actual COM interfaces, as the existing `D3D12_Hooks` does, rather than matching driver machine code. The executable fingerprints in `CyberpunkSurfacePass` serve a separate purpose: identifying a game-specific auxiliary depth pass that the public FG input does not name.

The existing `D3D12Hooks::RestoreRoot` is conditional on user configuration and does not unconditionally preserve all state needed by this insertion. Do not silently enable global restoration settings. The broad `ResTrack_Dx12::HookDevice` also explicitly skips `FGInput::NvngxFG`; enabling its HUD/resource registry wholesale is not a native-FG surface-capture solution. Keep the required Reset/barrier/submission callbacks in the isolated adapter and reuse existing dispatch points where their contracts match.

Current native FG observations show a fresh COMPUTE recording before each of the three generated phases. `ComputeRecording` admits insertion only after observing a successful `Reset(nullptr)` and all applicable state-changing entry points. It rejects missing hooks, non-COMPUTE lists, initial PSOs, intervening state setters, repeated insertion and stale reset tickets. The host must retain the command identity, serialize callbacks and exclude only its own correction commands from state observations. It must observe `SetPipelineState1` and `SetProgram` when their extended interfaces exist. Unknown interface-query errors do not count as interface absence.

After an admitted correction, `ClearState(nullptr)` restores the fresh binding contract before native FG. Resource barriers remain the pass's separate responsibility. This avoids a full binding-state save/replay and adds no queue submission or wait. `D3D12Observer` installs the complete applicable method set from actual COM interfaces using upstream `rewrite_signature` and Detours. It checks DIRECT/COMPUTE implementation compatibility, serializes queue calls with their after-callbacks and suppresses only this module's nested API calls. Binding changes on unrelated command lists avoid the host lock; depth barriers receive a cheap state filter before game-pass identification. Follow Microsoft's [Reset](https://learn.microsoft.com/en-us/windows/win32/api/d3d12/nf-d3d12-id3d12graphicscommandlist-reset) and [ClearState](https://learn.microsoft.com/en-us/windows/win32/api/d3d12/nf-d3d12-id3d12graphicscommandlist-clearstate) contracts.

### Streamline tag-state bridge

`OnStreamlineCommonLoad` receives the parameter interface and parsed common-plugin version from OptiScaler's existing loader. `WrapStreamlineCommonFunction` wraps the common plugin's named startup/shutdown callbacks. After successful startup, it obtains `sl.param.global.getTag` through the existing typed `IParameters` ABI and registers a forwarding wrapper before dependent plugins initialize. It does not scan driver instructions or detour a private FG provider. Shutdown disables observation and restores the original registration if the bridge still owns it. A module reference keeps the original forwarding target mapped for cached callbacks; replacing that target in the same process is rejected.

The wrapper reads a borrowed `CommonResource` result without modifying it or copying a `shared_ptr`. This is an **internal Streamline structure**, not a stable public FG input API. Its layout is isolated and admitted only for common versions 2.14.0/2.14.1. Offsets derive from the pinned NVIDIA 2.14.1 source and were observed on the installed 2.14.0 OTA binary. Structure GUID/version, clone identity, frame, state, viewport, extent and actual native-input pointer checks reject unknown inputs. Another Streamline ABI requires review; a driver patch alone does not select a new byte signature here. The game-specific auxiliary-depth identifier remains separately version-dependent.

Native FG input states can be read through `ReadStreamlineStates` once per native evaluation. Phase 1 requires fresh matching depth/motion/HUD-less tags; incomplete, mixed-frame or repeated data are rejected. The current supported state is COPY_DEST on ordinary 2D textures, with no simultaneous-access flag. The bridge stores scalar identities under a short mutex because live tag retrieval and native Evaluate occur on different threads. It allocates no GPU resource and records no GPU command. These metadata checks do not replace surface snapshot ownership, command-state admission or queue provenance.

Two bounded read-only game probes observed FG 91/90 times with no failures. After initial incomplete observations, native MV/color/depth matched their common tag clones in 81/78 evaluations respectively; all matched states were COPY_DEST and resource flags were 0x5. Diagnostic probes used an inspected, file-hash-guarded function RVA; that address is absent from this production bridge. Hooks were disabled afterwards. The new startup registration path passed an independent test with real D3D12 resource descriptions, unchanged forwarded outputs, different producer/consumer threads and shutdown behavior; it has not yet been deployed in the game.

A subsequent bounded game probe compiled the production decoder, `TaggedInputs` and `ReadStreamlineStates` directly. All 155 returned tags decoded successfully; 91 of 93 observed native evaluations passed metadata admission, with two initial incomplete phases rejected. All 104 observed FG calls succeeded. The probe added no GPU command or input substitution and was disabled afterwards. This validates live decoding/admission; the game's startup registration path and actual correction are still pending. Release x64 build and all six standalone executables passed after the final code changes.

## Native Windows host verification

The observer-driven standalone session test uses real D3D12 Reset, barrier, ExecuteCommandLists, Signal and Wait calls rather than manually forwarding their notifications. All applicable 16 binding observations installed. Missing native ordering, ambiguous surfaces and dirty compute state rejected correction. Reset without GPU completion retained resources. Destruction without Reset also retained in-flight resources until completion and allowed release afterward. A separate destruction-notifier test passed callback, explicit unregister and owner-before-command teardown cases. All eight standalone executables and the Release x64 solution build passed.

A five-second game probe compiled the production `NativeHost`, `NativeSession`, D3D12 observer, decoder and shaders. It captured 88 identified surfaces and substituted owned MV/depth inputs in 258 of 270 host evaluations. All 286 FG calls observed by the outer diagnostic wrapper succeeded. Stop disabled further correction and the host reported all entries retired after completion. The game's installed OptiScaler and MFG unlock files were unchanged. The probe's central/tag observation hooks were disabled; process-lifetime D3D12 forwarding hooks remain loaded with no correction session until game exit. No diagnostic DLL is distributed in this repository.

The diagnostic entry uses a file-identity-checked common-tag address because the game is already initialized. The production startup bridge still uses named registration. Therefore this experiment proves live capture, ordering, correction substitution and retirement, but not full DLL startup deployment or the final menu layout. The observed scene had changed to a wider view of the casino. No new moving-glass quality claim or performance guarantee follows from these successful calls.

## Existing MFG unlock

Keep the existing Ultimate ASI Loader `version.dll`, `plugins/mfg-unlock.asi` and OptiScaler loader configuration. Correction operates on FG input copies. It does not replace the ASI loader, patch support gates or kernels, or write multiplier options. Existing coexistence was observed in the running game; compatibility of a future connected correction still requires runtime validation.

## Menu controls and timing

The FG settings window contains **Transparent surface correction (experimental)**:

- Enable glass motion correction; default off while integration is unfinished.
- Correction strength, 0–100. Zero bypasses correction. Lower nonzero values require a larger fraction of reliable surface seeds per region. The selected vectors remain the actual projected surface vectors; surface/background velocities are not averaged. 100 preserves the preceding candidate's thresholds.
- Save, reload and reset buttons for `OptiScaler.Glass.ini`, alongside the loaded OptiScaler DLL. This separate file preserves unrelated OptiScaler settings and is not read on each evaluation.
- A GPU correction time line is always present. Measurement defaults on. Inactive and pending states are shown explicitly. `MeasureGpuTime=false` in the separate INI can disable sampling.

The pass samples the first and then every 30th correction batch. Each sample adds two timestamp queries and one 16-byte resolve. Eight slots reserve 128 bytes of readback data, plus driver-managed query/fence metadata. The host supplies its existing monotonic GPU completion fence. The timer adds no queue signal, GPU wait, CPU wait, clock calibration or stable-power-state changes. It only reads completed results and skips samples when its ring is full.

The displayed interval covers correction input copies and compute passes on the FG COMPUTE queue. It excludes the producer's surface-depth capture, DLSS-G evaluation and presentation. It is the last sampled interval, not total frame latency or the measurement's own overhead. Query operations still have a cost; sparse sampling limits their frequency without claiming zero overhead. Follow [Microsoft's timestamp contract](https://learn.microsoft.com/en-us/windows/win32/direct3d12/timing) and [query-resolution lifetime contract](https://learn.microsoft.com/en-us/windows/win32/api/d3d12/nf-d3d12-id3d12graphicscommandlist-resolvequerydata).

Settings changes are sampled when preparing phase 1; phases 2/3 retain the same prepared inputs. This preserves consistent 4x interpolation. `NativeHost` supplies controls and submission/reset callbacks, then publishes completed samples without waiting.

## Validation to date

- Visual Studio 2022 Build Tools / MSVC 14.44: Release x64 solution build passed, including typed native NGX adapter instantiation.
- Recorded replay: 32 rendered inputs, 96 successful 4x evaluations, 31 correction dispatch sequences, 93 substitutions and 96 pointer restorations. Generated outputs and corrected MV/depth/masks matched the preceding candidate in all 189 compared files.
- Stale auxiliary depth was rejected; the following history was invalidated. Error/exception restoration and invalid-input rejection passed CPU contract checks.
- Live read-only depth selector: selected the expected candidate on 36/36 occurrences; two other candidates were never selected. FG evaluations succeeded. This was observation, not correction deployment.
- Queue trace: 2,866 events across 28 surface generations. Replay of queue bookkeeping bound 82 subsequent FG evaluations and bypassed 3 initial evaluations; adversarial ordering checks passed. This is CPU submission evidence, not GPU timing.
- Strength controls: 100 matched all 189 prior output/input files; 0 matched all 96 unmodified baseline generated frames. At 50, selected pixels were a subset in all 31 corrected frames. Outside-selection values and MV Z/W stayed bit-identical. Frame 13 selected 95,100 pixels at 100 and 51,526 at 50. Full-scene images and MV views were inspected for frames 13/18; lower strength leaves some cups uncorrected and is not a new quality acceptance result.
- Timing replay: 96 FG evaluations succeeded and all 189 output/input files matched the run without timing. Two sparse samples were 0.284672 and 1.059840 ms; the first was a history warmup. These are replay observations with a game also running, not a live-game benchmark, a 1 ms guarantee or a timer-overhead measurement.
- Standalone GPU test: four depth copies, 8,192 exact pixels, in-flight reuse rejection, stale-token rejection and discarded-recording reuse passed. Timing returned no sample before completion and no duplicate after delivery. The D3D12 debug layer was unavailable; this is functional GPU evidence, not debug-layer validation.
- Settings tests: defaults, range clamping, save/reload, unrelated-key preservation, failed replacement preserving the previous file, malformed-value fallback and headless ImGui vertex generation passed. Production retains FreeType; the headless test uses stb fonts. This does not prove the final game-menu layout.
- Earlier candidate images reduced cup-body duplication; edge artifacts remain. Limited railing regions retained background motion. Dynamic objects and broader scenes are not accepted yet. No 1 ms performance guarantee is made.
- Same-recording replay: correction and FG were recorded together without submitting/draining between them. Readbacks moved after native Evaluate. All 96 FG calls succeeded and all 189 generated/corrected-input files matched the earlier replay. This closes the previous replay's submission-boundary gap; it does not prove live hook coverage or resolve edge quality.
- Bounded live state experiment: 16 base/extended method hooks installed successfully on the actual FG command list, including predication, indirect execution, state objects and programs. The production admission gate permitted 63 ClearState calls; all 134 observed FG calls succeeded. Hooks were disabled afterwards. No MV/depth substitution occurred, so this is state compatibility evidence, not live correction deployment or a performance result.
- Compute recording contract test: individually missing each of 16 observations, failed Reset, initial PSO, stale tickets, repeated insertion, changed identity and non-COMPUTE lists were rejected. This CPU test checks admission semantics, not the platform hook installer.
- Native session replay: a DIRECT queue uploads the recorded auxiliary depth and the pool captures it; native-style Signal/Wait links it to the COMPUTE queue. The session runs correction and actual 4x FG in one recording. All 96 calls succeeded and all 189 generated/corrected-input files matched the prior replay. Stale frame 19 was bypassed and completion plus discarded-recording release was verified. File-upload/readback waits belong to the standalone harness, not the session.
- Standalone native session test: missing dependency observation, ambiguous captures and intervening predication calls reject admission. Unsubmitted and GPU-in-flight release attempts are rejected; completion after Reset permits release. Timing is unavailable before completion, delivered once afterwards, and stop prevents new captures/evaluations. This uses an independent GPU device and synthetic inputs, not a game attachment or a quality test.

- Tag bridge tests: unsupported common versions and failed startup do not install a reader; named registration preserves output bytes and forwarding after shutdown. Fresh 2x/4x phases, cross-thread metadata, stale/mixed/missing data, changed handles/states/viewports/offsets and malformed structure/extent rejection passed. These tests do not prove the deployed game's startup interception.

Raw game captures and diagnostic DLLs are local investigation artifacts and are not part of this repository. The source-only standalone tests below are included; the recorded FG observations are not a portable end-to-end test suite.

## Standalone tests

From an x64 Visual Studio Developer PowerShell with the repository dependencies initialized:

```powershell
./OptiScaler/framegen/glass/tests/run-tests.ps1
```

This builds isolated GPU-resource/timing and settings/widget tests under `artifacts/glass-tests`. The settings test requires a fresh directory and does not access installed game settings. The GPU test uses its own device and queues. It uses deliberate queue gating and CPU drains to verify lifetime behavior; those test operations are not part of the timer implementation.

## Build and upstream updates

Clone with submodules, or run `git submodule update --init --recursive`. Build `OptiScaler.sln` with the upstream Windows dependencies and Release x64 configuration. `GlassFg.props` copies `GlassSurface.hlsl` and `GlassRegion.hlsl` into `$(TargetDir)Glass` and the upstream Release package's `a/Glass`. The fast build artifact also includes both shaders. Deployment requires this directory beside the OptiScaler DLL. The host resolves paths from the loaded DLL, independent of the process working directory.

Development is committed on `glass-motion`. Keep `upstream` pointing to the original repository and `origin` pointing to the user's fork. Merge upstream changes into the development branch:

```sh
git fetch upstream
git switch glass-motion
git merge upstream/dlss-neural-rendering
git submodule update --init --recursive
```

Review conflicts and build before pushing. Preserve the single `GlassFg.props` import and this directory. Future upstream changes to native NGX inputs, command-list state handling or feature lifetime require adapter review; Git separation cannot guarantee automatic compatibility. Do not overwrite upstream changes with an old complete project file.

The original repository history, submodule pins and license remain intact. No separate patch-file installation workflow is required.
