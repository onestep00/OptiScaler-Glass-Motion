# Experimental transparent-surface motion correction

- Created: 2026-09-10
- Updated: 2026-09-10
- Status: experimental; native host integration in progress
- Deployment: menu/source/build integration only; not installed in the game or connected to native FG evaluation
- Deprecated: no
- Scope: Cyberpunk 2077 native D3D12 FG, recorded 2x/4x conventions
- Upstream base: `7b7220bbb4994a9c8ae60cfc75a44cb67995efb8` from `y4my4my4m/OptiScaler_DLSSNR_Multipass_MFG`

This directory owns the correction. `OptiScaler.vcxproj` imports `GlassFg.props` once. The existing menu has one include and one render call. No upstream FG evaluation, loader or unlock code is changed at this stage. Building this branch does **not** enable the correction in a game.

## Boundaries

| Component | Responsibility |
| --- | --- |
| `GlassSurface.hlsl`, `GlassSurfaceGpu.h` | Surface-motion candidate and image correspondence |
| `GlassRegion.hlsl`, `GlassRegionGpu.h` | Bounded region selection, excluding broad flat glass and near-surface candidates |
| `GlassFgPass.h` | Typed NGX input validation, owned correction resources, one execution per rendered frame, scoped MV/depth substitution and restoration |
| `CyberpunkSurfacePass.h` | Version-specific executable signature and depth-transition identification; no fixed resource addresses |
| `SurfaceQueueLink.h` | Bounded CPU bookkeeping of observed submissions and fence dependencies; no GPU commands or waits |
| `SurfaceSnapshotPool.h` | Four owned depth copies, producer/consumer completion and command-reset tracking, no wait on pool exhaustion |
| `GlassControls.h`, `GlassSettings.cpp` | Atomic controls, separate INI persistence and OptiScaler menu widgets |
| `GlassGpuTimer.h` | Sparse GPU timestamps, existing host completion fence and nonblocking readback |
| `GlassFgCompile.cpp`, `GlassFg.props` | Native SDK compilation and isolated MSBuild registration |

The host must supply an unambiguous surface snapshot, its generation and actual resource state. It must establish GPU ordering, preserve command-list state, and drain GPU use before resource release or recreation. `SurfaceQueueLink` records observed queue dependencies; it does not own resources or prove GPU completion. `CyberpunkSurfacePass` identifies an observed consumer transition, not the shader that wrote the depth.

The intended host seam is the native FrameGeneration branch of `NVNGX_DLSS_Dx12.cpp`, using `HandleToFeature`. Stale parameter keys alone must not classify an evaluation as FG. Keep per-feature lifetime, snapshot capture and command-list restoration in a separate host adapter in this directory; keep the upstream call site small. That adapter is not implemented yet. The UI explicitly reports this pending state. Selection preview remains disabled until a valid runtime texture can be displayed.

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

Settings changes are sampled when preparing phase 1; phases 2/3 retain the same prepared inputs. This preserves consistent 4x interpolation. The native host still needs to pass `ReadControls()`, supply the timer's submission/reset callbacks and publish completed timings to the menu.

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

Raw game captures and diagnostic DLLs are local investigation artifacts and are not part of this repository. The source-only standalone tests below are included; the recorded FG observations are not a portable end-to-end test suite.

## Standalone tests

From an x64 Visual Studio Developer PowerShell with the repository dependencies initialized:

```powershell
./OptiScaler/framegen/glass/tests/run-tests.ps1
```

This builds isolated GPU-resource/timing and settings/widget tests under `artifacts/glass-tests`. The settings test requires a fresh directory and does not access installed game settings. The GPU test uses its own device and queues. It uses deliberate queue gating and CPU drains to verify lifetime behavior; those test operations are not part of the timer implementation.

## Build and upstream updates

Clone with submodules, or run `git submodule update --init --recursive`. Build `OptiScaler.sln` with the upstream Windows dependencies and Release x64 configuration. HLSL files remain source assets; the eventual runtime host must provide their resolved paths or embed compiled shaders.

Development is committed on `glass-motion`. Keep `upstream` pointing to the original repository and `origin` pointing to the user's fork. Merge upstream changes into the development branch:

```sh
git fetch upstream
git switch glass-motion
git merge upstream/dlss-neural-rendering
git submodule update --init --recursive
```

Review conflicts and build before pushing. Preserve the single `GlassFg.props` import and this directory. Future upstream changes to native NGX inputs, command-list state handling or feature lifetime require adapter review; Git separation cannot guarantee automatic compatibility. Do not overwrite upstream changes with an old complete project file.

The original repository history, submodule pins and license remain intact. No separate patch-file installation workflow is required.
