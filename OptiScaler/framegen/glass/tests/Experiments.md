# Dynamically replaceable experiment modules

- Created: 2026-09-11
- Updated: 2026-09-11
- Status: event control, raw draw census and targeted module recapture pass independent GPU checks; not deployed
- Deployment: none
- Deprecated: no
- Scope: capture, engine geometry/MV and FG experiments through a resident host

`ExperimentAbi.h` defines a sized/versioned C boundary with the exported
`GlassExperimentQuery`. Its table creates a private context, handles versioned
borrowed events and destroys the context. No STL or allocator ownership crosses
the boundary. Draw observation and capture preparation/lifecycle payloads are
defined. Game control/startup source is present; actual game validation and FG
payload/integration remain incomplete.

Latest draw payload version is 3 and capture payload version is 2. The nested
draw now supplies borrowed OM-time RTV/DSV metadata through `targetAt`; see
[GeometryTargets.md](GeometryTargets.md) for exact bytes, output and verification.
The version-2 pipeline service described below remains unchanged.

`ExperimentDrawAbi.h` and `ExperimentDrawBridge.h` pass borrowed original draw,
pipeline/root, shader descriptor, viewport/scissor and target handles to a
resident observer. Object entries and the mesh range decoder are accessed on
demand inside the callback. No vertex/bone/image buffer is copied by this bridge.
The payload does not authorize GPU recording or arbitrary retained pointers. Missing mesh
or compiled pipeline data stays explicit; descriptor handles do not establish
resource lifetime, view identity or FG correlation. The observer is invoked only
for direct indexed draws with an engine packet and tracked command recording;
the separate census path below also observes missing packets, non-indexed and indirect calls.

## Raw draw census and targeted recapture

Capability 8 / `ExperimentCensusAbi.h` version 1 observes the original indexed,
non-indexed and ExecuteIndirect call before capture insertion. An absent engine
packet or compiled capture pipeline does not exclude the call. Untracked command
recordings remain zero; an unknown engine frame remains zero. Viewport, scissor
and OM target evidence survive independently when complete raster admission fails.
The CPU sequence is not GPU execution order, and indirect arguments are not read.
`max_commands` and the optional count-buffer identity retain the original API
contract: without a count buffer that is the declared operation count; with one
it is an upper bound. Neither establishes an instance count or executed geometry.
See Microsoft's [DrawInstanced](https://learn.microsoft.com/en-us/windows/win32/api/d3d12/nf-d3d12-id3d12graphicscommandlist-drawinstanced)
and [ExecuteIndirect](https://learn.microsoft.com/en-us/windows/win32/api/d3d12/nf-d3d12-id3d12graphicscommandlist-executeindirect) contracts.

`ExperimentRuntime::observe` pins the active DLL only for this CPU callback and
does not require an invented FG frame lease. Capture/FG recording leases remain
separate. The inactive path checks an atomic capability flag before constructing
the extra metadata. The runtime test verifies replacement and disable during a
blocked census callback, with actual unload only after the callback returns.

The replaceable module allocates 32,768 fixed CPU rows once at creation. Each row
copies scalar draw metadata, nine target POD records and at most 32 original object
entries, including zero identities. No GPU buffer or shader bytes are copied by
the census. Contention/overflow and saved/original object-entry counts expose
omissions. It saves `draw-census.csv`, `.targets.bin`, `.objects.csv` and `.done`
when the retired module is destroyed. Raw COM addresses are observations only.
The target binary has nine `GlassExperimentTarget` records per CSV row. These
diagnostic buffers are not a production frame cache. Full render-pass/bundle,
unmatched shader provenance and actual game submission/frame linkage remain gaps.

An optional third `.config` line selects subsequent original draws from a census
in the same process, without rebuilding/restarting the host:

```text
select-v1 PID pipelineIdentity targetBinding targetResource mesh chunk indices instances startIndex baseVertex startInstance proxy
```

Fields are decimal; targetBinding 0..7 selects an RTV, 8 selects the DSV. All fields
match exactly, except proxy zero disables the optional object-entry membership
filter. A nonzero proxy also requires matching mesh and a valid generation. These
are diagnostic selection criteria, not proof of unique object ownership or stable
view identity. Refresh from a new census after restart/resource recreation. Invalid
syntax/PID rejects module creation, preserving the previous active module. The
selection cannot manufacture missing engine mappings or turn an unsupported PSO
into a capturable one. `selection.status` distinguishes matched/rejected prepares.
Without this line the preceding first-per-pipeline/size/instance sampler remains;
it must not be interpreted as complete object coverage.

`--controlled-recorder` saves 56 raw calls across two generations: 40 indexed
(8 without engine packets), 8 zero-vertex direct and 8 indirect fixture operations.
Their actual target IDs and original arguments remain intact. Generation two
uses an exact target/mesh/argument/proxy filter and rejects other draws while
producing the same reference capture. Original 143,360 pixels and 107,520 material
samples pass; no game objects or new MV are produced. The existing capture-module
mode also preserves 3,563 shader-MV samples. Full Release compilation passes.

The test still drains the GPU before manually forwarding submissions. Actual
queue-observer delivery and in-flight replacement through this capture owner
must pass before deployment. The earlier four-pixel game failure is unresolved.

Draw payload version 2 introduced `ExperimentPipelineAbi.h` / `ExperimentPipelineService.h`.
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
draw-observer registration has been added yet; the capture-owner startup below
does not enable this observation-only fixture. Build the fixture DLL from
`ExperimentDrawFixture.cpp`, `GeometryPipeline.cpp` and `DxilVertexHistory.cpp`
beside GeometryInstances with DXC includes and d3d12/dxgi libraries; `build_geometry_shader.ps1`
includes this check.

`ExperimentCaptureAbi.h` defines capability 4 with Prepare, Recorded and Retired
events. `ExperimentCaptureOwner.h` implements the existing resident draw-owner
seam. The module returns its PSO, history constants and owned GPU addresses; the
host preserves one original draw and restores root/PSO state. Prepare cannot issue
GPU commands. Recorded permits module-owned capture copies/transitions after that
restoration. Retired arrives on the control thread only after recording discard
and all submitted completion points. Capture callbacks are serialized; contended
prepare/collection skips admission instead of waiting on another callback.

The initial fixed pool holds 256 jobs/command trackers, eight engine-frame leases
and eight precreated queue fences. An engine frame keeps its selected module while
its jobs are pending. This is not engine-to-FG token correlation. Each observed
submission batch with jobs gets one shared completion signal. First observation
of a command installs an official destruction token; per-draw admission allocates
no GPU resources or full input copies. Unresolved submissions remain quarantined.
These diagnostic capacities/admission scans are not a production throughput claim.

`GeometryInstances --capture-module` uses this ABI/owner for all eight inserted
draws. Actual public Reset hooks discard recordings. The fixture forwards each
real submission through `NotifyGeometryCaptureSubmit` after its own GPU drain;
it does not install the live native queue observer. Seven jobs retire before the
last Reset; the final job and DLL remain retained even after runtime disable.
The final actual Reset/queue completion permits its Retired callback and unloading.
All 143,360 original pixels and 3,563 MV samples pass. Module preparation and capture
buffers in that mode still come from explicit fixture setup. Live runtime control,
in-flight replacement through this owner, resource/view census,
actual engine input and FG integration remain unverified/incomplete.

`ExperimentCoverageModule.cpp` now builds as a separate `experiment-coverage.dll`.
Its private worker compiles the original material audit PSO, initializes bounded
capture resources, and saves completed/discarded captures. No fixture supplies
its PSO or buffers. Prepare writes only its exclusively reserved upload data;
Recorded copies its own capture UAV to readback. The worker writes format 2
object bits, surviving/contributing references, draw metadata and original VS/PS.
The `.done` marker is written last. This module produces no object MV.

The DLL's same-stem `.config` contains two UTF-8 lines: the absolute DXC DLL path
and a new absolute output directory whose parent exists. It accepts Unicode paths
and never overwrites a previous session. Eight slots, 64 pipeline/size/instance
selections, 32 instances per draw and a conservative 256 MiB private-buffer budget
bound this diagnostic. Failed or timed-out private GPU initialization retains
possibly referenced resources. This is not a global budget across quarantined
module generations or a production streaming allocation strategy.

`GeometryInstances --module-recorder` uses this actual worker and capture owner,
replaces the module at frame five and preserves both output directories. The two
generations contain 107,520 original material reference samples; 143,360 original
color pixels remain exact. The second generation detects deliberately absent
object mapping. Both DLLs actually unload after completion and final Reset, joining
their workers and completing pending saves. Unicode DLL/config/output paths are
exercised. The fixture drains its GPU each frame: this does not prove replacement
while these captures are GPU-in-flight. Synthetic engine identities remain test
data; neither this result nor reference masks establish game contours or FG input.

Only module identity is recorded in DllMain. Worker creation and joining occur
outside the loader entry point, following Microsoft's
[DLL best practices](https://learn.microsoft.com/en-us/windows/win32/dlls/dynamic-link-library-best-practices).
The module checks failure/truncation of
[GetModuleFileNameW](https://learn.microsoft.com/en-us/windows/win32/api/libloaderapi/nf-libloaderapi-getmodulefilenamew)
before reading its configuration. Game registration is opt-in through the resident
control host described below.

## Resident event control

`ExperimentHost.cpp` claims capture ownership only when
`Glass/experiment-host.enable` exists. In that mode `GeometryHost` does not start
the legacy recorder. A private control thread creates `ExperimentControl` and
registers one process-resident capture owner, initially stopped. Startup errors
go to `Glass/experiment-host.error`; no second owner is installed as a fallback.
The installed MO2 DLL is unchanged and does not yet provide this control host.

The host exposes auto-reset `Local\OptiScaler.Glass.Experiment.<PID>.Request`
and `.Response` events. The diagnostic client serializes requests with `.Client`
mutex ownership. Each request file contains exactly three UTF-8 lines: a unique
ASCII request ID, `load`/`disable`/`status`, and an absolute DLL path for `load`
(an empty third line otherwise). Only a request event causes a file read.
`experiment.control.response` echoes request ID and PID with success/error,
active generation, loaded/unloaded modules, recorded/pending/retired captures,
submission-observer admission and `fg_connected=0`. A timed-out client must treat
completion as unknown and query status; it must not infer rollback from timeout.

The registered owner wakes control after a captured draw. Control checks pending
recordings at 100 ms intervals while any remain or a disabled module is draining,
then returns to an indefinite
event wait. These are CPU completion/discard checks with no file polling or GPU
work. A completed recording still requires its actual Reset/destruction before
Retired and unload. `disable` stops new admission while draining existing work;
a later successful `load` resumes it. Loader failures preserve the active module.

In the game, load is rejected until `NativeHost` has successfully installed its
process-resident queue observer. The current integration installs that observer
from the enabled native FG correction path. This dependency is explicit in status;
host-thread startup alone is not safe GPU-submission coverage. No captured job may
infer that an unobserved submission did not occur.

`GeometryInstances --controlled-recorder` runs the same two-generation GPU test
through the event/file client and independent resident control thread. It verifies
missing-observer rejection before module preparation, duplicate-path load rollback,
asynchronous retirement, worker saves, original pixels and two actual unloads.
The fixture forwards every submission manually after its GPU drain. It does not
exercise NativeHost's live queue observer or GPU-in-flight hot replacement.

Build `tests/ExperimentRequest.cpp` with C++20, `/EHsc /MD /W4 /WX` as a standalone
diagnostic client. Its interface is:

```text
ExperimentRequest.exe PID absolute-Glass-directory load absolute-module-DLL
ExperimentRequest.exe PID absolute-Glass-directory status
ExperimentRequest.exe PID absolute-Glass-directory disable
```

The coverage module requires its two-line same-stem configuration described above.
Requests can load a different module build in the same game process; they do not
replace resident observation hooks or add missing startup resource metadata.
Follow Microsoft's [event creation](https://learn.microsoft.com/en-us/windows/win32/api/synchapi/nf-synchapi-createeventw)
and [multiple-object wait](https://learn.microsoft.com/en-us/windows/win32/api/synchapi/nf-synchapi-waitformultipleobjects)
contracts. Event handles remain alive throughout the resident wait.

## Loader and recording lifetime

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
The capture owner now accepts the resident submission/Reset seam; its game startup
registration is not deployed. Teardown with unresolved leases conservatively
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
