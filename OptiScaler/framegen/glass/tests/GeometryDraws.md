# Engine render packets and draw ownership

- Created: 2026-09-11
- Updated: 2026-09-11
- Status: production engine callbacks and public D3D12 consumer pass independent tests; live provenance unverified
- Deployment: acquisition build 32054cc staged in MO2 Root; fresh-process verification pending
- Deprecated: no
- Scope: direct indexed mesh batches in the audited Cyberpunk executable; broader routes remain incomplete

The renderer writes the proxy registry index into bits 0..17 of the second 64-bit draw-packet word. Bits 33..49 identify a 48-byte packed transform in the renderer's frame array. The inspected packet producer reads these values directly from the proxy and allocation result. The consumer at RVA `1f1208` resolves the same registry slot and transform entry before appending instances. This provides direct provenance that a mesh/pose search cannot establish.

The append path `1f1a88` copies transforms into separate rigid (48-byte) and skinned (64-byte) arrays. The skinned record adds the engine's four actual deformation parameters. Flush functions `1f1fa8` and `1f020c` pass these arrays through `1f3e40`, then call `1f2098`. The latter invokes the actual COM `DrawIndexedInstanced` at return RVA `1f22c2`. The instance start is the upload allocator's returned element index. A separate rigid route uses an already populated global instance buffer and must match the packet's contiguous transform indices.

`CyberpunkDraws` observes these original operations. It does not replay draws or copy GPU buffers. A fixed pool stores packet intervals during the enclosing renderer call, preserving intervals whose identities are rejected. Registry slot/address, registered lifetime generation, frame, mesh, array count, stride and upload result must agree. Borrowed draw views exist only during the original flush call and require the exact audited public-draw call site and arguments.

The current identity admission is limited to single-instance mesh-proxy packets. Several such objects can still be combined in one indexed draw. A packet containing an internal reorderable instance array retains its interval with no admitted identity. Particle/procedural births, topology/mesh revisions, non-instanced draws, indirect draws and other render loops still require explicit treatment. A packet is not proof of transparency or of contribution to the FG color; the material/PSO and frame/queue consumer must establish those separately.

The source startup adapter now installs the audited engine observation paths after object-lifetime initialization. `GeometryCommands` consumes the borrowed view from the actual public indexed draw and joins it to the observed PSO and graphics root bindings. It adds no GPU command or replacement. The new acquisition has not been observed in a fresh game process and is not yet an FG correction. GPU allocation/lifetime, per-object contour composition and actual FG substitution remain unfinished. See [EngineGeometry.md](../EngineGeometry.md) and [GeometryShaders.md](GeometryShaders.md).

`CyberpunkDrawCallbacks.cpp` includes the production callbacks and invokes them with original-function substitutes and owned renderer/proxy layouts. It passed coincident objects, changed batching order, rigid/skinned copies, rejected multi-instance intervals, contiguous/noncontiguous global ranges, failed upload, registration-slot reuse, crossed frame, one-use borrowed views and fixed-pool exhaustion. All 16 original appends and nine original flushes were forwarded exactly once. It adds no game hook and does not test GPU binding or actual game call frequency. The complete Release x64 OptiScaler solution also built successfully.

The callback pool has 16 independent render scopes and 2,048 packet spans per rigid/skinned batch. It allocates once at startup, clears counters between batches and bypasses overflow without dropping unknown intervals. It introduces no GPU readback, wait, command or shader compilation. These arithmetic/storage limits are not a measured performance guarantee.

Microsoft's [DrawIndexedInstanced contract](https://learn.microsoft.com/en-us/windows/win32/api/d3d12/nf-d3d12-id3d12graphicscommandlist-drawindexedinstanced) defines the start-instance offset into instance vertex data. The adapter preserves all original arguments and results.

`startInstanceLocation` in the borrowed draw view is the IA buffer offset. It must not be copied into the shader-history constant's raw `SV_InstanceID` origin. The GPU fixture deliberately uses nonzero IA offsets and tests the raw system values independently.

## Public graphics observation and bounded reuse

The observer installs from the selected actual device implementation. Successful creation/Reset starts a fresh recording; Close ends readable state. It tracks all graphics root setters, partial constants, PSO and descriptor-heap changes. Bundle/indirect execution and extended state-object/program setters invalidate replay. Official destruction notifications prevent a reused command address from inheriting bindings. This is CPU observation, not GPU resource retirement proof. Render targets, predication, viewport, render-pass state and submission ownership still require the capture owner.

A fixed 512-record table allows at most 32 probes per lookup. Registration takes a mutex; ordinary setters do not. COM implementation checks are cached for each live command lifetime, and exact repeated heap lists reuse their immutable type classification. No shader analysis, compilation, GPU wait, readback or file I/O occurs in the draw callback. Existing shader cache limits remain 128 roots, 2,048 pipelines and 128 MiB of copied CPU data; these are not a driver PSO/VRAM bound or a measured frame-time guarantee.

The independent `GeometryInstances --commands` fixture uses the production public hooks with test-owned packet identities. All 40 actual indexed calls forwarded, with 24 borrowed identity/pipeline/root matches. It deliberately switches/restores roots, including separately set constants. All 143,360 original color pixels and the existing 3,563 accepted motion samples still pass. It installs no game hooks and does not prove actual engine packet frequency. The full Release x64 solution also builds successfully.

Binding invalidation follows Microsoft's [root signature semantics](https://learn.microsoft.com/en-us/windows/win32/direct3d12/using-a-root-signature), [bundle state inheritance](https://learn.microsoft.com/en-us/windows/win32/api/d3d12/nf-d3d12-id3d12graphicscommandlist-executebundle) and [descriptor heap contract](https://learn.microsoft.com/en-us/windows/win32/api/d3d12/nf-d3d12-id3d12graphicscommandlist-setdescriptorheaps).

Root binding reuse now keeps a 64-bit written-slot mask. Reset or a changed root invalidates this mask without clearing the 64 per-slot constant arrays. An address/table setter updates only its scalar payload; replay and validation visit the written slots. Partial constants retain a separate bit mask, so stale payload bytes cannot become valid through a root/heap change. This reduces deterministic memory writes without changing the fixed cache size or claiming a measured speedup.

The production `--commands` GPU test still passes all 143,360 original pixels, 3,563 MV samples and 24 draw/root joins after this change. Additional checks reject stale constant ranges and foreign slots, preserve bindings on a redundant root call, invalidate only tables on heap changes, and clear prior invalidation on Reset. The full Release x64 solution also builds successfully. This optimization is source-only at this checkpoint; staged build 32054cc predates it.

On 2026-09-11, after a successful quicksave, normal game exit and RootBuilder Sync/Clear, the full Release 32054cc DLL was staged as `overwrite/Root/bin/x64/dxgi.dll`. Its SHA-256 is `a5b3a6a42a5759a28f06a266b2566172fcc9d9bfe04d97f5246d025375024904`. Both HLSL files and the pinned `Glass/dxcompiler.dll` and `Glass/dxil.dll` were copied with hash verification. Settings, MFG ASI and `version.dll` hashes stayed unchanged; the existing diagnostic ASI exits before hook installation because its completed marker remains present. The previous files and a deployment manifest were backed up locally. The user will launch the game. This is staging evidence, not a live engine join or new FG-input substitution.
