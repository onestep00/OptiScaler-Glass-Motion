# Compatibility and runtime evidence

- Created: 2026-09-11
- Updated: 2026-09-11
- Status: MRT, indirect binding, relocation and status-display checks pass; fresh-process validation pending
- Deployment: Release package prepared for MO2; fresh-process application of this revision remains unverified
- Deprecated: no
- Scope: engine discovery, public D3D12 observation, bounded metadata and OptiScaler health reporting

## Compatibility

Public graphics observation obtains methods from the actual D3D12 device/interfaces and metadata from successful creation calls. It does not search NVIDIA driver binaries or pin the driver version. Unknown implementations, shader formats and signatures still require rejection; this is not a promise about every driver or wrapper.

`CyberpunkLayout` discovers nine engine functions using the PE exception-function table. Normalized profiles retain instructions, object-field offsets, internal branches and function lengths. Only audited relative call/RIP address operands may change. Referenced sections, shared registry/frame globals and append/upload/backend call relationships must agree. Duplicate candidates reject.

The adapter no longer requires the inspected executable's timestamp, image size or preferred RVAs. Function/global relocation with unchanged instruction and object layout is supported. Register allocation, instruction/field changes, unknown modifications to those functions and unsupported geometry families can still reject. No second official release has been executed to establish a release support range.

The surface-depth path separately finds two unique instruction windows. It normalizes relative calls but preserves ordered-stack and resource-contract validation. Geometry observation no longer depends on this separate depth-pass recognition. Failed engine discovery does not disable public D3D12 metadata observers or modify MFG unlock behavior. Discovery is performed once during initialization, never per draw. Local diagnostic probes intentionally pin their exact PID/binary; they are not the production discovery mechanism.

## Failures found in the running 32054cc build

Engine packets and public draws were observed, but pipeline/root matches stopped at 266. New object capture and FG replacement remained zero. A CPU trace in the filled-glass/railing scene found 12 audited transparent shader pairs on actual three-slot PSOs. The one-target prefilter rejected them before compilation. Cached one-slot variants did not prove coverage of the active PSOs.

The source now admits one-to-eight original targets, preserving all descriptor slots, auxiliary outputs and dual-source blending. Acceptance does not execute the modified PSO.

The same draws had invalid root histories. A bounded probe counted 2,700 invalidations; all 2,048 retained records were ExecuteIndirect. The old observer invalidated all graphics roots after every indirect command. The new public CreateCommandSignature observer learns which constants/views are reset to zero/null and preserves unaffected roots. Compute resets remain separate. Unobserved/unsupported signatures reject replay.

The signature cache holds at most 256 immutable entries, 32 lookup probes and 64 root-reset records per entry. Retained COM references prevent address reuse. Creation alone parses/locks; execution reads no GPU argument buffer and performs no driver query, allocation, compilation or wait for classification. This does not supply the missing production IA/render-target and submission ownership.

## Status display

The glass section shows `Object MV -> FG: NOT applied` until object capture and recent FG replacement evidence exist. Existing correction status explicitly identifies static-surface correction. Installed hooks and observed calls have separate labels. Rows show engine layout, creation/object/draw hooks, shader results, object/pipeline/root joins, indirect signatures, capture/replacement counts, frame and report age. Reasons distinguish unavailable components, missing joins and stalled evidence. Menus/paused scenes do not automatically prove hook failure.

UI sampling is limited to once per second, independently of FG success and log availability. It reads atomic counters and attempts a nonblocking numeric cache read. Contention skips the cache update. No compiler-error string allocation, driver query, binary scan, GPU work or wait is added. Existing throttled logging includes `GEOMETRY_HEALTH`. These diagnostic snapshots are not GPU ownership proof.

The production same-draw capture owner and new FG substitution remain unimplemented; their counters stay zero. Working hooks and compiled pipelines do not resolve background attachment. GPU history/allocation/retirement, per-object contours, exact FG-frame mapping and procedural/particle coverage remain required.

## Verification

- MRT and dual-source GPU fixtures each preserve 143,360 original/auxiliary target pixels, 3,563 MV samples and 24 public draw/root joins. Maximum reference MV error is 0.001687952 pixels in this synthetic test.
- The public indirect GPU fixture verifies untouched constants, partial zero reset, compute isolation, unknown-signature rejection and Reset recovery through 12 original VS stream-output values.
- The compatibility fixture reads the user-owned executable into non-executable CPU memory. Moving nine functions, two globals and two surface patterns, with changed timestamp/image size, preserves discovery. Field corruption, incorrect calls/registry aliases, reversed stack and duplicate functions reject. No game code is executed or process attached.
- Object/draw callback tests retain original results, generations, copied layouts, batching and rejection behavior.
- Headless settings tests distinguish hooks/capture from FG application, reject stale evidence, retain monotonic counters and throttle sampling. INI preservation and actual ImGui vertex generation pass. This is not installed-game visual verification.
- The complete Release x64 solution builds. No live quality or performance bound follows from these results.

`tests/build_geometry_shader.ps1` includes indirect/MRT checks; `tests/run-tests.ps1` includes settings. Build `tests/GeometryCompatibility.cpp` with the x64 C++20 compiler and supply an explicit user-owned Cyberpunk2077.exe path. Keep binaries and raw game data outside source control.

## Primary references

- [Indirect command semantics](https://learn.microsoft.com/en-us/windows/win32/direct3d12/indirect-drawing) and [signature creation](https://learn.microsoft.com/en-us/windows/win32/api/d3d12/nf-d3d12-id3d12device-createcommandsignature)
- [PSO target layout](https://learn.microsoft.com/en-us/windows/win32/api/d3d12/ns-d3d12-d3d12_graphics_pipeline_state_desc) and [dual-source blending](https://learn.microsoft.com/en-us/windows/win32/direct3d11/d3d10-graphics-programming-guide-output-merger-stage)
- [PE format](https://learn.microsoft.com/en-us/windows/win32/debug/pe-format) and [x64 function tables](https://learn.microsoft.com/en-us/cpp/build/exception-handling-x64?view=msvc-170)
