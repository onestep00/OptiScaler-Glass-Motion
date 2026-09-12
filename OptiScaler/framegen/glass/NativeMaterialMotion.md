# Native clip inputs with transparent material capture

- Created: 2026-09-12
- Updated: 2026-09-12
- Status: independent GPU validation passed; production integration incomplete
- Applied: source only; installed host unchanged
- Deprecated: no
- Scope: explicit current/previous clip inputs with supported read-only transparent material draws; no automatic engine-input admission

`NativeClipInputs::material` opts into the existing material blend validator and
transmission equation while using explicit current/previous float4 pixel inputs.
It preserves original color exports, blending and material discard. Depth/stencil
writes are rejected. The default opaque-native mode retains its stricter no-blend,
no-discard contract and can preserve native depth writes. Shader input IDs remain
explicit; this option does not discover or manufacture previous object transforms.

Run `tests/build_native_material.ps1` in an x64 Visual Studio Developer PowerShell.
The independent fixture checks perspective/jitter motion, varying transmission,
a discarded horizontal hole, opaque occlusion, exact original blended color and
depth, and rejection of a depth-writing material. The initial material run recorded
22 samples with maximum normalized MV error `1.005e-7`; occlusion retained 14.
All 256 color/depth pixels matched the original draw exactly. Opaque-native and
raw-input regressions also passed. This is not a game performance measurement.

## Live evidence preceding this source change

Local diagnostics in `work/glass-input-words-v1/` are not distributed:

- Shader evaluation of previous bone data matched 620 original N-1 local vertices
  exactly across four pairs. The engine's current Z input matched the prior X
  bone offset. This covers one selected skinned transparent route.
- Copying the opaque shader's `b7` previous object transform into that transparent
  route failed: previous screen coordinates differed by about 2,741 pixels.
  A bound register alone does not prove matching contents. That candidate was rejected.
- Reusing the current object transform with previous bones and the previous camera
  agreed with 310 actual N-1 screen vertices across two pairs, within 0.000106
  render pixels. The selected proxies had no separate history slot in a later CPU
  snapshot. Neither fact authorizes a static-transform assumption for other frames
  or objects. Moving object-root support is still required.
- A subsequent local shader diagnostic captured 793 material pixels with MV and
  RGB transmission, including 126 raster-footprint edge pixels. Another chunk
  contributed three pixels. The chunks belonged to different proxies and were not
  merged. The vertex comparison and pixel captures are from different frames.
- Capturing material pixels does not establish the complete object silhouette,
  all transparency families, synchronized FG inputs, or ghosting improvement.
  The latest pixel diagnostic did not independently compare original color and
  reference masks. All captures retired; the diagnostic module was unloaded.

The source change removes a temporary pixel-rewrite workaround from the required
implementation path. Linking newly derived VS outputs to original PS inputs,
proving frame/object inputs, bounded runtime storage, full-object boundary
composition and FG replacement remain separate unfinished work.
