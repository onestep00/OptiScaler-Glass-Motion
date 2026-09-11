# Dynamically replaceable experiment modules

- Created: 2026-09-11
- Updated: 2026-09-11
- Status: loader and CPU frame-lifetime tests pass; GPU retirement and game integration incomplete
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
That GPU adapter is not implemented. Teardown with unresolved leases conservatively
retains at most three modules. This fallback must not be normal streaming behavior.
Module destroy must stop/join its private workers before returning. Atomic shared
pointer acquisition is not claimed lock-free; render callbacks perform no DLL load.

`ExperimentRuntime.cpp` builds actual A/B/failing/bad-ABI DLLs and verifies A→B→A,
three phases keeping A after activating B, new-frame B selection, duplicate phase
rejection, failed preparation rollback, executing CPU callback retention and actual
module disappearance after release. No game or GPU completion is tested yet.

Build `ExperimentFixture.cpp` four times with `FIXTURE_ID=1,2,99,98`, naming the
DLLs `experiment-a.dll`, `experiment-b.dll`, `experiment-fail.dll`, and
`experiment-badabi.dll`. Build `ExperimentRuntime.cpp` and pass their artifact
directory. All compile with C++20 and `/W4 /WX`. Never inject these fixtures into
the game. Pending checks include GPU use, unsubmitted recordings, capacity and
actual engine/FG callbacks.

Follow Microsoft's [LoadLibraryExW](https://learn.microsoft.com/en-us/windows/win32/api/libloaderapi/nf-libloaderapi-loadlibraryexw)
and [FreeLibrary](https://learn.microsoft.com/en-us/windows/win32/api/libloaderapi/nf-libloaderapi-freelibrary)
contracts. No load/unload occurs in DllMain. The existing diagnostic injector
alone does not supply this retirement mechanism.
