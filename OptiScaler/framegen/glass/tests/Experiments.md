# Dynamically replaceable experiment modules

- Created: 2026-09-11
- Updated: 2026-09-11
- Status: loader, GPU retirement and module-compiled geometry capture pass independent checks; game integration incomplete
- Deployment: none
- Deprecated: no
- Scope: capture, engine geometry/MV and FG experiments through a resident host

`ExperimentAbi.h` defines a sized/versioned C boundary with the exported
`GlassExperimentQuery`. Its table creates a private context, handles versioned
borrowed events and destroys the context. No STL or allocator ownership crosses
the boundary. The draw observation payload is now defined; FG payload and GPU
capture preparation across the DLL boundary remain incomplete.

`ExperimentDrawAbi.h` and `ExperimentDrawBridge.h` pass borrowed original draw,
pipeline/root, shader descriptor, viewport/scissor and target handles to a
resident observer. Object entries and the mesh range decoder are accessed on
demand inside the callback. No vertex/bone/image buffer is copied by this bridge.
The payload does not authorize GPU recording or arbitrary retained pointers. Missing mesh
or compiled pipeline data stays explicit; descriptor handles do not establish
resource lifetime, view identity or FG correlation. The observer is invoked only
for direct indexed draws with an engine packet and tracked command recording;
it does not yet census missing packets or all rendering families.

Draw payload version 2 adds `ExperimentPipelineAbi.h` / `ExperimentPipelineService.h`.
An explicit opaque token retains the existing immutable pipeline-cache entry.
Only its host-provided release function destroys it; no shared_ptr or allocator
ownership crosses the DLL ABI. The versioned view supplies original descriptor,
owned shader/input-layout bytes through that descriptor, original/extended roots
and binding slots. One new token allocates a small CPU owner; reuse it per needed
pipeline rather than acquiring one on every draw. It makes no shader or game GPU
buffer copy. Retention can keep the original cache/COM resources alive, so modules
must bound retained pipelines and release on a worker/control thread after use.

`GeometryInstances --experiment` loads `experiment-draw.dll` through the real
runtime and production command bridge on an independent D3D12 device. It checks
32 callbacks / 48 object entries, bounds rejection, absent engine mesh metadata,
actual DLL unload, 143,360 unchanged original color pixels and the existing 3,563
geometry MV reference samples. After the first frame, the separately loaded DLL
uses its retained compiler inputs to compile and own a capture PSO. Seven later
draws run that PSO through the production insertion/restoration path; the same
color and MV reference checks pass. Compilation runs on the fixture control thread
after callbacks, not inside a draw. Its final GPU work is completed and recording
discarded before module release; a failed test retains the module conservatively.
The fixture supplies synthetic identities and a test-only PSO preparation export.
It does not invoke FG or supply the general production GPU preparation ABI. No production
startup observer registration has been added yet. Build the fixture DLL from
`ExperimentDrawFixture.cpp`, `GeometryPipeline.cpp` and `DxilVertexHistory.cpp`
beside GeometryInstances with DXC includes and d3d12/dxgi libraries; `build_geometry_shader.ps1`
includes this check.

`ExperimentRuntime.h` loads absolute paths on its control thread. Invalid ABI,
capabilities or failed preparation leaves the active module unchanged. Up to
three generations are retained. Still-loaded paths are rejected. The module
acquired for a frame remains the same across its simulated MFG phases; duplicate,
skipped or mismatched phases are rejected. This is not actual NGX correlation.

Collection unloads retired modules only on the control thread after frame leases
are released. Integration must retain those leases until **all GPU work completes
and command recordings are discarded**, not only until CPU callbacks return.
`ExperimentRecording.h` now retains module and completion-fence references for
one host-identified recording epoch. It accepts up to four submitted completion
points, requires actual discard plus all completions, and rejects wrong epochs.
Unknown submissions, overflow and device-removal values prevent retirement.
These notifications are not yet wired to the game's observer. Teardown with unresolved leases conservatively
retains at most three modules. This fallback must not be normal streaming behavior.
Module destroy must stop/join its private workers before returning. Atomic shared
pointer acquisition is not claimed lock-free; render callbacks perform no DLL load.

`ExperimentRuntime.cpp` builds actual A/B/failing/bad-ABI DLLs and verifies A→B→A,
three phases keeping A after activating B, new-frame B selection, duplicate phase
rejection, failed preparation rollback, executing CPU callback retention and actual
module disappearance after release. That CPU fixture does not test GPU completion.

`ExperimentGpu.cpp` loads two separate DLLs, each owning an actual D3D12 upload
resource. Three phase callbacks keep A while B is activated; this simulates the
MFG callback sequence and does not invoke native DLSS-G. A test-only queue gate
keeps the copies pending. Reset before completion retains A. B tests the inverse:
completion before Reset also retains the DLL. After both conditions, actual DLL
unload is observed and all 256 readback words exactly match A/B data. An unsubmitted
recording keeps its module until actual Reset. This tests module-owned GPU copies,
not MV shaders, engine inputs, automatic game observation or rendering quality.

Build `ExperimentFixture.cpp` four times with `FIXTURE_ID=1,2,99,98`, naming the
DLLs `experiment-a.dll`, `experiment-b.dll`, `experiment-fail.dll`, and
`experiment-badabi.dll`. Build `ExperimentRuntime.cpp` and pass their artifact
directory. All compile with C++20 and `/W4 /WX`. Never inject these fixtures into
the game. Build the GPU pair from `ExperimentGpuFixture.cpp` with IDs 1/2 as
`experiment-gpu-a.dll`/`experiment-gpu-b.dll`, then build `ExperimentGpu.cpp` with
`d3d12.lib`. Pending checks include capacity, failure quarantine and actual engine/FG callbacks.

Follow Microsoft's [LoadLibraryExW](https://learn.microsoft.com/en-us/windows/win32/api/libloaderapi/nf-libloaderapi-loadlibraryexw)
and [FreeLibrary](https://learn.microsoft.com/en-us/windows/win32/api/libloaderapi/nf-libloaderapi-freelibrary)
contracts. No load/unload occurs in DllMain. The existing diagnostic injector
alone does not supply this retirement mechanism.
