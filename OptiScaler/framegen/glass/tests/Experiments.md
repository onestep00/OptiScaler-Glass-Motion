# Dynamically replaceable experiment modules

- Created: 2026-09-11
- Updated: 2026-09-11
- Status: loader, CPU frame lifetime and independent D3D12 retirement checks pass; game integration incomplete
- Deployment: none
- Deprecated: no
- Scope: capture, engine geometry/MV and FG experiments through a resident host

`ExperimentAbi.h` defines a sized/versioned C boundary with the exported
`GlassExperimentQuery`. Its table creates a private context, handles versioned
borrowed events and destroys the context. No STL or allocator ownership crosses
the boundary. Actual engine/FG event payloads are not yet defined.

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
