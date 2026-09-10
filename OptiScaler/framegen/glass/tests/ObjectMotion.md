# Object geometry motion and separate boundary reference

- Created: 2026-09-10
- Updated: 2026-09-11
- Status: isolated GPU geometry verification passed; engine input acquisition remains incomplete
- Deployment: none; neither the native host nor the installed game DLL uses this path
- Deprecated: no
- Scope: caller-owned synthetic current/previous geometry, object and camera transforms; no FG quality claim

## Required behavior

A confirmed, visible silhouette of a transparent object selects that object's full geometric motion, even when its opacity is low. Transparent interiors may retain background motion. The surface motion must include both object and camera changes; deformed geometry requires previous deformed vertex positions as well as matrices.

`GlassObjectMotion.hlsli` computes current and previous clip positions from their own object and camera transforms. The raster fixture interpolates both homogeneous positions with perspective correction and divides in the pixel shader. Motion is normalized `previousUV - currentUV`. Matrices exclude jitter; current jitter affects rasterization only. Valid geometric motion can point outside the previous viewport. A previous point behind the camera is rejected, and visibility/disocclusion admission remains separate from the geometric calculation.

For opacity `a`, confirmed object edge `E`, gain `k` and bias `b`, the helper supplies:

```
interior = clamp(k * a + b, 0, 1)
w = E + (1 - E) * interior
MV = backgroundMV + w * (objectMV - backgroundMV)
```

`E=1` produces full object motion regardless of opacity. The vector's physical displacement is not multiplied to invent faster/slower surface motion; only selection between two candidates changes. Calling code must gate invisible material, invalid correspondence and ambiguous object ownership. The test verifies the GPU weights at E=0, 0.5 and 1; object-edge extraction and candidate selection currently run on the CPU using explicit fixture masks.

## Boundary data and runtime requirements

The reference keeps each object's raster coverage separate before any transparent-layer union. Two triangles submitted as separate draws share one object record, so their internal seam does not become an object outline. The second low-opacity object remains represented even underneath the first transparent pane. Pixel-shader discard holes also remain represented. The raster fixture's material shader is synthetic; it does not reproduce Cyberpunk's discard, refraction, animation or material semantics.

Production acquisition must retain object/render-instance identity, matching current/previous geometry, depth and surviving material coverage before those identities are merged. A PSO or material ID identifies a shared rendering program, not an individual object. One draw can contain several instances, and one object can use several draws. `SV_InstanceID` and a draw number alone are not stable cross-frame engine object identity. The actual transparent engine bindings and previous geometry values have not yet been captured/validated.

An ordinary nearest-surface ID buffer is insufficient when a foreground pane covers another transparent object's outline. Keep layer/instance information until each boundary has been determined. A frame-local diagnostic key can verify this extraction without claiming a persistent identity. The fixture's separate full-size per-object render targets are a reference, not a production storage/cost recommendation.

Silhouette extraction and opaque visibility are separate. The opaque-occlusion case first determines each object's geometric outline, then masks visibility with an explicitly supplied depth field. It does not turn an occluder's cut through a pane into a new outline that moves with the pane. This visibility comparison is CPU-only in the fixture. Actual opaque depth/ordering and material-dependent coverage still require runtime verification.

Preserve object ownership when mapping edges to a lower-resolution MV grid: do not independently average IDs, depths and velocities. When two visible boundaries with different motion share one sample, a single vector cannot encode both exactly. The conflict case explicitly reports and bypasses that unresolved condition. Additional layer handling or output recomposition remains a possible route; this test does not select it as a finished solution.

## Run and interpretation

From an x64 Visual Studio developer PowerShell:

```powershell
./OptiScaler/framegen/glass/tests/build_object_motion.ps1
python ./OptiScaler/framegen/glass/tests/check_object_motion.py `
  --executable ./artifacts/glass-object-motion/ObjectMotion.exe `
  --output ./artifacts/glass-object-motion-reference
```

The Python checker additionally needs NumPy, OpenCV and Pillow. The output directory must be new. The runner uses an independent D3D12 device, caller-owned 288-byte transform records and matched current/previous vertices. It does not attach to the game, load FG, or write game settings. Source and outputs are separate from the default standalone suite.

Eleven cases ran at 384x216 and 517x293: static, camera-only, object translation, object rotation, vertex deformation, combined camera/object/deformation, jitter-only, previous position outside the viewport, invalid previous projection, conflicting coincident edges, and opaque occlusion. The odd width also exercises padded GPU readback rows.

The CPU reference intersects camera rays with world-space triangles in double precision, obtains barycentric material correspondence, and projects the corresponding previous geometry independently of GPU interpolants. Across 1,769,686 checked samples, maximum motion error was 0.000983 output pixels. All covered samples found a matching reference triangle. This is a bounded reference check, not an assertion of mathematical exactness for arbitrary content. Material UV interpolation has a separate 1e-4 tolerance for rasterizer setup versus unsnapped ray geometry; its maximum observed residual was 0.0000326. The initial overly tight 2e-5 UV threshold was rejected after measuring that difference; the independent 0.003-pixel MV threshold was not relaxed.

In the combined case, merging the masks lost 310/422 boundary pixels at the two sizes. Separate masks retained them. The CPU edge selection copied the GPU object vectors bit-for-bit on 25,668 admitted boundary samples across all cases; exterior background vectors remained exact. GPU edge weights were 1 for objects with opacity 0.08/0.12. Camera-only motion differed from the combined-geometry reference by up to 45.52 pixels at the 95th percentile in the 517x293 combined fixture. Coincident conflicting edges (2,263 samples) were reported and bypassed, not counted as corrected. The oracle opaque-depth mask prevented 682 occlusion-cut samples from being mistaken for intrinsic silhouettes.

The comparison sheets were inspected directly. Their moving shapes are synthetic panes, not game objects, and the MV preview is an input-field illustration, not an FG-generated image. No new FG output, performance bound, driver compatibility or game quality acceptance follows from these results. The arithmetic is small; acquiring per-object geometry/material metadata can dominate cost and has no verified 1 ms bound.

Local evidence: workspace `work/glass-object-motion-raster-final/` and `work/glass-object-motion-raster-final-517x293/`, each with `audit.json`, source/binary hashes, input records, GPU outputs and comparisons. Raw game inputs are not used or distributed.

## Official contracts

- [HLSL interpolation modifiers](https://learn.microsoft.com/en-us/windows/win32/direct3dhlsl/dx-graphics-hlsl-struct): default perspective interpolation and integer identity interpolation requirements.
- [System-value semantics](https://learn.microsoft.com/en-us/windows/win32/direct3dhlsl/dx-graphics-hlsl-semantics): instance/primitive identifiers are pipeline values, not engine object-history guarantees.
- [DrawInstanced](https://learn.microsoft.com/en-us/windows/win32/api/d3d12/nf-d3d12-id3d12graphicscommandlist-drawinstanced): per-draw instances and vertex input semantics.

The geometry, identity and boundary policies above are this project's proposed contract, not extra guarantees supplied by these APIs or DLSS-G.

## Subsequent FG experiment

[BoundaryFG.md](replay/BoundaryFG.md) records the later actual-provider tests with camera/object motion and 1/2/4-pixel inner bands, followed by replay of actual Cyberpunk HUDless/Backbuffer. The synthetic edge/depth candidate improves some edge scores but worsens interiors. Actual recorded frames retain ghosting. The game recording lacks the per-object previous geometry/coverage used by this reference; it tests a static-world depth-boundary approximation rather than proving the full engine input path.
