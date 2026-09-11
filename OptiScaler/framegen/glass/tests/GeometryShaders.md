# Actual vertex-output history and material motion

- Created: 2026-09-11
- Updated: 2026-09-11
- Status: independent GPU implementation verified; runtime acquisition/integration incomplete
- Deployment: none; this is not yet called by the game host
- Deprecated: no
- Scope: instrumenting supported DXIL VS/PS 6.0 shaders without replacing their geometry or material math

## Implemented source

`DxilVertexHistory.cpp` rewrites DXC disassembly, which the caller must assemble and validate before creating a pipeline. It preserves the original vertex outputs and records the actual computed clip position in a caller-owned history buffer. A generation and expected-frame tag reject stale or unrelated entries. New varyings expose the previous position and a missing-history flag. This includes transformations and deformations already computed by the original vertex shader; it does not infer motion from scene pixels or duplicate a skinning algorithm.

`RewriteMaterialMotion` preserves the original material computation/discard and emits normalized previous-minus-current motion, mean RGB attenuation, and current device depth into a separate target. Material opacity is derived from the verified destination blend factor; colored transmission becomes `1 - mean(saturate(T.rgb))`. A scalar coefficient cannot fully represent colored/refraction layers. Pixels contributing neither source color nor attenuation are excluded. Invalid history and nonfinite motion are rejected. Original color-output stores are replaced only in this separate diagnostic/capture PSO; this shader must never replace the game's color pixel shader as-is.

All changes are confined to this module. `GlassFg.props` compiles the rewriter, but no production call site currently invokes it. The original static-world correction remains installed.

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

The test found that an interpolated valid value of one becomes `0.999999940395` at some pixels. Comparing it exactly to one created coverage holes. The implementation instead interpolates a missing-history flag: zero means valid. Zero interpolation is exact, and a nonzero contribution from a vertex with missing history rejects the sample. This avoids loosening the validity threshold.

The nonzero draw start offsets also exposed an incorrect initial test assumption. IA offsets are distinct from the shader's vertex/instance system values. The final test uses the actual raw-index/instance origins and verifies captured slots against stream output. See [Microsoft's extended command information specification](https://microsoft.github.io/hlsl-specs/proposals/0015-extended-command-info/).

Local, unpublished game shader inputs were also processed: 17 vertex shaders (including 11 observed transparent variants) and four material pixel shaders assembled and passed DXIL validation. Pixel output initialization followed by an unconditional final overwrite is supported; branch-local exports are rejected. Validation of those shaders is not proof that their complete live root bindings or frame histories have been acquired.

## Runtime contract still required

- Stable live object/generation/chunk/vertex identity. A changing instance batch, reused resource address, particle birth/death, or topology change must not inherit another element's history.
- Both history allocations validated with `VertexHistoryConstants::valid`, immutable per-recording data, real queue ordering, GPU completion, and resize/cut invalidation.
- Actual rendering provenance excluding HUD; original read-only depth/stencil semantics; complete root/state restoration and shader compatibility checks.
- Separate per-object coverage before detecting boundaries. A union mask loses outlines behind other transparent objects.
- Correct FG frame, jitter convention, viewports, and resource-scale mapping, then actual FG input replacement.

The separate material pass is useful for validation but would duplicate material shading. A proposed runtime path would append capture writes to the original color shader, using a bounded per-object target and rasterizer-ordered access. That path is not implemented here. It must preserve original color exports/discard and only use early depth/stencil when original depth/stencil writes are absent. See [Microsoft's ROV ordering contract](https://learn.microsoft.com/en-us/windows/win32/direct3d12/rasterizer-order-views) and [early depth/stencil semantics](https://learn.microsoft.com/en-us/windows/win32/direct3dhlsl/sm5-attributes-earlydepthstencil). No live performance or ghosting-improvement claim follows from this independent test.
