# Original-only depth-writing pipeline observation

- Created: 2026-09-11
- Updated: 2026-09-11
- Status: implemented; independent D3D12 descriptor/lifetime test passed
- Deployment: not installed in the running game
- Deprecated: no
- Scope: bounded diagnostic census of VS/PS triangle graphics pipelines that write depth

`GeometryObservationCache` retains original PSO/root references and owns the VS,
PS and input-layout bytes from successful public PSO creation. It makes no GPU
buffer, modified shader, extended root or command. Names, hashes and render-target
indices do not classify these observations as velocity or transparent materials.

The separate cache admits at most 1,024 entries and 32 MiB of accounted CPU
payload. Container/allocator and driver-owned COM storage are additional; this
is not a total process or VRAM bound. Exhaustion rejects new observations and
does not evict active leases or consume the existing material compiler's budget.
Unsupported shader stages and pipeline streams remain unobserved.

The census uses `FindObservedGeometryPipeline`; correction still uses only
`FindGeometryPipeline`. Original-only entries have no instrumented PSO or extended
root and report `rootReplayable=0`. The existing retained-token view exposes the
original descriptor with a null extended root. No experiment ABI size changed.
Capture modules must continue to require their actual extended layout.

`GeometryObservation.cpp` creates real independent D3D12 roots/PSOs and verifies
owned shader bytes after source mutation, duplicate admission, entry/byte bounds,
rejection of read-only depth passes, absence of replay objects and descriptor
lease survival after cache destruction. It does not execute a velocity shader,
observe game bindings, validate N-1 history or replace an FG input.

Build the single test with MSVC C++20, linking d3d12, dxgi, d3dcompiler and dxguid.
Run the resulting executable without arguments. The observed result was
`GEOMETRY_OBSERVATION_OK owned_bytes bounded_cache original_only retained_after_cache`.

The complete Release x64 solution also compiled and linked with exit code 0.
Existing XeSS inheritance and linker alignment/default-library warnings remain.
Post-build packaging printed missing file/path messages; this is a DLL build
result, not verification of a complete distributable package.

Next: verify census delivery from the deployed host, identify native velocity
outputs from shader dataflow, and obtain actual current/previous bound transforms
and deformation data. Descriptors alone do not provide buffer contents or GPU
resource lifetime. Do not substitute a native velocity VS onto transparent draw
bindings without checking those inputs.
