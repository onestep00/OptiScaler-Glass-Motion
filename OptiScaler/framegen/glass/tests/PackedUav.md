# Packed capture with original pixel UAV writes

- Created: 2026-09-12
- Updated: 2026-09-12
- Status: explicit in-place rewrite and independent GPU fixture verified; runtime integration incomplete
- Deployment: not enabled in the production compiler or installed game
- Deprecated: no
- Scope: original PS resource writes preserved during one instrumented draw; not general replay safety or FG quality

`RewriteMaterialMotion(..., preserveOriginalUavs=true)` permits original UAVs only
for packed capture with no native-input override. Original resource metadata and
operations remain present. The new capture UAV receives a new range ID and a
reserved-space binding. Original space 31 collisions are rejected. Ordinary
capture/replay calls still reject original UAVs. Ray calls and barriers remain
unsupported. The shader tool exposes this contract as `packed-inplace-mapped`.

Coverage-only capture does not need a merged final color value. It now preserves
branch-local exports without trying to infer opacity from them. Original exports
and discard remain unchanged; the added payload has zero interior weight. This
does not recover the material's opacity.

Depth/stencil exports receive a specific rejection before parsing their unassigned
signature register. The remaining recorded hair-alpha candidates export
`SV_DepthLessEqual`. Adding a UAV does not by itself prove that its writes are
limited to fragments passing the final depth/stencil test. Do not force early
tests or admit these shaders without validating original behavior and coverage.
Microsoft documents [early depth/stencil ordering](https://learn.microsoft.com/en-us/windows/win32/direct3dhlsl/sm5-attributes-earlydepthstencil).

## Independent GPU test

Run `build_packed_uav.ps1` in an x64 developer shell. It uses its own D3D12 device
and does not attach to the game. The fixture draws a 64x32 triangle with normal
transparent blending, pixel discard, a raw-buffer store and an atomic counter.
It compares 11 original/instrumented cases with valid and missing identities,
signed motion, motion outside the packed range, an overflowing object ID and a
new allocation generation whose history storage still contains the old tag.
Every draw runs once. Synthetic N-1 vertex history is supplied explicitly.

All 22,528 samples passed: original counter values, per-pixel buffer values and
color remained exact; discarded pixels produced no writes; the added packed
coverage appeared only with valid identity. The production compiler's default
path still rejected the original-UAV fixture. This checks raw-buffer writes and
atomics, not typed textures, append counters, MSAA, depth export, occlusion,
runtime identity, performance or actual DLSS-G quality.

Packed motion is quantized at 1/8 pixel with a range of -128 to 127.875 pixels
per axis at the capture viewport. Values outside the range now skip the added
write instead of silently clamping to a wrong displacement. Ordered comparisons
also reject nonfinite scaled values. Object IDs must fit 1..32767; higher bits
cannot wrap to another object. Exact decoded signed displacements were checked
inside the range. A skipped pixel is not a solved artifact or proof that this
format has enough range at every resolution/camera speed.

## Recorded shader batch

The expanded local base-cache batch has 3,592 unique candidate VS/PS pairs.
In-place mode validates 3,530 pairs: 3,040 use color-based capture and 490 use
coverage-only capture. The remaining 62 pairs export pixel depth. Another 25
techniques lack an exact VS/PS pair. These are bytecode conversion/validator
results, not actual pipeline execution or full world-transparency support.

Keep game shader binaries and local batch reports outside Git. The production
compiler continues using the default false argument until its draw insertion,
resource-state and coverage contracts have been verified for the relevant paths.
