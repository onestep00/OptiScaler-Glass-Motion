# Actual vertex-output history and material motion

- Created: 2026-09-11
- Updated: 2026-09-12
- Status: live vertex snapshots recorded; continuous object history, jitter/view admission and FG integration incomplete
- Deployment: replaceable vertex diagnostic ran in PID 56340 and unloaded; installed correction unchanged
- Deprecated: no
- Scope: instrumenting supported DXIL VS/PS 6.0 shaders without replacing their geometry or material math

## Implemented source

`VertexInputPair` records two explicitly selected uint32 input components in
the existing 32-byte vertex record at offsets 24/28. It rejects absent IDs,
unsupported packing/types, invalid components and conflicting diagnostic
payloads. Original output calculations and the pixel shader stay unchanged.
The standalone unused-Z/W test preserves six exact uint words, current position,
frame/generation and guard records; native-pair/depth and the 32-frame history
regressions also pass. This adds two input reads and widens the existing tag
store, without additional history allocation or a second draw.

`GLASS_CAPTURE_INPUT_WORDS` builds the replaceable diagnostic. After the usual
compiler/output/selection lines, config requires
`input-words-v1 PID PIPELINE INPUT_ID FIRST_COMPONENT SECOND_COMPONENT`.
Its format-4 sidecar labels the input ID/components and byte offset. Numeric
selection is an audited experiment filter, not production material detection.
The compiler tool exposes `rewrite-input[-mapped]` with `ID,FIRST,SECOND`.
Compile `InputWordsVertex.hlsl` to `input-words-vs.dxil`, then run
`NativePairGpu FIXTURES DXCOMPILER --input-words` for the independent GPU check.

Live PID 72636 generations 9 and 10 each saved 64 captures / 6,272 vertices on
the observed pipeline 853, without restarting. Input ID 9 is the original
`INSTANCE_SKINNING_DATA` uint4; the original shader reads only X/Y. The first run
read Z/W: Z was nonzero and W was one. The second read X/Z: they differed in every
captured vertex. Three exact consecutive pairs, matched by recorded proxy/mesh/
slot/generation/chunk/layout/viewport, had current Z equal to preceding X for
all 155 vertices each. This supports a previous bone-offset candidate already
supplied to this transparent draw. It does not prove bone-buffer contents,
previous deformation, camera/world transforms, general route coverage or MV.
Next validation must evaluate that previous state and compare its positions
against original N-1 outputs; buffer address equality alone is insufficient.
Evidence: local `work/glass-input-words-v1/capture-analysis.json` and
`capture-xz-analysis.json`. Both generations retired and unloaded; totals are
384 recorded/retired, zero pending/loaded modules, `fg_connected=0`.

The replaceable-module bridge now optionally forwards pre-submit observations
for each captured recording, in actual command-list order. The separate
`GlassExperimentSubmission` capability preserves the existing capture ABI.
Submission payloads include the captured job/recording, queue identity, list
position/count and a monotonic submission number. They are callback-scoped scalar
observations; modules must not issue commands, wait or modify uploads in this
callback. This is not admission to reuse history buffers across frames or queues.
Modules requiring this event are rejected on a host without the capability.
An atomic zero-job check bypasses the new lock/scan when no opted-in jobs exist.
The existing bounded diagnostic job array is reused; no GPU resource or command
is added by observation. Status responses expose `capture_before_submit` and
`capture_before_submit_rejected` independently of FG substitution.

The actual public D3D12 observer delivered eight module callbacks in the
independent `--capture-module` test. Job/epoch/queue/order checks passed; all
143,360 original pixels, 3,563 motion samples and 896 overlapping samples still
passed, and the DLL unloaded after GPU completion and recording discard.
An unsupported host rejected the new module before activation. The older
coverage capability also passed its two-generation capture/unload test without
opting into submission events. Evidence is local `work/glass-submit-bridge-v1/`.
The new bridge is not deployed; live history ownership and dense MV/FG remain
incomplete. Earlier sections below retain the history of missing host seams.
Release x64 compilation/linking also passed; the scoped local host artifact has
SHA-256 `062ae30aeecec772b2cd42621337e1b0ec0d220ea4dfbd8dc25b856d155d9030`.
Existing XeSS/linker warnings and package missing-file/path messages remain;
this is not complete package verification. The running PID 72636 still reports
256 captured/retired jobs, zero pending/loaded modules and `fg_connected=0`.
It has not loaded this new host.

The history shader now requires a nonzero expected previous frame equal to the
current frame minus one, as well as matching stored frame and generation tags.
Previously an explicitly requested N-2 tag could pass. The independent GPU test
now retains frame 4, skips frame 5, and requests frame 4 at frame 6. It failed
with the preceding shader and passes with this guard. Current capture remains
available on missing history. This adds integer checks, no allocation, copy,
CPU wait or queue synchronization.

The five-sample regression preserves 122,880 original color pixels and all 90
current vertices; 36 valid previous vertices remain exact and 6,017 motion
samples pass (maximum error 0.001586 pixels). The independently rebuilt
`--capture-command` instance test also passes: 143,360 original pixels, 3,563
motion samples and 896 overlap samples. Local evidence is in
`work/glass-history-adjacency-v1/`. This change is source-tested, not deployed;
continuous game GPU history, view/topology admission and FG remain incomplete.

Native pixel capture now ran through replaceable generations 5--7 in PID 68908.
The first recording and several later mesh/chunk recordings contained only zeros;
those are failed coverage samples, not boundary images. Generation 7's first
sample at engine frame 26160 contains 95 depth-tested native PS invocations and
exactly 95 stamped MV records, with zero object-status flags. It covers render
coordinates x=1139..1184, y=123..147 at 2560x1440. Median motion is 2.3198 render
pixels, maximum 2.5283. An optional diagnostic atomic counter at reserved byte 16
distinguishes PS invocation from later object/MV rejection. It is off by default
and requires reserving the first 32-byte record. Later samples can still be empty;
this does not establish the cause of the earlier all-zero frames.

Local artifacts are `work/glass-native-pixels-live-v3/analysis.json`,
`native-mv-full.png`, `native-coverage-full.png`, and `native-mv-screen.png`.
The visualization was inspected: it shows only two small disconnected fragments,
not a complete cup/railing silhouette. Its 65 boundary pixels come from the actual
raster mask's four-neighbor boundary, including occlusion cuts. It is not image
segmentation, but neither does it establish the full object's intrinsic outline.
There is no same-frame scene-color capture identifying the visible object.
Do not present this as complete object coverage or a new FG input.

`GLASS_CAPTURE_NATIVE_PIXELS` uses one-instance diagnostic draws, full viewport
32-byte pixel records, and a 512 MiB accounted buffer budget. At 2560x1440 this
allows one in-flight allocation group, reused after retirement. At most eight
distinct observed mesh/chunk combinations are requested, with exact matching
when prepared. This is diagnostic sampling, not a production material whitelist
or memory strategy. All recorded jobs completed; generation 7 unloaded with
75 cumulative recorded/retired jobs and zero pending. The resident host and
existing FG correction remain unchanged during these replaceable experiments.

The independent native test now also compares original and captured D32
depth-writing draws with a foreground depth occluder and per-instance mapping.
All 256 color/depth samples match exactly, 12 occluded samples are excluded,
and 20 visible pixels match both the capture and invocation count. The mapped
fixture intentionally has zero vertex-history capacity, as in this live native
path. The existing five-frame geometry regression passed with 122,880 unchanged
color samples and 6,017 motion samples. Other depth/stencil modes are not covered
by this particular comparison.

Native pixel-capture source now accepts explicitly identified, perspective-linear
current/previous clip inputs. It computes normalized motion in the original PS
and retains original color exports. The native mode requires disabled blending/
logic operations and no alpha-to-coverage; it rejects original discard, depth
exports and resource-write side effects. It does not infer material opacity: the
native G-buffer diagnostic records zero transmission as a coverage marker only.
Ordinary transparent-material capture remains on its existing admission path.

NativePairGpu now exercises the native pixel writer without previous-frame buffer
history. Its 32 raster samples agree with independent double-precision barycentric
correspondence within 1.005e-7 normalized units, with varying current/previous W
and separate raster jitter. Its subsequent depth comparison and bounded live
pixel capture are documented above. General object coverage and FG integration
remain incomplete.

Live native-pair checkpoint: host SHA-256
`31b145045ee78ed31a041b25066bf1737d615100e46eabd8fd4c6b10722c1853`
was deployed through MO2 and loaded in PID 68908 together with the existing MFG
ASI. The bindings recorder observed the original native VS `7193f0d2...`; worker
preparation was accepted and later census exposed its prepared identity 955.
The replaceable pair recorder then completed 64 captures / 9,536 valid vertices
over engine frames 8242--8321, with one unchanged recorded proxy/mesh/slot/
generation/chunk tuple. All full current/previous clips were finite with positive
W. Forty-seven consecutive capture pairs compared each native previous NDC with
the preceding captured native current NDC; maximum displacement disagreement was
0.0001528 render pixels. This supports N-1 correspondence for this observed draw,
not all transparent objects or FG-frame equivalence.

Evidence is local `work/glass-native-pair-live-v1/analysis.json` and `capture/`.
Its 149 vertices occupy a small moving screen region. No triangle topology,
material boundary raster or new FG input was captured by this recorder. It is
not the requested dense boundary MV image. All 64 jobs retired and generation 4
unloaded with zero pending. MO2's `overwrite/bin/x64/Glass` request file shadows
the physical request file in this run; both request and response use that actual
redirected directory, avoiding a needless game restart.

`VertexClipPair` optionally captures two explicitly audited native VS float4
outputs, preserving their full clip W. Output IDs come from the selected original
shader, not a material or driver whitelist. Unpacked float4 signatures and unique
scalar stores are required; absent, duplicate or incompatible selections reject
rewriting. This does not identify the semantic meaning of either output.

The diagnostic uses 64-byte records: original SV_Position at 0, frame/generation
at 16/20, native current clip at 32 and native previous clip at 48. Both previous
and current buffers use that stride. It is mutually exclusive with CB-word
capture. It adds two raw float4 stores and no matrix/skinning recomputation or
perspective divide. Default history remains 32 bytes. Full previous clip W is
necessary for subsequent perspective-correct rasterization; vertex NDC XY alone
is not a sufficient substitute. No boundary raster is implemented by recording.

`GLASS_CAPTURE_NATIVE_PAIR` builds the replaceable recorder with this layout.
After compiler/output/selection config lines it requires
`clip-pair-v1 CURRENT_PID PREPARED_PIPELINE_ID 4 5`. It rejects other pipelines
before any capture allocation. The final numbers are audited output IDs. The example
is specific to the two locally inspected native MeshStatic VS variants, not a
generic detector. The `.draw` file declares format 3 and both selected IDs/offsets.
Allocation accounting, history capacity, retirement and saved byte lengths use
64-byte records; the existing 256 MiB diagnostic budget is unchanged. Original
PS/depth/color state remains intact. It produces no dense MV or FG substitution.

The actual recorder DLL compiled with /W4 /WX. Both recorded native VS variants
assembled and passed DXIL validation with the selected pair; absent output 99
was rejected. `NativePairGpu.cpp` independently executes the production compiler
on `NativePairVertex.hlsl` / `NativePairPixel.hlsl` and verifies all original,
current and previous clip values, including distinct raster jitter and varying W,
tags, and untouched guard records. It passed with `NATIVE_PAIR_GPU_OK`. The
existing five-frame geometry GPU regression also passed unchanged (122,880 color
samples, 6,017 motion samples). These are independent tests, not game execution.
The resident game host still lacks the new native preparation ABI, so this DLL
has not been loaded into it. Actual native previous-input validity, dense object
boundary capture, and game/FG linkage remain incomplete.
The complete Release x64 solution subsequently compiled and linked with exit 0.
Existing XeSS/linker warnings and post-build missing-path messages remain; only
the identified DLL and required sidecars are candidates for scoped deployment.

Compile the two NativePair HLSL fixtures with the existing GeometryShaderTool
`compile` mode as `native-pair-vs.dxil` (vs_6_0) and `native-pair-ps.dxil` (ps_6_0).
Build NativePairGpu.cpp with GeometryPipeline.cpp and DxilVertexHistory.cpp using
the same includes/libraries as GeometryShaderGpu. Run with the fixture directory
and absolute dxcompiler.dll path. The fixture does not attach to a game.

`GeometryCompiler::createVertexCapture` now permits depth-writing and arbitrary
original blend states because it retains the original PS/state and replaces the
draw once. Material/coverage rewriting still rejects writable depth and unsupported
blends. This does not permit duplicating a native draw over its own depth writes.
PSO and separately bound command state remain distinct under Microsoft's
[pipeline-state contract](https://learn.microsoft.com/en-us/windows/win32/direct3d12/managing-graphics-pipeline-state-in-direct3d-12).

The independent five-frame GPU fixture renders original and captured variants
from separately cleared depth/color, with depth writes enabled, LESS comparison,
blending disabled and original material discard. All 122,880 color pixels and
122,880 depth samples match bit-for-bit; each frame checks nonempty, partial depth
coverage. Existing actual-position/history and material-MV checks also pass.
The compiler and fixture built with /W4 /WX. This proves supported native-state
preservation, not a Cyberpunk previous-transform capture or FG substitution.
The creation cache still auto-admits only the previous material subset; explicit
worker preparation and host admission for original-only observations remain next.
Both recorded MeshStatic native velocity VS variants also assembled and passed
DXIL validation with per-instance vertex recording and the explicit b1 row-51
word capture. This offline result is recorded locally in
`outputs/glass-native-velocity-route-audit/native-vertex-capture-audit.json`.
It does not verify their game root contents or execute those recorded shaders.
The full Release x64 DLL also compiled and linked with exit code 0 after the
depth-writing extension. Existing compiler/linker warnings and post-build missing
file/path messages remain; complete packaging and game deployment are unverified.

`ExtractNativeMotionTarget` can retain an explicitly identified native float4
SV_Target as target 0 while removing other color exports. It preserves original
inputs, arithmetic, branching and discard, and rejects non-color outputs and
pixel write side effects. It does not discover motion semantics or supply engine
previous transforms. The compiler tool's `native-motion` mode takes the known
target index as its final argument and assembles/validates the result.

Recorded MeshStatic velocity PS variants for `glass_deferred` and
`glass_cracked_edge` passed target-3 extraction and DXIL validation. Their four
retained export values match the originals exactly; all non-export instructions
also match after resolving renumbered/self-referential control-flow metadata.
Requesting absent target 7 was rejected. Local audit is
`outputs/glass-native-velocity-route-audit/motion-extraction-audit.json`.
This has not been GPU-rendered or integrated into a live engine velocity draw.
Unused material calculations are still present in the intermediate shader; no
reduced execution-cost claim follows from removing exports. Native input binding,
MV-only pipeline state and inward-boundary selection remain required.

Source now has optional `D3D12Callbacks::beforeSubmit`, called under the existing
enter/leave submission lock before the real ExecuteCommandLists. NativeHost
forwards it to `GeometryDrawCaptureOwner::beforeSubmit`; existing owners default
to no operation. It neither skips game commands nor inserts waits or barriers.
The independent observed-session GPU test passed 13 paired pre/post callbacks,
checking queue/count/list identity with a sticky ordering failure flag. Existing
lifetime, state and timing checks also passed. This is the host seam only: no
GPU-history owner, pre-submit experiment ABI or production MV is implemented by
this change, and the running game retains its previous resident host.
Release x64 solution build also passed after limiting compilation to one process.
The first parallel attempt failed with Windows commit-limit/PCH allocation errors;
no page-file or game settings were changed. Existing XeSS inheritance and native
alignment warnings remain. The newly built host has not been deployed.

Generation 9 measured when the diagnostic Retired callback arrives, using the
latest observed engine draw frame (not a wall-clock FPS estimate). Of 64 jobs,
53 arrived two frames later, 3 three frames later, 7 four frames later and 1 five
frames later. All 64 retired and the module unloaded; cumulative host counters
were 376/376 with zero pending. Evidence: local `outputs/glass-retirement-live-v9`.
Therefore CPU readback/Retired handoff cannot supply consecutive-frame history
in this observed run. Do not feed N-2 or older positions as N-1 motion.

The real-time path must retain GPU history and establish N-1 write -> N read
ordering without awaiting CPU retirement. Retired remains a resource-release
condition only. Current `D3D12Observer::submit` calls the geometry callback AFTER
the real ExecuteCommandLists; the experiment ABI exposes no pre-submit queue
admission. A future live history owner needs an explicit pre-submit admission
path or independently proven queue provenance before recording, barriers for
history access, and retained allocations through all recorded/in-flight users.
The current diagnostic does not establish that production contract. No full
screen object MV or FG replacement has been produced by this measurement.

The `GLASS_CAPTURE_VERTEX_COVERAGE` diagnostic records original VS positions and
original-material coverage in the same inserted draw. It uses the existing VS
history and audited coverage PS, with no second material draw. Vertex records and
coverage bits occupy disjoint regions of one bounded owned UAV allocation. A
single retired readback is split into `.vertices.bin` and `.coverage.bin` on the
worker. The common 256 MiB allocation budget remains; this is diagnostic memory,
not the intended production allocation strategy. Both shader stages receive the
same frame-local instance map. No temporal identity is inferred from that map.

Generation 8 ran in the existing PID 56340 and captured 64 same-draw pairs. All
128 vertices per snapshot had the expected frame/write tags. In every snapshot,
the per-instance coverage equaled the independent same-draw contributing
reference, and status was zero. The inspected frame 95944 contains 90 material
pixels; projected vertices align with the material region, including vertices
outside its surviving/occlusion-clipped pixels. Its arrows use the actual frame
95943/95944 vertex displacement, with jitter still included. This is not a dense
boundary MV and does not identify intrinsic silhouettes separately from material
discard and opaque occlusion. No image segmentation was used. The shader compiler
and diagnostic DLL built with `/W4 /WX`.

Local evidence: `outputs/glass-vertex-coverage-live-v8/combined-analysis.json` and
`same-draw-coverage-vertices.png`. Host totals reached 312 recorded/retired jobs,
zero pending, then generation 8 unloaded. No FG substitution occurred. The live
v8 `.done` files retain the old `vertex_only=1` label; their `.draw` files correctly
declare `coverage_same_draw=1` and exact byte ranges. Source fixes that label for
subsequent builds. These observations do not validate every transparent route.

The optional `VertexConstantPair` records two raw words from an existing draw CB
at bytes 24/28 of each 32-byte vertex record. Resource metadata resolves the CB
ID from space/register; exact size, singleton binding and row bounds are checked.
Absent/incompatible bindings reject compilation. Default history is unchanged.
The generic rewriter does not identify these words as camera data. The separate
Cyberpunk vertex diagnostic explicitly requests b1/space0, 848 bytes, row 51 XY.
It adds no buffer allocation, CPU CB copy or new root binding. It does add a CB
load and expands the existing tag store; no zero-cost claim is made.

Three recorded game VS variants (pipelines 859, 854, 931) assembled and passed
DXIL validation with this capture; a fixture without the binding was rejected.
The existing independent GPU history/material regression passed. The compiler
bridge exposes `rewrite-camera-mapped` for this explicit local diagnostic layout.

Live generation 7 then captured 64 snapshots of 128 vertices in PID 56340.
Every record had valid frame/write tags, finite clip coordinates and the same
finite CB pair across its draw. There were 38 consecutive-frame pairs with one
recorded owner/mesh/slot/generation/chunk/pipeline tuple. Raw CB words were acquired
on the GPU from the original draw, not inferred from motion. Subtracting their
candidate NDC jitter delta changed the first pair's median displacement from
0.926960 to 0.102383 render pixels. This is a candidate calculation, not proof of
the live jitter sign, view identity, temporal topology or final surface MV.
The format-2 sidecar records the exact CB source and byte offset. Evidence is in
local `outputs/glass-vertex-camera-live-v7/camera-analysis.json`.
The generation completed 64 jobs and unloaded: host totals 248 recorded/retired,
zero pending and zero loaded experiment modules. No FG inputs were replaced.

`GeometryCompiler::createVertexCapture` now supports diagnostic actual VS-output
recording with the original PS bytecode retained unchanged. It reuses the existing
history instrumentation and extended root; the caller must still supply bounded
history resources, immutable constants and verified instance mapping. This mode
does not capture material coverage, infer topology, or produce an FG input.
It does not require pixel ROV support. Its later depth-writing extension is
documented above; automatic material-cache admission remains restricted.

The independent GPU fixture additionally renders this pipeline on each of five
frames, preserving all 122,880 original material pixels. Its history writes are
included in the existing exact vertex-output comparisons; all 90 current outputs
and 36 accepted previous outputs still match. A UAV barrier orders the added
diagnostic draw after earlier writes. This test passed after rebuilding the
compiler and fixture with MSVC `/W4 /WX`.

The replaceable `ExperimentCoverageModule` now has a separate
`GLASS_CAPTURE_VERTEX_OUTPUTS` build. It saves frame-local raw clip positions and
tags as 32-byte records, preserves original PS bytes, and uses separate owned
buffers for each pending recording. The 256 MiB diagnostic budget, eight pending
slots and 64-capture ceiling remain. No cross-frame GPU dependency or temporal
identity is invented: previous input is zeroed, generation 1 is only a local
write-enable tag, and actual engine identity is stored separately in CSV. It
records at most one selected draw per engine frame. Original coverage mode also
still compiles with `/W4 /WX`.

In the same running game, generation 6 captured a selected skinned draw 64 times
over engine frames 79426--79518. All 128 vertices in every snapshot had the actual
frame tag, local write tag and finite clip coordinates. One owner/mesh/slot/
generation/chunk tuple persisted in these records; 35 pairs had consecutive frame
numbers. Position differences were calculated directly from the recorded VS
outputs, without image matching. They still include projection jitter and do not
prove complete temporal topology/view identity or a rasterized surface MV.
Each snapshot is 4,096 bytes; this is diagnostic readback, not the production path.
The host reported 64 new recorded/retired jobs, then generation 6 unloaded with
zero pending captures. The older pinned CPU observers are separate and remain
resident with recording stopped. No new motion was substituted into FG.

The diagnostic selector can use UINT32_MAX for startInstance to permit changing
upload offsets while still checking live object/mesh/pipeline/target criteria.
That wildcard never supplies missing identity. The selected numeric identifiers
are a local experiment filter, not a production object whitelist.

The diagnostic now uses `OriginalColorAndCoverageAudit`: the same PS records
two reference bit regions before any object-map rejection. One records surviving
PS pixels after original discard and early depth, the other records material
color/transmission contribution. Existing per-instance bits follow afterwards.
This isolates absent material contribution from object-map/storage losses without
segmenting an image or replaying the material draw. It does not verify the view
or establish complete object contours; both references still share the original
material computation in the instrumented draw. Ordinary production/history and
coverage targets do not include these diagnostic writes.

Format 2 appends the two reference regions after the per-instance regions. The
`.draw` sidecar supplies their exact first bits, stride, pixel count and the
recording epoch/pipeline identity. Original VS/PS bytes are saved by the worker
as `.vs.dxil`/`.ps.dxil`, for local inspection only. The same fixed diagnostic
budget charges the added buffers; no new readback or shader file write occurs
on the render thread. These references are not additional per-object full-size
textures in the proposed continuous correction.

The independent recorder fixture deliberately removes one instance identity in
the second session. Original color stays exact and both material references
still match separate original draws. The omitted object's mapped bits remain
zero. A decoded sample contains 459 contributing pixels and 364 mapped pixels:
95 missing pixels are exposed, with no excess mapped pixels. Its comparison
image was inspected. This is owned synthetic geometry, not a recovered game
silhouette. Original shader artifacts are byte-identical to the fixture inputs.
The live four-pixel failure below has not yet been recaptured with this audit.

Fresh-process observation of installed DLL SHA-256
`f025dfbd3512195420504ce8119d66766cc51a96b021a68d00aad02c2cf7e680`
confirmed concurrent loading of the existing MFG ASI. Seven completed per-object
captures at 2560x1440 were saved from engine frames 3094 through 3114. Five have
zero covered pixels; the other two have one and three pixels. The contact sheet
was inspected and contains no usable cup/railing silhouette. Submission/readback
works, but this does not verify target selection or complete material coverage.
The recorder currently consumes each slot on the first matching draw; a tiny,
occluded or wrong-pass sample is not retried. That sampling limitation and the
unrecorded target provenance must be resolved before judging the shader path.
No object MV was produced or substituted into FG. The native substitution counter
still describes the preceding static-world correction.

The recorder also saves a `.draw` sidecar with original indexed arguments,
viewport/scissor and observed RTV/DSV descriptor handles. When the actual engine
chunk decoder succeeds, it includes vertex/index counts, stream offsets and
buffer IDs. Missing engine metadata is explicit. Descriptor handles are binding
observations, not retained resource or camera/view identity. The sidecar therefore
marks view identity and topology history unproven; it must not authorize mask
merging or motion history on its own. These scalar copies occur only on selected
one-shot captures and file writes stay on the worker.

`GeometryCoverageRecorder` is a request-driven game diagnostic owner, enabled only by
`Glass/capture-objects.request` at startup. The file contains an absolute output
directory. It requests supported pipelines from actual engine-mapped draws, builds
coverage variants and allocates buffers on a worker, then records each admitted
draw once with its original material and a separate object bit allocation.
Eight concurrent slots, at most 32 instances per draw and a conservative 256 MiB buffer
reservation budget bound this diagnostic. The resampling revision retires completed,
discarded captures outside the owner lock and admits up to 64 distinct pipeline,
viewport-size and instance-count combinations. Output numbers remain unique as
slots are reused. The fixed 3840x2160 limit is removed; integer dimensions up to
the mapping's 32768 coordinate limit are checked using 64-bit allocation arithmetic
and the same memory budget. This is broader diagnostic sampling, not exhaustive
object coverage or a continuous production allocation strategy. Each session stops
accepting after 30 seconds or a Stop request. Missing completion/discard ends the
drain after two minutes without authorizing readback or releasing possibly referenced resources.
PSO/driver allocations are additional; this is not the continuous production cost.

The worker then waits on `Local\OptiScaler.Glass.Capture.<PID>.Start`; the matching
`.Stop` event ends admission. Idle waits perform no polling, GPU work or file
reads. Each accepted later request uses a new `request-N` subdirectory, preserving
earlier results. Unresolved recordings reject restart. Requests during an active
session report busy. This permits new captures with the same compiled code; it
does not replace shaders or the diagnostic DLL. General DLL replacement remains
the separate [experiment-host](Experiments.md) integration task.

The native submission observer forwards actual submissions; successful command
Reset marks old recordings discarded. Readback and CSV identity metadata are
written only after both events and the actual GPU fence. Copy calls run without
the recorder mutex, avoiding inversion with the native host's submission lock.
The independent `--recorder` fixture stops and restarts capture within one
process, compares 107,520 individual material samples across both requests with
original draws (including an intentionally inactive mapping), and preserves
143,360 original color pixels. Both same-draw reference masks match the original
material union independently of that omitted mapping.
The fixture supplies test identities, not Cyberpunk objects. This recorder emits
no MV and makes no FG substitution. The fresh-game limitation is recorded above.

Latest source checkpoint (2026-09-11): `OriginalColorAndCoverage` records separate
object material coverage as bits, independent of vertex-history availability.
It preserves original color exports, material discard and read-only depth tests.
It produces no MV and requires a per-instance identity map with zero history
range. An independent eight-frame GPU test preserves 143,360 original pixels
and matches 5,296 covered samples against separate original object draws.
This diagnostic is not live game capture or a complete silhouette producer.

`GeometryDrawCapture.h` now supplies the actual indexed-hook insertion seam.
The registered owner must reserve immutable mappings and retain all resources;
the hook binds the prepared pipeline, forwards the draw once, then restores the
original root values and PSO. It does not infer identities or own retirement.
`GeometryInstances --capture-command` exercises this production hook on an
independent device: eight inserted draws, 143,360 unchanged original pixels,
3,563 geometry MV samples with maximum error 0.001688 pixels, and 896 overlapping
object samples. The fixture supplies identities and synchronized resources;
the live owner, queue/frame linkage and FG replacement remain unimplemented.

The draw observer also records actual viewport, scissor, RTV/DSV bindings,
predication and render-pass state. Capture insertion requires a known single
viewport/scissor outside predication/render passes; bundle execution invalidates
the raster snapshot until reset. These are API binding observations, not retained
resource identities or FG-color provenance. The capture-command fixture checks
the real 160x112 viewport, scissor and target handles at all eight insertions.

The latest [compatibility checkpoint](../Compatibility.md) adds preservation of actual MRT/dual-source layouts and precise indirect-command root resets. Both MRT GPU fixtures and the public indirect fixture pass. These source fixes do not create a production capture caller or new FG replacement.

`DxilVertexHistory.cpp` rewrites DXC disassembly, which the caller must assemble and validate before creating a pipeline. It preserves the original vertex outputs and records the actual computed clip position in a caller-owned history buffer. A generation and expected-frame tag reject stale or unrelated entries. New varyings expose the previous position and a missing-history flag. This includes transformations and deformations already computed by the original vertex shader; it does not infer motion from scene pixels or duplicate a skinning algorithm.

`RewriteMaterialMotion` preserves the original material computation/discard and emits normalized previous-minus-current motion, mean RGB attenuation, and current device depth into a separate target. Material opacity is derived from the verified destination blend factor; colored transmission becomes `1 - mean(saturate(T.rgb))`. A scalar coefficient cannot fully represent colored/refraction layers. Pixels contributing neither source color nor attenuation are excluded. Invalid history and nonfinite motion are rejected. Original color-output stores are replaced only in this separate diagnostic/capture PSO; this shader must never replace the game's color pixel shader as-is.

`PackedMotion.cpp` and `PackedMotion.hlsl` isolate the proposed bounded overlap store. Each contributing layer issues one 64-bit maximum operation to a root-descriptor raw UAV. The high 20-bit key selects the nearest candidate and the remaining bits retain signed object motion, material weight and a frame-local object ID as one indivisible value. The independent GPU test requires Shader Model 6.6 and `Int64ShaderOps`, submits seven competing layers to five pixels, and verifies every decoded winner by CPU reference. This establishes the storage primitive only. Runtime pixel addressing, native material instrumentation, per-frame clearing, boundary composition and FG replacement are not connected by this test.

The algorithm and creation observer remain in this module. The upstream D3D12 device hook has explicit startup and final-root-creation integration calls. Acquisition build 32054cc is staged in MO2 Root for fresh-process validation. Its correction still uses static-world projection; the shader capture has no production draw caller yet.

`OriginalColorAndCapture` preserves the original color exports and appends a rasterizer-ordered raw-buffer capture in the same draw. A bounded per-object rectangle stores surface motion/depth and RGB transmission without a second material evaluation. Original discard still executes. Missing history or an out-of-range capture address skips only the added storage. The host must validate `MaterialCaptureConstants` against the actual allocation and prove the original pass has read-only depth/stencil before using its early-depth variant.

`GeometryPipeline.cpp` creates a separate extended root and validated VS/PS pipeline. It preserves original parameters, ranges, flags and static samplers. Added parameters cost 16 DWORDs: a 36-table game layout fits in 52 DWORDs. Material data uses a root CBV instead of 16 inline constants. Collisions in register space 31 and roots exceeding the hardware budget are rejected. `GraphicsRootBindings.h` restores observed original root values after insertion, including partially set constants; unknown command state must bypass insertion. Creation and shader compilation belong on a worker, never in a draw callback.

VS and PS signature extents can differ. The recorded VS-only `SV_ClipDistance` made independently appended history varyings occupy different registers; all 68 original shader/input/root combinations initially failed modified PSO creation despite passing individual DXIL validation. `GeometryCompiler` now passes the actual rewritten VS register to the PS rewriter. All 68 combinations then passed creation on an independent NVIDIA device. Missing rasterizer/alpha-blend fields in that local audit used neutral values, so this is shader/root linkage evidence rather than a complete original-game PSO replay. The public fixture now includes an unused VS-only clip-distance output to cover this failure.

`CyberpunkCamera.h` decodes an already identified 848-byte camera constant block and projects engine bounds to a storage rectangle. Bounds never define material coverage or motion. The adapter still has to prove b1 binding, executable layout, recording/frame identity, stable upload bytes and conservative bounds. The producer must detect coverage escaping the rectangle and reject that object. An eye-plane crossing uses a budget-dependent full-viewport fallback.

`GeometryInstance.h` adds an immutable per-instance mapping from a draw to independently owned object history and coverage allocations. `PerInstance` shaders carry an uninterpolated map index into the PS, so reordering a batch does not move an object's history or merge its mask with another object. This layout adds one shared root SRV: 18 additional DWORDs, or 54 for the recorded 36-table layout. The original draw is not split. The adapter must supply verified engine identities and nonaliasing allocations; the map does not discover those identities.

An exact all-zero mapping is now an explicit inactive instance. It retains its position in the batch, renders original color, and writes no history, coverage or object-status record. A partially populated zero-generation mapping remains invalid, as does a wholly inactive draw. The VS sends a sentinel map index for an inactive allocation; the PS skips added motion/storage work after that input is available. A valid allocation with a missing vertex still marks the object's rejection status and must not be silently treated as inactive.

Mapped capture atomically marks the object's status if contributing material pixels escape its rectangle, use incomplete history, or have nonfinite motion. A consumer must reject the entire flagged object, including any valid-looking pixels already captured. Status records must be cleared once before that frame's material draws. Atomic OR is required because different screen pixels can update the same object status; ordinary ROV ordering alone is insufficient. These atomics are on rejection paths. Invalid previous vertex tags select the current position as a finite placeholder while retaining the missing-history flag.

`GeometryPipelineCache` owns copied root/shader/input-layout data and original COM identities. A single worker compiles modified pipelines; draw-side lookup returns an immutable shared lease and performs no compilation or driver call. Default limits are 128 roots, 2,048 pipelines and 128 MiB of copied CPU data. The byte limit is not a bound on driver PSO memory. The lease retains all pipeline/root data after the cache stops; a renderer must retain it until GPU completion **and** recording discard.

`GeometryCreation` observes successful public graphics PSO creation on the actual selected device. Root bytes come from each final upstream creation branch, including sampler reserialization. Compiler-generated calls are excluded. Pipeline-stream calls are counted and forwarded, but are currently unsupported by the rewriter. Unknown or over-budget pipelines keep the original rendering path. Callback code remains resident for process lifetime; an owning control thread can stop and join the cache. No static destructor joins a worker under the loader lock. `GeometryHost` admits the inspected executable and packaged DXC pair before starting this observer. It records no draw or FG replacement.

## GPU validation

`GeometryShaderGpu` additionally records 32 consecutive fixture frames in one
command list. It reuses exactly two vertex-history buffers, with explicit
read/write transitions and no interframe CPU wait, readback, history copy or
allocation. Original and modified stream outputs are retained solely as a test
oracle and read once after the final submission completes. All 576 current
vertices match the original shader exactly; all 558 accepted previous vertices
match the immediately preceding original outputs. Warmup rejects history.
The existing material-MV/color/depth regression also passes. Local evidence is
`work/glass-history-adjacency-v1/gpu-batch-result.txt`.

This is a single ordered command-list test, not proof of game multi-list or
cross-queue ordering. Test stream-output/readback storage is not the proposed
runtime allocation. The live owner still must prove queue/recording ordering,
identity, view and lifetime before reusing these buffers. Its current diagnostic
ABI exposes no safe original index-buffer capture service either. Dense game
boundary MV and FG integration remain incomplete. Resource transitions follow
Microsoft's [resource-state synchronization contract](https://learn.microsoft.com/en-us/windows/win32/direct3d12/using-resource-barriers-to-synchronize-resource-states-in-direct3d-12).

Run `build_geometry_shader.ps1` from an x64 Visual Studio Developer PowerShell. It uses the repository's pinned DXC binary, generates shaders from the included synthetic HLSL, runs the actual production rewriter/assembler/validator, and creates an independent NVIDIA D3D12 device. It does not attach to or modify a game. It has no Python dependency.

The fixture uses indexed, instanced geometry with nonzero IA start offsets, time-dependent deformation, camera movement, perspective, and a material with both a curved alpha contour and a discarded internal gap. Five frames test warmup, valid history, changed generation, and stale-frame rejection. Stream output independently reads original and instrumented vertex results.

Observed in the current test:

- All 122,880 original color pixels and all 90 emitted current vertex positions remain bit-identical.
- All 36 accepted previous vertex outputs match the prior original vertex outputs exactly.
- All covered/noncovered material pixels agree; invalid-history frames produce no admitted pixels.
- Across 6,017 accepted motion pixels, the maximum difference from double-precision perspective correspondence is 0.001586 pixels (including rasterization precision).
- Material scalar attenuation matches the fixture's original alpha within `3e-7`; history writes leave the allocation exterior unchanged.
- CPU allocation checks reject overflowing instance/address ranges, zero generation, and empty buffers.
- The simultaneous color/capture pipeline preserves all 122,880 original color pixels. Its 6,017 captured motion/depth records exactly match the separate material pass; RGB transmission matches the original material. A padded, offset rectangle leaves storage outside the allocation unchanged.
- A draw after restoring the original root/PSO produces the same original color. The 36-table root test preserves descriptor ranges (including an unbounded range) and rejects collisions/overflow. Analytic camera tests cover fixed origins, jitter signs, offset viewports, perspective bounds, eye-plane crossings and invalid inputs.

The test found that an interpolated valid value of one becomes `0.999999940395` at some pixels. Comparing it exactly to one created coverage holes. The implementation instead interpolates a missing-history flag: zero means valid. Zero interpolation is exact, and a nonzero contribution from a vertex with missing history rejects the sample. This avoids loosening the validity threshold.

The nonzero draw start offsets also exposed an incorrect initial test assumption. IA offsets are distinct from the shader's vertex/instance system values. The final test uses the actual raw-index/instance origins and verifies captured slots against stream output. See [Microsoft's extended command information specification](https://microsoft.github.io/hlsl-specs/proposals/0015-extended-command-info/).

Local, unpublished game shader inputs were also processed: 17 vertex shaders (including 11 observed transparent variants) and four material pixel shaders assembled and passed DXIL validation. Pixel output initialization followed by an unconditional final overwrite is supported; branch-local exports are rejected. Validation of those shaders is not proof that their complete live root bindings or frame histories have been acquired.

`GeometryInstances.cpp` changes the order of three overlapping instances across five frames. It compares each object's stored coverage against a separate original material draw and its motion against independent perspective correspondence using original VS stream output. All 89,600 original color pixels remain exact; 2,399 admitted motion pixels pass with maximum error 0.001688 pixels. The comparison includes 810 overlapping object samples, opaque depth rejection, generation replacement, one missing vertex, escaped bounds and recovery after clearing status. Test identities come from synthetic instance data. These checks do not establish the engine adapter or FG quality.

The subsequent eight-frame fixture also disables one overlapping object's map for a frame. Its old history/pixel bytes and every guard/status record remain unchanged by that object. Returning to an active map rejects absent history, then recovers on the following frame. All 143,360 original color pixels remain exact; 3,563 admitted MV pixels and 896 overlap samples pass, with maximum error 0.001688 pixels. The production creation observer remains enabled for this independent GPU test. This adds no live game or FG quality evidence.

The same test obtains its modified pipeline through the production compiler worker, overwrites the caller's copied shader bytes, stops/destroys the cache, and then renders through the retained lease. Its `--observe` run installs the real public D3D12 creation hooks on the independent device and uses the final-root wrapper. The original color/MV checks still pass. The run also verifies recursive compiler exclusion, unchanged failed creation, forwarding/counting a real pipeline-stream creation, rejecting an over-budget PSO and stopping further admission. It never attaches these hooks to a game. A Release x64 OptiScaler solution build including this adapter passed. The recorded 68 shader/root combinations also pass the mapped 54-DWORD root audit; the same incomplete-original-descriptor limitation applies.

## Runtime contract still required

- Stable live object/generation/chunk/vertex identity. A changing instance batch, reused resource address, particle birth/death, or topology change must not inherit another element's history.
- Both history allocations validated with `VertexHistoryConstants::valid`, immutable per-recording data, real queue ordering, GPU completion, and resize/cut invalidation.
- Actual rendering provenance excluding HUD; original read-only depth/stencil semantics; complete root/state restoration and shader compatibility checks.
- Separate per-object coverage before detecting boundaries. A union mask loses outlines behind other transparent objects.
- Correct FG frame, jitter convention, viewports, and resource-scale mapping, then actual FG input replacement.

The same-draw capture implementation is independently tested but has no production caller yet. ROV ordering applies within one draw; overlapping writes from different draws require a UAV barrier or another proven dependency. The instance fixture verifies separate overlapping objects and opaque depth rejection. It does not test several material fragments accumulating in one object's pixel or overlapping writes from different draw calls. Runtime allocation, engine identity, live ordering and FG consumption remain incomplete. See [Microsoft's ROV ordering contract](https://microsoft.github.io/DirectX-Specs/d3d/RasterOrderViews.html) and [early depth/stencil semantics](https://learn.microsoft.com/en-us/windows/win32/direct3dhlsl/sm5-attributes-earlydepthstencil). No live performance or ghosting-improvement claim follows from this independent test.
