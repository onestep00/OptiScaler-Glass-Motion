# Engine render packets and draw ownership

- Created: 2026-09-11
- Updated: 2026-09-11
- Status: production callback tests and full Release build passed; live provenance unverified
- Deployment: none
- Deprecated: no
- Scope: direct indexed mesh batches in the audited Cyberpunk executable; broader routes remain incomplete

The renderer writes the proxy registry index into bits 0..17 of the second 64-bit draw-packet word. Bits 33..49 identify a 48-byte packed transform in the renderer's frame array. The inspected packet producer reads these values directly from the proxy and allocation result. The consumer at RVA `1f1208` resolves the same registry slot and transform entry before appending instances. This provides direct provenance that a mesh/pose search cannot establish.

The append path `1f1a88` copies transforms into separate rigid (48-byte) and skinned (64-byte) arrays. The skinned record adds the engine's four actual deformation parameters. Flush functions `1f1fa8` and `1f020c` pass these arrays through `1f3e40`, then call `1f2098`. The latter invokes the actual COM `DrawIndexedInstanced` at return RVA `1f22c2`. The instance start is the upload allocator's returned element index. A separate rigid route uses an already populated global instance buffer and must match the packet's contiguous transform indices.

`CyberpunkDraws` observes these original operations. It does not replay draws or copy GPU buffers. A fixed pool stores packet intervals during the enclosing renderer call, preserving intervals whose identities are rejected. Registry slot/address, registered lifetime generation, frame, mesh, array count, stride and upload result must agree. Borrowed draw views exist only during the original flush call and require the exact audited public-draw call site and arguments.

The current identity admission is limited to single-instance mesh-proxy packets. Several such objects can still be combined in one indexed draw. A packet containing an internal reorderable instance array retains its interval with no admitted identity. Particle/procedural births, topology/mesh revisions, non-instanced draws, indirect draws and other render loops still require explicit treatment. A packet is not proof of transparency or of contribution to the FG color; the material/PSO and frame/queue consumer must establish those separately.

The source startup adapter now installs the audited engine observation paths after object-lifetime initialization. The public draw consumer has not been connected. The new acquisition has not been observed in a fresh game process and is not yet an FG correction. The geometry shader, GPU allocation/lifetime, per-object contour composition and actual FG substitution remain separate unfinished work. See [EngineGeometry.md](../EngineGeometry.md) and [GeometryShaders.md](GeometryShaders.md).

`CyberpunkDrawCallbacks.cpp` includes the production callbacks and invokes them with original-function substitutes and owned renderer/proxy layouts. It passed coincident objects, changed batching order, rigid/skinned copies, rejected multi-instance intervals, contiguous/noncontiguous global ranges, failed upload, registration-slot reuse, crossed frame, one-use borrowed views and fixed-pool exhaustion. All 16 original appends and nine original flushes were forwarded exactly once. It adds no game hook and does not test GPU binding or actual game call frequency. The complete Release x64 OptiScaler solution also built successfully.

The callback pool has 16 independent render scopes and 2,048 packet spans per rigid/skinned batch. It allocates once at startup, clears counters between batches and bypasses overflow without dropping unknown intervals. It introduces no GPU readback, wait, command or shader compilation. These arithmetic/storage limits are not a measured performance guarantee.

Microsoft's [DrawIndexedInstanced contract](https://learn.microsoft.com/en-us/windows/win32/api/d3d12/nf-d3d12-id3d12graphicscommandlist-drawindexedinstanced) defines the start-instance offset into instance vertex data. The adapter preserves all original arguments and results.

`startInstanceLocation` in the borrowed draw view is the IA buffer offset. It must not be copied into the shader-history constant's raw `SV_InstanceID` origin. The GPU fixture deliberately uses nonzero IA offsets and tests the raw system values independently.
