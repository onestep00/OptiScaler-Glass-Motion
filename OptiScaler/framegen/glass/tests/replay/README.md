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
