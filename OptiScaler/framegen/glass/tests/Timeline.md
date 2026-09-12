# Continuous draw and object metadata timeline

- Created: 2026-09-12
- Updated: 2026-09-12
- Status: independent concurrency checks and bounded moving-game capture passed; array owner bridge remains absent
- Deployment: replaceable diagnostic loaded and unloaded in PID 21760; production correction unchanged
- Deprecated: no
- Scope: CPU draw/object correspondence for observed blended pipelines; not complete GPU replay or object MV

## Recording contract

`ExperimentTimelineModule.cpp` uses the resident census ABI. It records each
selected draw, without the previous eight-sample limit per pipeline. Selection
uses actual RGB blending or alpha-to-coverage, with no material-name list.
Unrecognized pipelines and the host's missing pipeline identities are counted;
therefore a continuous sequence of frame numbers does not prove full rendering
coverage. HUD and world attribution is not established by blend state alone.

Each row copies the host frame, CPU sequence, millisecond tick, recording/list
identity, pipeline, mesh/chunk, counts and original object metadata. Object
addresses are numeric observations. They are never retained or dereferenced
after the callback. Immutable pipeline inputs are retained once per identity.
Shaders are saved once when the host drains callbacks and unloads the module.
There are no GPU commands or copies, and no MV input changes.

Default draw storage is 46,137,344 bytes, with 41,943,040 bytes of object storage.
Pipeline metadata and retained host compiler inputs are additional. Capacity
exhaustion rejects later data and is reported. Published pipelines are immutable;
draw/object ranges use atomic reservations. Only a first pipeline observation
needs a short insertion lock. A concurrent first insertion can be rejected and
counted; this is not a universal zero-loss guarantee.

`draws.bin` has 88-byte little-endian records: seven uint64 fields
`sequence,frame,tick,recording,pipeline,command,mesh`, then eight uint32 fields
`chunk,operation,indices,instances,objectFirst,objectCount,objectDeclared,flags`.
`objects.bin` has 40-byte records matching `GlassExperimentObject`. Flags are
1 for missing object callback, 2 for a failed callback, 4 for object capacity.
Unused reserved object slots stay zero and must not be interpreted as objects.
Only the prefix referenced by each row's objectCount is valid.

CPU observation order is not GPU execution order. Frame zero remains unknown.
No camera, previous vertex positions, descriptor-buffer contents or FG images
are captured. This trace cannot replay rendering by itself.

## Checks and live evidence

`TimelineModule.cpp` checks twelve consecutive fixture frames, source mutation
after capture, generation changes, unknown frames, failed object callbacks,
capacity limits and a single retained pipeline token. Four concurrent producers
record 8,000 events with no duplicate sequence, overwritten object range or
lost event after pipeline publication. This does not inject into a game.

The first five-second live version saved 72,440 draws in 119 consecutive frames.
It lost 1,722 events to a per-draw try-lock; that lock was subsequently removed.
The moving capture with the revised recorder saved 524,288 draws and 336,215
object records in frames 276590..277358, with no missing frame number or unknown
frame. Recorded timestamps span 33,313 ms. It reached its draw capacity before
the 40-second request ended; 171,136 later events hit the full guard. The final
frame can be partial. Contention was zero in this run. Missing host pipeline
identities numbered 3,730,627, so this is not complete scene coverage.

The simultaneous array wrapper trace saved 1,854 updates and resolved 7,438
source-element occurrences. Its events lack frame numbers; simultaneous
recording alone does not prove producer-to-draw temporal ownership.

The decisive missing bridge is explicit: all 40,256 array/global-range object
records contain zero proxy, mesh and generation. `GeometryDrawBatch::append`
clears identity for multi-instance spans. It prevents unsafe history admission,
but also hides parent provenance from the existing census ABI. The direct
instance producer must supply the original owner and selection in the same
render domain; array addresses cannot replace that evidence. Fifty-five proxy
addresses have multiple nonzero owner keys elsewhere in the trace. The array
source trace has 266 proxy addresses with differing source tuples. Neither
observation is permission to reuse a pointer as a persistent object ID.

Local artifacts: `work/glass-native-material-v1/timeline-1789222297206303900/`,
`work/glass-array-source-live-v1/walk-timeline-1/`. Local launch/analyze tools are
`timeline_workflow.py` and `analyze_object_timeline.py`. Raw game data is not
distributed. Actual object MV and FG correction remain incomplete.
