# Actual vertex-output history and material motion

- Created: 2026-09-11
- Updated: 2026-09-11
- Status: independent GPU and public creation observer verified; engine acquisition/draw integration incomplete
- Deployment: none; startup creation adapter added to source, with no new game draw or FG substitution
- Deprecated: no
- Scope: instrumenting supported DXIL VS/PS 6.0 shaders without replacing their geometry or material math

## Implemented source

`DxilVertexHistory.cpp` rewrites DXC disassembly, which the caller must assemble and validate before creating a pipeline. It preserves the original vertex outputs and records the actual computed clip position in a caller-owned history buffer. A generation and expected-frame tag reject stale or unrelated entries. New varyings expose the previous position and a missing-history flag. This includes transformations and deformations already computed by the original vertex shader; it does not infer motion from scene pixels or duplicate a skinning algorithm.

`RewriteMaterialMotion` preserves the original material computation/discard and emits normalized previous-minus-current motion, mean RGB attenuation, and current device depth into a separate target. Material opacity is derived from the verified destination blend factor; colored transmission becomes `1 - mean(saturate(T.rgb))`. A scalar coefficient cannot fully represent colored/refraction layers. Pixels contributing neither source color nor attenuation are excluded. Invalid history and nonfinite motion are rejected. Original color-output stores are replaced only in this separate diagnostic/capture PSO; this shader must never replace the game's color pixel shader as-is.

The algorithm and creation observer remain in this module. The upstream D3D12 device hook has explicit startup and final-root-creation integration calls. The original static-world correction remains installed; the new source has not been deployed.

`OriginalColorAndCapture` preserves the original color exports and appends a rasterizer-ordered raw-buffer capture in the same draw. A bounded per-object rectangle stores surface motion/depth and RGB transmission without a second material evaluation. Original discard still executes. Missing history or an out-of-range capture address skips only the added storage. The host must validate `MaterialCaptureConstants` against the actual allocation and prove the original pass has read-only depth/stencil before using its early-depth variant.

`GeometryPipeline.cpp` creates a separate extended root and validated VS/PS pipeline. It preserves original parameters, ranges, flags and static samplers. Added parameters cost 16 DWORDs: a 36-table game layout fits in 52 DWORDs. Material data uses a root CBV instead of 16 inline constants. Collisions in register space 31 and roots exceeding the hardware budget are rejected. `GraphicsRootBindings.h` restores observed original root values after insertion, including partially set constants; unknown command state must bypass insertion. Creation and shader compilation belong on a worker, never in a draw callback.

VS and PS signature extents can differ. The recorded VS-only `SV_ClipDistance` made independently appended history varyings occupy different registers; all 68 original shader/input/root combinations initially failed modified PSO creation despite passing individual DXIL validation. `GeometryCompiler` now passes the actual rewritten VS register to the PS rewriter. All 68 combinations then passed creation on an independent NVIDIA device. Missing rasterizer/alpha-blend fields in that local audit used neutral values, so this is shader/root linkage evidence rather than a complete original-game PSO replay. The public fixture now includes an unused VS-only clip-distance output to cover this failure.

`CyberpunkCamera.h` decodes an already identified 848-byte camera constant block and projects engine bounds to a storage rectangle. Bounds never define material coverage or motion. The adapter still has to prove b1 binding, executable layout, recording/frame identity, stable upload bytes and conservative bounds. The producer must detect coverage escaping the rectangle and reject that object. An eye-plane crossing uses a budget-dependent full-viewport fallback.

`GeometryInstance.h` adds an immutable per-instance mapping from a draw to independently owned object history and coverage allocations. `PerInstance` shaders carry an uninterpolated map index into the PS, so reordering a batch does not move an object's history or merge its mask with another object. This layout adds one shared root SRV: 18 additional DWORDs, or 54 for the recorded 36-table layout. The original draw is not split. The adapter must supply verified engine identities and nonaliasing allocations; the map does not discover those identities.

An exact all-zero mapping is now an explicit inactive instance. It retains its position in the batch, renders original color, and writes no history, coverage or object-status record. A partially populated zero-generation mapping remains invalid, as does a wholly inactive draw. The VS sends a sentinel map index for an inactive allocation; the PS skips added motion/storage work after that input is available. A valid allocation with a missing vertex still marks the object's rejection status and must not be silently treated as inactive.

Mapped capture atomically marks the object's status if contributing material pixels escape its rectangle, use incomplete history, or have nonfinite motion. A consumer must reject the entire flagged object, including any valid-looking pixels already captured. Status records must be cleared once before that frame's material draws. Atomic OR is required because different screen pixels can update the same object status; ordinary ROV ordering alone is insufficient. These atomics are on rejection paths. Invalid previous vertex tags select the current position as a finite placeholder while retaining the missing-history flag.

`GeometryPipelineCache` owns copied root/shader/input-layout data and original COM identities. A single worker compiles modified pipelines; draw-side lookup returns an immutable shared lease and performs no compilation or driver call. Default limits are 128 roots, 2,048 pipelines and 128 MiB of copied CPU data. The byte limit is not a bound on driver PSO memory. The lease retains all pipeline/root data after the cache stops; a renderer must retain it until GPU completion **and** recording discard.

`GeometryCreation` observes successful public graphics PSO creation on the actual selected device. Root bytes come from each final upstream creation branch, including sampler reserialization. Compiler-generated calls are excluded. Pipeline-stream calls are counted and forwarded, but are currently unsupported by the rewriter. Unknown or over-budget pipelines keep the original rendering path. Callback code remains resident for process lifetime; an owning control thread can stop and join the cache. No static destructor joins a worker under the loader lock. `GeometryHost` admits the inspected executable and packaged DXC pair before starting this observer. It records no draw or FG replacement.

## GPU validation

Run `build_geometry_shader.ps1` from an x64 Visual Studio Developer PowerShell. It uses the repository's pinned DXC binary, generates shaders from the included synthetic HLSL, runs the actual production rewriter/assembler/validator, and creates an independent NVIDIA D3D12 device. It does not attach to or modify a game. It has no Python dependency.

The fixture uses indexed, instanced geometry with nonzero IA start offsets, time-dependent deformation, camera movement, perspective, and a material with both a curved alpha contour and a discarded internal gap. Five frames test warmup, valid history, changed generation, and stale-frame rejection. Stream output independently reads original and instrumented vertex results.

Observed in the current test:

- All 122,880 original color pixels and all 90 emitted current vertex positions remain bit-identical.
- All 36 accepted previous vertex outputs match the prior original vertex outputs exactly.
- All covered/noncovered material pixels agree; invalid-history frames produce no admitted pixels.
- Across 6,017 accepted motion pixels, the maximum difference from double-precision perspective correspondence is 0.001586 pixels (including rasterization precision).
- Material scalar attenuation matches the fixture's original alpha within `3e-7`; history writes leave the allocation exterior unchanged.
- CPU allocation checks reject overflowing instance/address ranges, zero generation, and empty buffers.
- The simultaneous color/capture pipeline preserves all 122,880 original color pixels. Its 6,017 captured motion/depth records exactly match the separate material pass; RGB transmission matches the original material. A padded, offset rectangle leaves storage outside the allocation unchanged.
- A draw after restoring the original root/PSO produces the same original color. The 36-table root test preserves descriptor ranges (including an unbounded range) and rejects collisions/overflow. Analytic camera tests cover fixed origins, jitter signs, offset viewports, perspective bounds, eye-plane crossings and invalid inputs.

The test found that an interpolated valid value of one becomes `0.999999940395` at some pixels. Comparing it exactly to one created coverage holes. The implementation instead interpolates a missing-history flag: zero means valid. Zero interpolation is exact, and a nonzero contribution from a vertex with missing history rejects the sample. This avoids loosening the validity threshold.

The nonzero draw start offsets also exposed an incorrect initial test assumption. IA offsets are distinct from the shader's vertex/instance system values. The final test uses the actual raw-index/instance origins and verifies captured slots against stream output. See [Microsoft's extended command information specification](https://microsoft.github.io/hlsl-specs/proposals/0015-extended-command-info/).

Local, unpublished game shader inputs were also processed: 17 vertex shaders (including 11 observed transparent variants) and four material pixel shaders assembled and passed DXIL validation. Pixel output initialization followed by an unconditional final overwrite is supported; branch-local exports are rejected. Validation of those shaders is not proof that their complete live root bindings or frame histories have been acquired.

`GeometryInstances.cpp` changes the order of three overlapping instances across five frames. It compares each object's stored coverage against a separate original material draw and its motion against independent perspective correspondence using original VS stream output. All 89,600 original color pixels remain exact; 2,399 admitted motion pixels pass with maximum error 0.001688 pixels. The comparison includes 810 overlapping object samples, opaque depth rejection, generation replacement, one missing vertex, escaped bounds and recovery after clearing status. Test identities come from synthetic instance data. These checks do not establish the engine adapter or FG quality.

The subsequent eight-frame fixture also disables one overlapping object's map for a frame. Its old history/pixel bytes and every guard/status record remain unchanged by that object. Returning to an active map rejects absent history, then recovers on the following frame. All 143,360 original color pixels remain exact; 3,563 admitted MV pixels and 896 overlap samples pass, with maximum error 0.001688 pixels. The production creation observer remains enabled for this independent GPU test. This adds no live game or FG quality evidence.

The same test obtains its modified pipeline through the production compiler worker, overwrites the caller's copied shader bytes, stops/destroys the cache, and then renders through the retained lease. Its `--observe` run installs the real public D3D12 creation hooks on the independent device and uses the final-root wrapper. The original color/MV checks still pass. The run also verifies recursive compiler exclusion, unchanged failed creation, forwarding/counting a real pipeline-stream creation, rejecting an over-budget PSO and stopping further admission. It never attaches these hooks to a game. A Release x64 OptiScaler solution build including this adapter passed. The recorded 68 shader/root combinations also pass the mapped 54-DWORD root audit; the same incomplete-original-descriptor limitation applies.

## Runtime contract still required

- Stable live object/generation/chunk/vertex identity. A changing instance batch, reused resource address, particle birth/death, or topology change must not inherit another element's history.
- Both history allocations validated with `VertexHistoryConstants::valid`, immutable per-recording data, real queue ordering, GPU completion, and resize/cut invalidation.
- Actual rendering provenance excluding HUD; original read-only depth/stencil semantics; complete root/state restoration and shader compatibility checks.
- Separate per-object coverage before detecting boundaries. A union mask loses outlines behind other transparent objects.
- Correct FG frame, jitter convention, viewports, and resource-scale mapping, then actual FG input replacement.

The same-draw capture implementation is independently tested but has no production caller yet. ROV ordering applies within one draw; overlapping writes from different draws require a UAV barrier or another proven dependency. The instance fixture verifies separate overlapping objects and opaque depth rejection. It does not test several material fragments accumulating in one object's pixel or overlapping writes from different draw calls. Runtime allocation, engine identity, live ordering and FG consumption remain incomplete. See [Microsoft's ROV ordering contract](https://microsoft.github.io/DirectX-Specs/d3d/RasterOrderViews.html) and [early depth/stencil semantics](https://learn.microsoft.com/en-us/windows/win32/direct3dhlsl/sm5-attributes-earlydepthstencil). No live performance or ghosting-improvement claim follows from this independent test.
