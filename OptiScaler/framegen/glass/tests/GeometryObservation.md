# Original-only depth-writing pipeline observation

- Created: 2026-09-11
- Updated: 2026-09-11
- Status: explicit native vertex preparation and replaceable-DLL request passed independent host checks; live capture incomplete
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

Original root creation also records up to 128 root layouts in the same CPU
payload budget. The official versioned deserializer converts to the 1.1 layout;
parameter and descriptor-range arrays are copied before releasing it. No extended
root is created for this observation. Unobserved roots remain layout-unknown.
Shader register numbers and spaces come from these original descriptors, not
assumed fixed root-slot numbers. Exact post-override serialized creation bytes
and the original node mask are also retained. These preserve static samplers,
flags and version information absent from the parameter-only view. Serialized
storage is included in the bounded cache accounting. See Microsoft's [deserializer contract](https://learn.microsoft.com/en-us/windows/win32/api/d3d12/nf-d3d12-d3d12createversionedrootsignaturedeserializer).

The census uses `FindObservedGeometryPipeline`; correction still uses only
`FindGeometryPipeline`. Original-only entries have no instrumented PSO or extended
root and report `rootReplayable=0`. The existing retained-token view exposes the
original descriptor with a null extended root.
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

The later `GLASS_OBSERVATION_HOST` test uses production creation/command hooks on
its independent device. An actual zero-count DrawInstanced delivers the original
depth-writing descriptor through census, with rootReplayable false and no entry
in the correction lookup. Stop removes observation lookup access. Root CBV b7
and table SRV t10 survive destruction of the serialized source bytes, including
owned range pointers. This test records but does not submit/rasterize that draw.

`GraphicsRootBindings::observe` borrows a single current root-slot value without
allocation or buffer copying. Tests cover unset slots, CBV address, heap-change
invalidation of tables only, partial constant masks, invalidation and Reset.
The returned GPU address cannot be dereferenced as a CPU pointer and establishes
neither a live resource lease nor its execution-time contents. Constants are
borrowed only for the synchronous callback. Live native previous-transform
capture remains incomplete.

The module-facing draw input exposes a callback-scoped `bindingAt` accessor.
It copies only one requested root-slot metadata record; unset constant words are
zero and their known-bit mask is explicit. Retained pipeline views expose owned
original parameter/range layouts. The host test binds a real upload buffer to
root slot 0 and verifies its exact GPU address and original register mapping b7.
No buffer contents are copied or inferred.

Nested ABI versions are draw 4, census 2 and capture 3. Pipeline-view size also
changes; old exact-size/version readers reject these payloads. Rebuild experiment
modules for this host. Saved census/capture snapshots clear the borrowed binding
source and function pointer along with their other callback-only pointers.
The final versioned source passed the host observation test and full Release
solution build. The updated coverage experiment DLL also compiled with /W4 /WX;
it was not loaded into the older running host.

Next: verify census delivery from the deployed host, identify native velocity
outputs from shader dataflow, and obtain actual current/previous bound transforms
and deformation data. Descriptors alone do not provide buffer contents or GPU
resource lifetime. Do not substitute a native velocity VS onto transparent draw
bindings without checking those inputs.

## Replaceable binding recorder

`ExperimentBindingsModule.cpp` is a census-only DLL. Its adjacent `.config`
contains one UTF-8 line naming a fresh absolute output directory whose parent
already exists. It retains at most 256 unique pipeline tokens and 2,048 draw
records, with approximately 35.1 MiB of fixed row storage. Locks are try-only
in the callback; overflow/contention increments a drop counter. New pipeline
tokens allocate once per admitted pipeline. There are no GPU commands, buffer
readbacks or callback file writes. Retained shader/root references add storage
beyond the fixed rows. This is an on-demand diagnostic, not the runtime algorithm.

After callbacks stop, destroy writes draws/bindings/constants/layout CSV files
and original VS/PS bytecode plus available serialized roots as `.root.bin`.
`bindings.done` records counts, drops, CPU ordering,
unknown frame-zero semantics and absence of previous-transform verification.
GPU virtual addresses and descriptor handles are observations, not retained
buffer leases. No generated-frame or silhouette correctness follows from them.

The independent host test with `GLASS_OBSERVATION_MODULE` loads the actual DLL,
passes a real draw callback, stops the creation cache, saves through module
destruction, checks the exact CBV address and completion counts, then unloads.
It passed. An initial test failure was Windows CRLF comparison in the test;
normalizing line endings resolved it without changing the recorded address.
The module compiled with /W4 /WX. No game deployment occurred.

The serialization extension passed the independent module test after rebuilding
all host translation units: the retained bytes survive mutation of the source
blob, and the actual DLL saves byte-identical root data after cache shutdown.
This verifies ownership and transport, not replay or previous-pose validity.
The appended pipeline-view fields require rebuilding size-checked modules.
The full Release x64 solution compiled and linked after this extension with exit
code 0. Existing warnings and post-build missing-file messages remain as above;
the installed game DLL was not replaced.

The remaining native-pass capture gap is explicit: `GeometryCommands` calls the
capture owner only for compiler-prepared entries, and `GeometryPreparedDraw`
binds that entry's extended root/history slots. Original-only observations cannot
use this path until explicitly prepared as described below. Census callbacks must
not issue GPU commands. Original replacement still needs checked state restoration
and the existing completion plus recording-discard lifetime gates before admission.

## Explicit native vertex preparation

`requestVertexCapture` in the retained pipeline view accepts the retained token
on a worker/control thread. The resident service resolves the same original COM
PSO in its observation cache, then queues bounded compilation using the preserved
original descriptor/root. It does not dereference GPU addresses, wait for the
compiler, or authorize rendering. Ordinary creation still uses the existing
material eligibility rules; there is no automatic compilation of all native PSOs.
Repeated requests reuse the queued entry and do not copy shaders again. Failed
compilations remain rejected; a successful request is not readiness proof.

Successful native preparation publishes an immutable `vertexOnlyCapture` entry
through the existing prepared lookup. A subsequent indexed engine-mapped draw can
therefore reach the normal capture owner. The original PS and depth/blend state
are preserved; this mode replaces the original draw once. The owner still needs
instance mapping, valid raster state, resources and lifetime admission. Material
coverage compilation continues to reject writable depth. No new automatic FG
substitution follows from preparation.

The binding recorder accepts an optional second config line:

```text
prepare-vertex-v1 <current-process-id> <observed-pipeline-identity>
```

It retains the selected pipeline once and wakes a single worker to make the
request. Other observed pipelines remain read-only. Status records the selected
identity, request result and a subsequently observed prepared identity matched by
the original PSO, not a reused ordinal. Preparation changes the cache identity;
use the recorded prepared identity when selecting the later vertex capture. The
worker is joined before saving/releasing tokens. No GPU command or game buffer
copy occurs in this recorder. This selector is process-local diagnostic data,
not a production material whitelist.

`GLASS_OBSERVATION_NATIVE` adds DXIL VS/PS fixtures compiled from
`GeometryObservationVertex.hlsl` and `GeometryObservationPixel.hlsl` into
`artifacts/glass-tests/observation-native-vs.dxil` and `observation-native-ps.dxil`.
With the existing HOST and MODULE defines, the actual DLL's worker queues native
preparation. The test waits on its own control thread, verifies deduplication and
unchanged original PS bytes, and observes the prepared entry at the production
indexed hook's capture owner using synthetic engine identities. The owner declines
replacement; this test never submits that draw. It then checks stopped-host
rejection, two recorded pipeline states, saved root bytes and module unload.

The test passed with `NATIVE_REQUEST_OK` and `BINDING_MODULE_OK`. The initial mixed
DXIL-VS/legacy-PS fixture failed PSO creation; using DXIL for both stages resolved
the fixture failure. Actual Cyberpunk previous transforms, draw coverage, GPU
capture and FG quality remain unverified by this test. Pipeline-view size changed;
rebuild modules before loading them into the new host.
