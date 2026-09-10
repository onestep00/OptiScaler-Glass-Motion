# Experimental transparent-surface motion correction

- Created: 2026-09-10
- Updated: 2026-09-10
- Status: experimental; native host integration in progress
- Deployment: source and build integration only; not enabled in the game's FG path
- Deprecated: no
- Scope: Cyberpunk 2077 native D3D12 FG, recorded 2x/4x conventions
- Upstream base: `7b7220bbb4994a9c8ae60cfc75a44cb67995efb8` from `y4my4my4m/OptiScaler_DLSSNR_Multipass_MFG`

This directory owns the correction. `OptiScaler.vcxproj` imports `GlassFg.props` once. No upstream FG evaluation, loader or unlock code is changed at this stage. Building this branch does **not** enable the correction in a game.

## Boundaries

| Component | Responsibility |
| --- | --- |
| `GlassSurface.hlsl`, `GlassSurfaceGpu.h` | Surface-motion candidate and image correspondence |
| `GlassRegion.hlsl`, `GlassRegionGpu.h` | Bounded region selection, excluding broad flat glass and near-surface candidates |
| `GlassFgPass.h` | Typed NGX input validation, owned correction resources, one execution per rendered frame, scoped MV/depth substitution and restoration |
| `CyberpunkSurfacePass.h` | Version-specific executable signature and depth-transition identification; no fixed resource addresses |
| `SurfaceQueueLink.h` | Bounded CPU bookkeeping of observed submissions and fence dependencies; no GPU commands or waits |
| `GlassFgCompile.cpp`, `GlassFg.props` | Native SDK compilation and isolated MSBuild registration |

The host must supply an unambiguous surface snapshot, its generation and actual resource state. It must establish GPU ordering, preserve command-list state, and drain GPU use before resource release or recreation. `SurfaceQueueLink` records observed queue dependencies; it does not own resources or prove GPU completion. `CyberpunkSurfacePass` identifies an observed consumer transition, not the shader that wrote the depth.

The intended host seam is the native FrameGeneration branch of `NVNGX_DLSS_Dx12.cpp`, using `HandleToFeature`. Stale parameter keys alone must not classify an evaluation as FG. Keep per-feature lifetime, snapshot capture and command-list restoration in a separate host adapter in this directory; keep the upstream call site small. That adapter is not implemented yet.

## Existing MFG unlock

Keep the existing Ultimate ASI Loader `version.dll`, `plugins/mfg-unlock.asi` and OptiScaler loader configuration. Correction operates on FG input copies. It does not replace the ASI loader, patch support gates or kernels, or write multiplier options. Existing coexistence was observed in the running game; compatibility of a future connected correction still requires runtime validation.

## Validation to date

- Visual Studio 2022 Build Tools / MSVC 14.44: Release x64 solution build passed, including typed native NGX adapter instantiation.
- Recorded replay: 32 rendered inputs, 96 successful 4x evaluations, 31 correction dispatch sequences, 93 substitutions and 96 pointer restorations. Generated outputs and corrected MV/depth/masks matched the preceding candidate in all 189 compared files.
- Stale auxiliary depth was rejected; the following history was invalidated. Error/exception restoration and invalid-input rejection passed CPU contract checks.
- Live read-only depth selector: selected the expected candidate on 36/36 occurrences; two other candidates were never selected. FG evaluations succeeded. This was observation, not correction deployment.
- Queue trace: 2,866 events across 28 surface generations. Replay of queue bookkeeping bound 82 subsequent FG evaluations and bypassed 3 initial evaluations; adversarial ordering checks passed. This is CPU submission evidence, not GPU timing.
- Earlier candidate images reduced cup-body duplication; edge artifacts remain. Limited railing regions retained background motion. Dynamic objects and broader scenes are not accepted yet. No 1 ms performance guarantee is made.

Raw game captures and diagnostic DLLs are local investigation artifacts and are not part of this repository. These observations are not a portable end-to-end test suite.

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
