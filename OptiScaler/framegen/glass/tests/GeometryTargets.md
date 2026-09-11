# Render-target binding provenance

- Created: 2026-09-11
- Updated: 2026-09-11
- Status: public descriptor/binding observations and module output pass independent checks; live validation incomplete
- Deployment: none
- Deprecated: no
- Scope: observed RTV/DSV heaps, view creation/copy and direct OM bindings on the selected actual D3D12 device

`GeometryViews` observes the actual device's public CreateDescriptorHeap,
CreateRenderTargetView, CreateDepthStencilView, CopyDescriptors and
CopyDescriptorsSimple methods. No executable or driver byte signature is used.
It is enabled at startup only in explicit experiment-host mode. Other descriptor
types retain their original path without entering this registry.

The registry stores numeric resource identities, allocation descriptors, exact
explicit view descriptors and default/null-descriptor flags. A private GUID's
eight-byte application tag distinguishes resources independently of reused COM
addresses. It does not change resource contents or retain a resource reference.
Heap identities use the existing official destruction-notification token, now
also accepting descriptor heaps. Destroyed or unobserved heaps do not resolve.
Unknown descriptor sources invalidate their destinations; observation exceptions
disable future lookup rather than exposing possibly stale entries.

`GeometryRasterState` copies immutable metadata references during the actual
OMSetRenderTargets callback. Later overwriting/copying/freeing the CPU descriptor
heap cannot change that recording's original binding snapshot. Lookup contention
returns unknown. Metadata snapshots retain no heap or texture COM reference and
do not authorize later pointer dereferences, GPU copies or new resource use.
Allocation, camera/view identity, subresource state and FG correlation are distinct.

Limits are 2,048 tracked live heaps and 131,072 descriptor entries. Dead descriptor
entries are reclaimed when the table reaches its limit; heap creation does not
scan the entire descriptor table. The registry performs CPU allocations at view
writes/copies, not GPU image copies. Bound snapshots can outlive current table
entries. These are diagnostic limits, not a measured production frame-time claim.

Draw payload version 3 adds callback-scoped `targetAt`: indices 0..7 are RTVs and
8 is the DSV. Capture payload version 2 carries this nested draw contract. The
coverage worker saves nine sized `GlassExperimentTarget` records in `.targets.bin`
and readable identities/allocation fields plus exact descriptor DWORDs in
`.targets.csv`. Explicit descriptor bytes retain mip, array/volume slice, plane
and depth/stencil flags. A default descriptor has zero explicit descriptor bytes;
its default flag and original allocation are preserved. Missing entries have
size zero. `.draw` reports the observed-binding count and OM observation point.
It still does not claim camera/view identity or topology/history proof.

The control response exposes target-view hook/health and lookup/miss counters.
`GEOMETRY_VIEWS` also reports heap creations, view writes and descriptor copies.
Hook success and nonzero lookup counts do not establish complete world coverage.

`GeometryTargetViews` installs the production public hooks on an independent
device. It verifies distinct same-sized resources, several views of one resource,
mip/array slices, DSV flags, default/null views, both copy APIs, unknown-source
invalidation and exact borrowed-ABI bytes/bounds. An actual OM binding remains
unchanged after its descriptor is overwritten. Destroyed heaps stop resolving;
an original texture really destroys while its numeric metadata remains retained.

`GeometryInstances --controlled-recorder` additionally checks the saved color and
depth resource identities/handles against the exact resources used by the captured
draw in both module generations. Its original 143,360 color pixels and 107,520
material samples still pass. `--capture-module` also retains the existing 3,563
actual shader-MV reference checks. The full Release build passes. These fixtures
do not inspect game resources or fix the earlier four-pixel live capture.

The API basis is Microsoft's [non-shader-visible descriptor lifetime](https://learn.microsoft.com/en-us/windows/win32/direct3d12/non-shader-visible-descriptor-heaps),
[RTV defaults](https://learn.microsoft.com/en-us/windows/win32/api/d3d12/nf-d3d12-id3d12device-createrendertargetview),
[DSV defaults](https://learn.microsoft.com/en-us/windows/win32/api/d3d12/nf-d3d12-id3d12device-createdepthstencilview),
[copy semantics](https://microsoft.github.io/DirectX-Specs/d3d/ResourceBinding.html#copying-descriptors)
and [application private data](https://learn.microsoft.com/en-us/windows/win32/api/d3d12/nf-d3d12-id3d12object-setprivatedata).
The copy observation assumes valid, nonoverlapping source/destination ranges as
required by the API. It does not validate undefined application descriptor operations.

Remaining gates: full draw/missing-packet census, non-OM/render-pass routes, live
target correspondence, complete per-object material aggregation, previous/current
geometry, actual FG frame/coordinate transport and generated-frame quality.
