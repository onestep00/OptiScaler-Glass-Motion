# Standalone recorded DLSS-G replay

- Created: 2026-09-10
- Updated: 2026-09-10
- Status: standalone implementation verified against recorded inputs; scene generalization and quality experiments continue
- Deployment: standalone only; no game attachment
- Deprecated: no
- Scope: GFR1 evaluation packets, RGBA8 color/HUDless, RGBA16F motion, R32 depth, 2x/4x generated phases

This executable creates a D3D12 device and a fresh native FG feature, uploads recorded inputs, calls the locally supplied NVIDIA provider and saves each generated phase. It has no window, input automation or game-scene simulation. Optional prepared MV/depth files are substituted before evaluation. Candidate preparation remains separate, allowing the production correction code or offline experiments to generate those files.

Paths, dimensions, frame count and multiplier come from a UTF-8 JSON manifest. Dimensions are configurable; the resource format profile is currently fixed and must be declared as `rgba8-mv16f-depth32`. Input files use `frame-NN-index-S.bin`, with S=0 color, 7 HUDless, 4 motion and 5 depth. The GFR1 packet carries recorded evaluation parameters and inline matrix/vector payloads. Unmapped non-null pointers are rejected. Frame zero resets the fresh feature. Creation defaults are explicitly reconstructed, not captured; unknown internal history is not reproduced.

Required fields are `provider`, `capture`, `arguments`, `output`, `outputWidth`, `outputHeight`, `renderWidth`, `renderHeight`, `frames`, `generatedCount`, `applicationId`, `sdkVersion` and `formatProfile`. Relative paths resolve against the manifest. `generatedCount` is 1 or 3. Three requires the caller's `unlock` DLL. `overrides` optionally names prepared replacement motion/depth files. Output must not already exist. No DLL is installed into a game directory.

Build using `build.ps1` from an x64 Visual Studio Developer PowerShell. Run the resulting `nvngx.dll.glass-replay.exe manifest.json`. The filename preserves the provider caller-name convention already observed in the local replay experiment. Provider and unlock identity must be recorded for comparisons. Neither is bundled.

Prefer `python run.py manifest.json --executable path/to/nvngx.dll.glass-replay.exe`. This requires `providerSha256` and, for 4x, `unlockSha256`. It verifies binary identities, packet frame count and every selected input length, inventories original/replacement hashes, validates every generated phase, and writes a report beside the output. Use `--compare reference-output --expect-identical` for a repeatability check. Ordinary candidate comparisons omit `--expect-identical`; differences are recorded without treating either candidate as visually better. Python 3.11 or later is required; no third-party Python packages are used.

The initial 24-frame 4x recording produced 72 successful outputs twice. All outputs matched each other and the preceding local replay byte-for-byte. A third run through the Python validator also matched all 72. An eight-frame prefix ran successfully at 2x, producing eight outputs. These checks establish the port's recorded-input behavior, not new ghosting improvement or coverage of every dimension/format/provider version. The current game files were unchanged.

Compare candidates with identical original packets, creation defaults, binaries, resets and frame order. Repeat the baseline to check reproducibility. Inspect real generated frames and changed-input masks; lower photometric error does not prove better FG. CPU/GPU drains in this offline harness are synchronization for correctness, not a model for the final game's performance.

## Separated-layer experiment

`build_layer_composite.ps1` builds the independent `LayerComposite.exe` runner for `../../GlassLayerComposite.hlsl`. It reads eight same-size RGBA32F files named `input-0.bin` through `input-7.bin`: previous/current accumulated source color, previous/current RGB transmission, FG-generated background, fallback generated color, previous/current endpoint UVs packed into XY/ZW, and correspondence validity in X. It writes RGBA32F. Invoke with `shader input-directory width height phase output-file [admitted]`; existing output files are rejected. This runner has no FG call, capture hook or game attachment. It consumes a previously generated background.

Run `python test_layer_composite.py --executable <exe> --shader <hlsl> --output <fresh-directory>` for the standard-library-only synthetic contract check. The 2,048-pixel GPU check covers three generated phases, global rejection, invalid/unknown correspondence, nonfinite inputs, unsupported transmission, HDR/additive/opaque colors and alpha preservation. Separately, the shader's 1280x720 output on a recorded synthetic background-FG image matched the Python endpoint-warp reference at every valid interior 8-bit pixel.

A controlled no-refraction scene compared background motion, all-surface motion, binary gradient-energy selection and blended motion through 12 rendered frames of actual 4x FG. Across frames 4–11 and all three phases, cup-region RGB MAE against analytic intermediate-time truth was 5.42, 6.13, 4.37 and 4.99 respectively (8-bit levels). A fifth background-only FG run with separately warped endpoint source/transmission reduced cup MAE to 1.48 and visibly removed the prominent duplicated cup outlines in the inspected middle-phase frame. Pane MAE was 1.00 versus 0.95 for the original, so the experiment is not a universal improvement. Each run produced 36 successful generated outputs. The opaque control put phases near their intended positions but retained roughly 1.3-pixel edge/centroid errors; this is an approximate quality control, not exact temporal calibration.

These results assume known layer separation, known constant surface/background translation, a shared linear blend domain, no refraction and no independently moving overlapping transparent layers. Actual game F/T sequences, background, endpoint correspondences, tone mapping/exposure and resource ownership have not passed those requirements. Do not enable the compositor on the current game inputs. Full-frame and actual generated images were inspected; synthetic success does not approve game deployment. The new shader is isolated source and is not copied into the production package or called by `NativeHost`.
