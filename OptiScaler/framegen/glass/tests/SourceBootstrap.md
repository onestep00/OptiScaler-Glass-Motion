# Existing source-owner bootstrap

- Created: 2026-09-11
- Updated: 2026-09-11
- Status: diagnostic bootstrap and same-frame draw correspondence observed live
- Deployment: standalone diagnostics in PID 70152; production MV/FG unchanged
- Deprecated: no
- Scope: bounded candidates from previously observed node creations, not all-world enumeration

`CyberpunkSourceBootstrap.h` revalidates each recorded node/definition/shared
handle, renderer handle, typed mesh, original source range and instance count.
Two equal metadata reads and an unchanged lifecycle epoch are required. An
active creation/destruction callback rejects admission; that scope includes the
original engine callback. The cache publishes scalar metadata under its lock.
Repeated registration of the same admitted source retains its serial. This does
not prove unobserved source mutations, complete lifecycle coverage, view ordering,
or consecutive-frame GPU history. No transform content is copied or returned.

`GlassInstanceSeed` reads at most 4,096 candidates once on explicit diagnostic
request. Its binary header is magic 0x53454531 and count, followed by 80-byte
candidate records; trailing data is rejected. No file is read per frame. The
fixture covers in-flight events, an event between snapshots, changed controls,
same-source re-registration, and a null transient transform pointer.

## Live result

The first bootstrap rejected every candidate. A bounded read-only check found
2,254 matching current chains whose proxy+0x108 pointers were all zero. Requiring
that pointer to be nonzero incorrectly coupled source-owner registration to
transform availability. It is now only snapshotted, never dereferenced. This
does not authorize null/stale transform inputs for MV.

The revised DLL admitted 3,930 of 4,096 candidates without restarting PID 70152.
Save reported 189 destructor callbacks, no rejected lifecycle reads, 3,930 live
entries, ready=1 and healthy=1. The already-loaded producer was retargeted through
its `.source` sidecar while stopped. A new interval recorded 20,747 source rows;
15,312 had owner metadata, spanning 2,194 proxies/nodes and six engine frames.

A subsequent simultaneous producer/census capture joined exact frame, render
mesh and global transform slots. All instances in 2,144 anonymous draw ranges
resolved to owner/source metadata (10,982 instance occurrences). There were zero
conflicting source keys. Another 4,043 ranges had no owned overlap. This bounded
capture is not an all-transparency coverage count or a view/history admission.
No new object MV or FG input was produced.

The census-only module was unloaded; no GPU jobs were recorded or pending.
Standalone owner modules remain pinned with lifecycle forwarding active and CSV
recording stopped. Current-process chained profiles are diagnostic artifacts,
not reusable production signatures or a design for unbounded hook stacking.

Local evidence: `work/glass-source-bootstrap-live-v{1,2}/`, especially
`candidates.bin.result`, `capture/{owners,source-query}.txt`,
`producer-capture/analysis.json`, and
`work/glass-node-draw-census-v6/capture/owned-source-join.json`.
The independent `NodeLifetimes` build/test used `/O2 /W4 /WX` and passed.

## Next integration constraint

Consume this correspondence in the ordered rendering path, then verify actual
current/previous geometry and material coverage. Source-array mutation, omitted
creation routes, view/submission identity and GPU resource lifetime remain
required checks. Never substitute an older available frame for missing N-1.

## Current native-output checkpoint

A separate same-process binding capture found the native material PS hash
13eb28e98e1cd1d95bb35fe440fe59c9, matching the earlier verified shader. Its current
observation identity was 9223372036854776630, not the older process's 955.
Neither this observation identity nor its low bits selected a capture-ready
pipeline. Both diagnostic attempts captured zero jobs and were unloaded.

The binding recorder now accepts an optional mesh after the pipeline in
`prepare-vertex-v1 PID PIPELINE [MESH]`. This filters observation before reserving
rows or retaining pipelines, while the existing worker requests preparation.
The request succeeded and the recorder observed prepared identity 1375, with
139 rows, two pipelines and zero dropped rows. This is process-local diagnostic
selection; it is not a production object whitelist.

Native VS outputs 4/5 then yielded 64 captures and 9,536 valid vertex records.
Comparison requires matching proxy, mesh, registry generation, chunk, vertex
layout and viewport plus exactly consecutive frame numbers. Forty-five pairs
passed, with maximum previous-versus-prior-current error 0.000086499 pixels.
Largest captured displacement was 1.949 pixels. This is one mesh chunk; view
identity and all-transparency coverage are not established. It is independent
of the array-source bootstrap above, not evidence that those arrays supply MV.

A following native pixel capture returned five jobs but only five total pixels
(0, 1, 4, 0, 0). Thus the selected chunk is unsuitable as proof of a full object
silhouette. All jobs retired and the module unloaded. No FG input changed.
Local evidence: `work/glass-current-native-bindings-v1/`,
`work/glass-current-native-prepare-v1/capture/bindings.done`,
`work/glass-current-native-pair-v3/analysis.json` and
`work/glass-current-native-pixels-v1/analysis.json`.
