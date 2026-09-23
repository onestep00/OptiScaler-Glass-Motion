# Native material motion supply and creation gates

- Created: 2026-09-13
- Updated: 2026-09-24
- Status: 240 ordinary grafts and 10 separate preskinned-input candidates validate offline; selected startup/b7 supply verified; broader native inputs, all-route MV and FG incomplete
- Applied: diagnostic only; installed OptiScaler correction unchanged
- Deprecated: no
- Scope: common material modifier supply, shader declaration creation and framework feasibility; all world transparency remains the objective

## Verified supply

The current executable has a shared material-modifier list evaluator. It filters
records by update category and dispatches enum 7 (`EMATMOD_MotionMatrix`) to the
object transform supplier. The supplier resolves the actual packet's registry
slot, calls the proxy's history-capability getter, and reads the current/previous
root transform. The supplied byte weight controls native interpolation; missing
or invalid history follows the engine's current-transform fallback.

The output row is the signed byte in the modifier payload, not a universal
`b7` offset. Four float4 rows are written and the material block is marked dirty.
For weight 255, its first 48 bytes preserve the selected packed transform, including
integer translation bit patterns. A later shared uploader copies a 448-byte block.
This does not yet prove its final GPU binding at every transparent draw.

The 2026-09-13 live diagnostic on process 21760 captured 4,096 actual evaluator
calls. All record lists parsed within fixed bounds. Of 1,275 calls with a motion
record, 1,122 selected its update category. All 1,118 calls admitted by the checked
mesh layout/weight/output conditions matched the expected current or previous
transform exactly. The 229 with different current and previous transforms all
wrote the previous transform. Observed output rows were 1, 2 and 3.

There were 2,821 calls without a motion record. Among the checked mesh layouts,
760 still had a readable accepted history record; 140 of these differed from the
current transform. Thus missing material declaration does not imply missing
object history. None of the 386 observed array-proxy calls had a motion record.
A root transform must not be substituted for independently moving array elements.
These observations are not a transparency classification or all-scene coverage test.

## Creation conditions and the next common hook

The upstream builder has three distinct conditions:

1. The combined compiled-layout modifier mask must request bit 7.
2. The named constant map must contain `MatMod_MotionMatrix`. The lookup returns
   its row byte, or `0xff` if absent. The builder omits the record on `0xff`.
3. At runtime the modifier evaluator's update category must include bit 4.

The two creation inputs originate higher up in the shader cache provider.
The actual provider's metadata getter was resolved from its live interface.
It returns a 32-byte record containing a request mask and a list of 16-byte
name/row records. The layout builder merges metadata and then copies it into the
compiled layout used by the modifier-record builder. This is a common interception
point; no per-material filename lookup is required to observe the mechanism.

A stable read-only inventory of the loaded provider contained 1,019 records:
208 requested motion, 107 named its slot, 101 requested it without naming a slot,
and none named it without requesting motion. The inventory read 148,776 bytes in
total. Individual cache records are merged by the layout builder, so a missing
name in one record does not establish that the final combined shader lacks it.
The inventory is limited to currently loaded metadata.

An isolated process executed copied original lookup, dispatch, supplier and
record-loop functions against owned data. Eight cases checked absent names,
disabled update category, enabled supply at different rows, valid/invalid/missing
history and zero weight. Expected output bytes and untouched destination ranges
all matched. The full cache/layout builder was not executed in this fixture:
its request-mask branch was inspected statically, and the fixture formed the
resulting small record list explicitly. Intermediate weights remain untested here.

The next experiment must couple declaration changes to valid storage and shader
consumption. Merely forcing a branch or inventing row 2 can overwrite other
material parameters. Use a private augmented layout with proved free storage, or
a separate owned constant binding, and validate the resulting shared shader
input rewrite. Preserve the original native interpolation when borrowing its
supplier. Existing-instance caches and layout invalidation must also be addressed
before an in-game creation-condition change can affect already built materials.
Do not broaden the independent per-vertex history implementation before resolving
this shared supply route.

## Supplying missing records in the running game

A second bounded diagnostic borrows the original evaluator's three-field context
and invokes the original native supplier into an aligned private CPU block at row
zero. It does not change the original record list, material declaration or output
buffer. Only checked mesh proxies with no instance-array source/global allocation
and weight 0/255 are admitted in this diagnostic; arbitrary proxy getters and
intermediate weights are not called. The actual engine supplier and leaf helpers
are byte-verified before attachment. The existing observer's chained entry is
verified against its loaded module, and an independent Detours chain test passed
ten concurrent replacement cycles before the game attachment.

The game completed 4,096 observed calls. In 1,800 calls without an existing motion
record, the private block was filled by the engine and matched the expected root
transform exactly. This included 95 calls with distinct previous/current roots.
All 1,800 original 448-byte material destinations remained bit-identical across
the additional supplier call. There were 448 distinct admitted proxy addresses;
this is not a lifetime-stable entity count. The 523 observed array-proxy calls
were excluded from this root-only supply. The process remained responsive and
recording was stopped. Both diagnostic hooks remain pinned and forward while off.

This establishes an in-game route around an absent modifier record without
overwriting arbitrary `b7` rows. It does not yet establish which supplied calls
belong to the selected transparent GPU draws, exact N-1 render-token identity,
any per-element array motion, new pixel MV or FG input substitution. The private
CPU block is currently retained only in diagnostic records. The next integration
must carry its verified draw provenance into the existing explicit-matrix GPU
binding and shared vertex-position calculation, then verify resulting vertices.
Do not describe the current CPU block as a shader input already installed.

## Draw-time GPU supply and flag experiment, 2026-09-13

The actual scene used by the native material caller comes from the renderer at
executable RVA `0x3427c00`, then member `0x4628`. It is not the pointer at proxy
offset `0x68`. An initial read-only hypothesis using that proxy member failed.
The corrected native caller path resolved all 202 nonzero proxies in a fresh
203-entry census to their actual registry slot. This after-capture check alone
does not establish draw-time ownership.

The replaceable `native-world-gpu/NativeWorldObserver.dll` subsequently resolved
the registry again inside the actual capture callback, checked the draw proxy,
called the original supplier, and placed 48 output bytes into the existing
per-capture upload SRV. The registry/context/header checks were repeated before
accepting the result. Source shader bytes and native helper bodies were checked.
No arbitrary history addresses, image estimates or parent-root substitution for
arrays were admitted. The existing upload/capture slots remain owned until GPU
completion and recording discard.

The game produced 64 completed GPU vertex captures from three proxies using
pipeline 958's original VS. Forty-four N-1 pairs compared 2,984 vertices with no
invalid values and a maximum screen-coordinate difference of 0.000241491 pixels.
Motion was small (maximum 0.359400 pixels); this does not prove large-motion
quality, all shader families, full material contours or FG substitution.

`FlagWorldObserver.dll` additionally calls the original named-slot constructor
`0x11923fc` and original modifier evaluator `0x1f0418`. Its private declaration
requests bit 7 and row 24. The request-bit branch and one-record container are
constructed by the adapter, not the entire native builder. In every admitted
draw, update mask 0 leaves the owned block untouched; mask 4 makes the engine
fill the new MotionMatrix slot. That slot's first 48 bytes reach the GPU SRV.
This second live run produced 64 captures, 47 N-1 pairs and 2,399 valid compared
vertices; maximum error was 0.000276900 pixels and maximum motion 0.484175 pixels.
Both GPU observers unloaded. The installed FG correction remains unchanged.

## Native declaration-provider hook, 2026-09-13

A read-only inventory of 19,037 binary-provider records resolves shader cache
identities through each binary record's `+8` metadata key to the actual modifier
metadata. Preserve all cache aliases for equal shader bytes. Pipeline 958's
captured VS aliases resolve to metadata key `0xccd5360c883c5323`; its PS resolves
to `0xf23347db6fb057ff`. The original VS has no b7 loads; the inspected PS reads
only b7 row 2. These shader-specific results are not general free-slot admission.

`CyberpunkDeclarationProbe.cpp` (standalone diagnostic, not imported into the host
project) is now installed in process 21760 at the real
provider method RVA `0x2adc5c`. For the selected VS metadata key and exact expected
32-byte record, it returns an owned immutable copy with request bit 7 set and
`MatMod_MotionMatrix` assigned row 24. Other keys/results pass through unchanged.
It preserves the original metadata/name storage and retains its 64-byte clone
in the pinned module. This is a scoped diagnostic, not a material whitelist to
ship or a completed general implementation. An owned test and warning-clean
build passed before attachment.

The native provider was then called through its hooked entry in the game:
the returned declaration was the augmented copy, and the original record was
bit-identical. The initial counters were one call/one match/no rejection, entirely
from that explicit verification call. **This is not evidence that the normal
game layout builder has consumed the declaration.** Existing compiled-layout
caches were not invalidated. The hook remains enabled; its control exports can
record counters or stop redirection without unloading the pinned module. A later
snapshot recorded 191 calls and still only the one explicit matching call: normal
provider activity exists, but the selected declaration has not been requested
again by the normal game path.

The immediate remaining task is native compiled-layout/record refresh or a
same-call augmented-record route that demonstrably uses the engine's ordinary
448-byte uploader. Validate the shader's slot consumption with that route.
Do not describe the existing private SRV experiment as this original upload path.
Trace the opaque/grouped draw conditions in parallel with this integration:
the latest 73-pipeline census found native velocity shader matches only on
single-instance observed draws. Its grouped opaque matches were highlight
passes, so it does not establish how grouped opaque velocity is supplied.
The full cache velocity-VS inventory now contains 879 unique binaries and is
available for this comparison. Do not assume a root MotionMatrix covers array
elements simply because the common declaration bit can be enabled.

Local tools/evidence added under `glass-native-material-v1/`:

- `audit_opaque_velocity.py`, `opaque-velocity-audit/index.json`: all cache velocity VS extraction/signatures; no live grouped-motion admission.
- `capture_velocity_groups.py`: existing unloadable census extended to opaque draws.
- `check_draw_supplier_context.py`: bounded read-only registry/context check.
- `prepare_native_world_capture.py`, `prepare_flag_world_capture.py`, `run_native_world_capture.py`, `analyze_native_world.py`: live GPU supply/flag experiments, with completed results under `native-world-gpu/`.
- `resolve_motion_metadata.py`, `motion-metadata-resolved.json`: live binary-to-metadata mapping, including aliases.
- `build_declaration_hook.py`, `native-declaration-hook/`: build/profile, owned test and status for the currently enabled upstream declaration probe. The canonical source is the module's `CyberpunkDeclarationProbe.cpp`.

## Startup consumption and original GPU upload, 2026-09-13

The user accepted a startup/load-time adapter. The RED4ext build of
`CyberpunkDeclarationProbe.cpp` was placed in MO2's
`overwrite/red4ext/plugins/GlassMotion/GlassMotion.dll`, not an independently
registered MO2 mod. Its SHA-256 is
`9dfd2d1cea433eae7e369a9426e622c1e1e080d3964900b0658ad53a8d0e7ee4`.
Owned self-tests, exports and rejection of a foreign executable passed before
deployment. The plugin attaches only the code hook during RED4ext Load and
initializes the immutable declaration on its first real provider callback.
Game objects and allocators are not dereferenced during plugin Load.

Fresh process 43504 actually loaded that file. Its normal engine calls requested
the augmented declaration 34 times out of 8,376 observed provider calls, with
zero rejection and zero manual provider verification calls. This supersedes the
preceding process's one-manual-call-only result. The metadata scope is still the
single checked key; global transparency admission is not established.

The existing bounded output observer then recorded 4,096 original evaluator
calls. In 198 calls on 45 distinct proxy addresses, the native record list included
the new row 24 and the original evaluator filled that slot. All 198 outputs were
readable/nonzero and their proxy roots stayed stable across the call. The observer
did not call the supplier itself. No admitted row belonged to an array proxy.
Current/previous equality counts overlap and are not counts of moving objects.

`prepare_engine_bound_capture.py` changes the diagnostic shader's previous-root
binding to the game's original b7 rows 24..26. It calls no supplier and uploads no
private matrix. The original root bindings reach the diagnostic shader unchanged.
Its 64 completed GPU captures produced 53 N-1 pairs and 8,427 compared vertices,
with zero invalid vertices and maximum error 0.000169611 pixels. Maximum observed
motion was 0.371287 pixels. This proves the selected declaration-to-native-upload
route; it is not complete world coverage or generated-frame quality evidence.

`match_shared_native_motion.py` now compares original position-expression graphs
using logical input semantics and referenced resource contracts. It does not
match by material names. Across 879 native velocity VS and 675 transparent VS,
42 transparent VS have an exact current-position match plus a native prior-clip
candidate. Loops/unresolved controls and nonmatching expressions remain explicit
gaps. The matcher is a candidate selector, not a runtime binding validator.

`graft_native_motion.py` copies the original native previous-position arithmetic,
including original bone addressing/selection, and reuses identical target
subexpressions. It relocates the native motion matrix reads to b7 rows 24..26 and
retains the target's original color/position outputs. Forty grafts pass DXIL
validation and exact expression checks against the native source after row
relocation; all original output SSA values and definitions remain unchanged.
Two matched candidates lack the three-row root contract and remain unsupported.
These 40 binaries are not enabled globally: their complete VS/PS storage/supply
and grouped-instance contracts still require admission.

The grafted shader for the already supplied route was then executed in the game
using the same original b7 upload. In 64 captures, 47 N-1 pairs compared 7,473
vertices with no invalid values and maximum error 0.000096012 pixels. That run had
negligible motion (maximum 0.000105668 pixels), so it validates binding/arithmetic
consistency but not moving-object quality. No single-object visualization was
presented as a full-screen result. Both GPU observers unloaded after capture.

The next required result is full-screen geometry-derived object/edge MV with
coverage across supported routes, not another selected-object image. Final
composition must preserve these user requirements: actual object boundaries use
object MV; interior weight is `saturate(opacity * InteriorStrength)` with a numeric
control (zero means boundary-only); overlapping coverage selects the nearest
camera surface before MV composition. Unrelated background pixels remain intact.
No new FG input has been connected by these experiments.

Local evidence: `startup-native-output-latest.json`,
`engine-bound-gpu-1789232792886130800/capture-1789232842864661300/analysis.json`,
`shared-native-motion-matches.json`, `native-grafted/index.json`,
`native-grafted/verification.json`, and
`native-graft-gpu-1789233454907652200/capture-1789233464654268300/analysis.json`.
All paths in this paragraph are under `glass-native-material-v1/`.

## Broader original shader reuse, 2026-09-13

The preceding 40-graft result used an incomplete prior-output detector. Many
native velocity VS pack previous clip components into different output semantics.
The current component-level detector recovers prior clip from 837 of 879 native
velocity VS; 42 still lack an admitted complete prior-projection result. No
previous component is inferred from an output name alone.

The matcher now compares ordered expression/control graphs, including loop
backedges, without unrolling loops or estimating positions. The original cache's
named modifier metadata provides row-relative identities for material constants.
The extracted contracts cover 7,477 shader binaries and 1,019 parameter sets;
1,281 shader binaries have multiple parameter-set aliases. Used rows must have
consistent named meanings across aliases; ambiguity rejects that correspondence.
Names are used to relocate constant fields, not to select material families.

This identifies native reuse candidates for 69 of 675 transparent VS. All 69
grafts pass DXIL validation: 49 use expression grafting and 20 retain original
native control flow for previous deformation/garment calculations. The latter
retain required original branch predicates and loop operations, remove irrelevant
value computations, and do not introduce a replacement deformation algorithm.
Target values that dominate the insertion point can be reused. This reduced
generated instruction lines in 14 shaders by 510 lines in aggregate; it is a
static source count, not a GPU instruction count or timing claim.

All 69 pass the original-output preservation check and complete cyclic
expression/control comparison against the native source after relocation. The
comparison permits common-subexpression sharing but requires equal operations,
inputs, branch polarity, predecessor dependencies and resource contracts. This
supersedes the straight-line-only verifier. Native loop-control hints are omitted
from the graft; arithmetic and control semantics remain checked.

The remaining 606 transparent candidates are explicit gaps: 601 have no exact
current-position correspondence under the current contracts, and five have
multiple stores to the same output component. The last game check still covers
only the earlier scoped declaration/b7 route. The 69 generated shaders are local
offline artifacts, not globally deployed material flags or full-screen MV.
Shader-specific constant semantics, independent grouped instances, particle
inputs and final full-screen admission remain required. Do not equate this
compiler result with support for every transparent object.

Canonical tools are in `tools/`; local workspace wrappers forward to them so
future runs do not silently use the preceding local implementation. Extracted
game binaries remain outside Git. Current results are
`shader-modifier-contracts.json`, `shared-native-motion-matches.json`,
`native-grafted/index.json`, and `native-grafted/verification.json` under the
local inventory workspace.

## Preserved coverage, specialization and shared slots, 2026-09-13

The matcher now canonicalizes only commutative add/multiply operands and paired
phi inputs. CRC32 graph colors are sorting hints; acceptance still compares the
complete graph. Unit-amplitude specialization of a native material scalar is
allowed only when the resulting complete current-position graph equals the
target graph. This first raised the checked graft count from 69 to 86.

An additional recognized target pattern chooses either its computed clip position
or exactly `(0,0,0,1)` through four phis sharing the same terminal decision. The
matcher compares the pre-collapse geometry. It does not remove the branch,
change SV_Position, expand the original material coverage, or substitute a
different visibility test. Nonliteral collapse and different component decisions
are rejected. Previous visibility/history validity still requires runtime proof.

The complete inventory now yields 215 candidates out of 675 transparent VS.
Of these, 209 compile and pass original-definition, original-output,
original-branch and native previous-expression checks. There are 168 DAG and
41 native-control-flow grafts; 123 retain the target's terminal coverage collapse.
Fifteen have an originally shorter b7 declaration, expanded to the known native
448-byte supply requirement in the generated shader metadata. This does not
establish the actual bound resource size for every game draw.

Six matched candidates remain rejected because their native previous path requires
t9 and b3 contracts absent from the target shader. The other 460 inventory entries
remain unmatched or have multiple stores per position component. No missing
binding is silently synthesized, and no image-derived motion is substituted.
The 209 generated VS resolve to 83 provider metadata keys; none of their audited
partner contexts already declares a conflicting MotionMatrix slot.

`tools/plan_motion_slots.py` joins every stage of every technique sharing any
target provider key, including nontransparent aliases. For the 675 target VS,
151 metadata keys affect 2,071 shader aliases and 10,689 technique combinations.
The tool extracted and disassembled 3,780 unique VS/PS binaries. No unresolved or
dynamic b7 load occurred, and none read b7 rows 24..27. Native MotionMatrix writes
four rows, so reserving only the three rows read by the VS is insufficient.
The cache, compiler and analysis source identities accompany the saved results;
cached footprints avoid repeated disassembly when those inputs are unchanged.

Read-free storage is not full producer admission. The provider name table mixes
constant rows with resource binding indices. Static native inspection confirms
that `MatMod_GarmentMorphOffsetScales` feeds the resource-binding helper while
`MatMod_GarmentMorphOffsetScalesIsBound` writes one b7 row; the same separation
applies to `MatMod_CullObjectsCB` versus `MatMod_DismParams`. The conservative
planner does not yet classify every writer, so its 103 tail candidates are not
103 approved declarations. Actual writer spans, grouped proxy capabilities,
previous deformation inputs and upload/draw ownership still gate expansion.

Local evidence in `glass-native-material-v1/`: `motion-slot-plan.json`,
`motion-slot-audit/footprints.json`, `modifier-writer-audit/constructors.json`,
`native-grafted/index.json`, `native-grafted/verification.json` and
`native-grafted/admission-checks.json`. The latter checks reject malformed
coverage decisions, dynamic/out-of-range b7 accesses and unresolved handles.
The native constructor/supplier map covers the original 32 modifier categories;
it is static inspection, not runtime coverage. Process 43504 was still responsive
at this checkpoint. No new DLL or full-screen FG input was deployed in this step.

## Bounded declaration and actual shader-pair scope, 2026-09-13

`MotionDeclarationTable.h` replaces the single-key implementation in the pending
adapter with a fixed-capacity table: at most 256 declarations, 4,096 source names
and 32 names per augmented declaration. Original records and names remain owned
by the engine. The adapter clones only the requested metadata, appends the native
MotionMatrix name and reserves all four rows 24..27. It validates actual name/hash
and row values on each provider call, including after a pointer is reused. The
first clone initialization has a mutex; subsequent reads use immutable published
storage. This table holds no object positions, GPU buffers or frame histories.

Metadata keys are not shader identities. The 83 selected keys are shared with
unselected particle/fullscreen and opaque/depth programs. The pending adapter
therefore additionally hooks the original stage resolver. That function selects
the VS from combination+8 for stage 0, or PS from combination+16 for stage 1,
resolves its binary, then requests the binary's metadata before merging the mask
and names into the compiled layout. The original merge and uploader remain native.

`MotionShaderScope.h` authorizes an exact VS/PS cache-identity pair and its expected
VS metadata. The scoped thread-local selection exists only across that native
stage call. Metadata changes require both the expected key and the verified
direct metadata-call return site. Pixel stages, unselected partners, calls outside
the scope and unrelated nested calls forward unchanged. Nested stage calls clear
or replace the outer selection and restore it on return. This is shader/layout
creation work, not per-draw matching or a material-name whitelist.

The pending export contains 1,854 VS/PS pairs covering 204 of the 209 grafted VS.
Five grafted VS occur only in ten selected VS-only depth/G-buffer techniques and
are explicitly absent from this pair adapter. These inventory flags are candidate
scope, not proof that every pair contributes visible transparency to FG. The
other 466 VS still require their original motion paths. A stable read-only snapshot
of process 24404's 19,647 loaded combination records contained all 1,854 requested
pairs in stage-0 ordering, with no missing pair. It read 3,143,592 bytes twice/with
header checks in total; that one-shot diagnostic copy is not part of runtime.
This establishes loaded cache correspondence, not execution of the new hooks.

Startup function discovery uses `RelocatableCode` profiles for six audited native
functions. Only relocatable address operands are normalized; data layouts, opcode
bytes and branch structure remain checked. The actual referenced lookup helper
must match its profile. Owned PE tests relocate the metadata and stage functions,
reject duplicate matches and a wrong callee, and verify the relocated metadata
return site. This handles supported address relocation, not arbitrary code changes.
The preceding manual diagnostic entry still uses its exact fingerprint/RVA profile.

`tools/check_motion_declarations.py` builds and runs the bounded table, scope,
compatibility, actual adapter callback and negative startup checks together. The
maximum table check exercised 8,192 concurrent calls. The scope check exercised
32,768 calls across eight threads. The adapter's owned fixture passed all 83
declarations and 1,854 stage pairs while preserving original metadata and pair
records; pixel stages and unscoped calls stayed unchanged. A foreign executable
was rejected before any hook attempt and wrote the explicit rejection status.
These checks use owned memory and do not execute or attach to the game.

The two fixed tables occupy 256,096 bytes together, plus small counters and TLS.
There is no per-frame file read, shader hashing, GPU readback or GPU synchronization
in this selection path. Existing object-supplier cost and new rendering work are
separate and are not measured by these CPU checks.

The new startup adapter reports configured/prepared declarations, configured
pairs, stage hook installation, selected/out-of-scope calls and native layout
rejection in `status.json`. The pending adapter remains uninstalled. Process 24404
still loaded the preceding GlassMotion DLL hash `9dfd2d1...e4` and OptiScaler host
hash `997b5465...204`; the original version.dll ASI loader was present. The new code
does not establish all native writer spans, array element history, live b7 upload,
full-screen object/edge MV or a new FG input. Those remain required before broader
runtime admission and completion.

After code/table validation, the startup adapter pins its own module before
attaching callbacks. This keeps immutable metadata and forwarding code mapped
even if a subsequent attach or detach fails; Stop disables redirection. The
startup adapter lasts until process exit, while the separate diagnostic-host
experiment DLL remains replaceable. This follows the documented
[GetModuleHandleExW PIN lifetime](https://learn.microsoft.com/en-us/windows/win32/api/libloaderapi/nf-libloaderapi-getmodulehandleexw)
and the SDK's separate
[Attach/Detach results](https://raw.githubusercontent.com/WopsS/RED4ext.SDK/master/include/RED4ext/Api/v1/Hooking.hpp).

Local evidence: `pending-motion-declarations/manifest.json`,
`shader-pair-live-1789261207863059700.json`,
`declaration-adapter-1789261537898194200/result.json`, and the stage/constructor
disassemblies in `modifier-writer-audit/`. Extracted cache/engine binaries and
diagnostic builds remain local.

## Second native camera layout and remaining input groups, 2026-09-13

`audit_motion_gaps.py` groups every unresolved entry in the 675-VS base-cache
candidate inventory by its actual position/control dependencies. It records
camera/material/global rows, instance/skinning inputs and resource reads. Family
names are report labels only. This found a substantial set using b1 rows 0..3
where the earlier native reference set primarily projected through rows 28..31.
Those matrices are not assumed interchangeable.

The earlier detector had excluded 42 of 879 native velocity VS because they use
b1 rows 12..15 for prior clip instead of rows 16..19. The original
`79f7efb...a454` VS, for example, exports the four prior components across output
5.w and 6.xyz after reading rows 12..15 and the original b7 MotionMatrix. Its
actual paired PS `682c2fb4...b035` writes native velocity target 3 using those
components divided by prior W minus the corresponding current components divided
by current W, scaled by `(0.5, -0.5, 1000)`. This confirms an actual native MV
consumer, not just a guessed output name or a copied camera matrix.

`native_previous()` now recognizes both complete, unambiguous component layouts.
It keeps the original native arithmetic and bindings, records the chosen camera
rows, and rejects incomplete, conflicting or mixed-current candidates. All 879
native velocity VS now expose a candidate: 837 use rows 16..19 and 42 use rows
12..15. The complete current-position graph still must match the target before
copying any previous calculation. No camera-row equivalence was introduced.

This expands exact transparent matches from 215 to 250. Of those, 240 grafts pass
DXIL validation and the original-definition/output/branch plus native-prior
expression verification. Thirty-one are new; none of the preceding 209 was lost.
The four other new candidates lack required native resource contracts and remain
rejected, joining six preceding rejections. Five other shaders have multiple
position-component stores and 420 have no admitted native current-position match.

The pending declaration export now contains 85 declarations, 221 names and 2,024
VS/PS pairs covering 235 grafted VS. The five VS-only entries remain unpaired.
All 85 declarations and 2,024 pairs passed the actual adapter's owned callback
fixture. This newer 2,024-pair set has not been live-hook tested; the preceding
read-only loaded-cache proof covered 1,854 pairs. No new DLL was installed.

There are 435 unresolved candidate VS. Of 430 parsed entries, 227 read all camera
rows 0..3, 187 read all rows 28..31, 256 use material b4, 130 use global b0, 245
use instance-transform inputs, 128 use skinning inputs and 185 read position
resources. These overlapping counts are dependency groups, not supported-object
counts or proof that a prior input is missing. Particle/procedural and remaining
deformation supply, native writer spans, broad live binding, full-screen MV and
actual FG remain incomplete.

A sharing-independent graph comparison was also evaluated over the full native
and target sets. It added no candidate and removed none. That experimental code
was removed rather than adding work to the permanent matching path.

Repeatable commands are `audit_motion_gaps.py --workspace <local inventory>` and
`check_native_projection.py --workspace <local inventory>`. Local evidence:
`native-prior-detector-gaps.json`, `native-motion-gaps.json`, updated
`shared-native-motion-matches.json`, `native-grafted/index.json`,
`native-grafted/verification.json`, and
`declaration-adapter-1789262827914444400/result.json`. The native PS disassembly
remains in the local `modifier-writer-audit/` directory.

## Current clip convention and original batch scheduling, 2026-09-13

The verifier now checks the added diagnostic current-clip output as well as the
native previous expression. It recognizes only the exact original camera binding
and `XY - b1[51].xy * W` using the same clip W. It rejects double subtraction when
the original pre-coverage position already has this form. All 240 existing grafts
pass; this adds no supported shader. Sixteen remaining candidates already contain
explicit jitter subtraction. Removing that difference for a trial comparison
added no native match, so that matcher fallback was removed. Declaration export
requires the new current-clip result. Owned sign/component/row/W negative checks
also pass. These expression checks do not establish the live camera contents.

`audit_native_instance_inputs.py` examines both data and control dependencies of
current and native prior clip across all 879 original velocity VS. All parsed:
720 use INSTANCE_TRANSFORM in the current graph but not the previous graph;
154 use it in both; five use it in neither graph. A declared input alone is not
counted. The 154 include original effect, foliage, diode and distant-crowd/vehicle
variants; they are not one universal grouped-motion implementation. Reading the
current instance transform in a previous-position graph does not prove that the
engine supplies an independent N-1 instance transform. Full dependency reports
remain local for following each actual buffer path without a material whitelist.

The common native creation-to-batch path was traced in the current executable:

1. Builder `0x2ae99c` collects update categories from the actual constructed
   modifier list into material byte `+0x22` bits 0..2. The native category helper
   assigns MotionMatrix to category 4.
2. Map insertion `0x52b848` places the value at node `+0x10`; its copy constructor
   `0x52b964` preserves those category bits. They are consequently at node `+0x32`.
3. Packet builder `0x1e9658` copies these bits into packet qword 0 bits 14..16.
   The alternate array packet builder `0x1ea780` also packs the low three bits there.
4. In `0x1f1208`, a changed low-18-bit registry index enables update categories 5.
   If the packet categories intersect the active update mask, the engine calls
   batch flush `0x1f191c` before the original modifier evaluator `0x1f0418`.
   That flush uploads a dirty 448-byte material block and emits pending geometry.
5. The subsequent array append `0x1f1a88` still consumes the packet's entire
   encoded instance count (qword 1 bits 18..32), copying 48-byte transforms or the
   original 64-byte extended records. There is no per-element MotionMatrix
   evaluation in that append loop.

This explains how native per-proxy material updates can separate pending draws
without proving that an array proxy has per-element history. Adding a declaration
alone must not authorize assigning one root transform to every array element.
The live native packet category, grouping, supplier output and actual shader
input still need joint verification. The fixed RVAs above are local diagnostic
evidence for the recorded executable, not the startup compatibility mechanism.

Local evidence: `native-instance-input-audit.json` (all 879 VS),
`native-batch-gate-audit.json` (ten exact native instruction ranges checked against
the executable SHA), and `audit_native_batch_gate.py`, under
`glass-native-material-v1/`. No game attachment, new DLL deployment, new
full-screen MV or FG substitution occurred during this audit.

## Exact pair direct-writer audit, 2026-09-13

`audit_motion_writer_slots.py` now joins all 2,024 pending pairs through their
actual VS and PS binary-to-metadata identities, rather than every alias of equal
shader bytes. These pairs contain 94 distinct metadata unions. It consumes the
local executable-specific constructor/supplier inspection in
`native-writer-layouts.json`; extracted executable bytes remain local.

Twelve modifier constructors expose 30 payload fields, including the proposed
MotionMatrix. The original presence gates differ: CullObjects/DismParams and
DestructionRegions require both fields before adding a modifier, while several
other constructors accept any present field. A requested mask bit with absent
required names therefore does not necessarily execute a supplier. MaterialParam
and CullObjects constructor dataflow was decoded on the warm-name path; cold
registration and recursive supplier callees are not certified by this audit.

The inspected direct spans distinguish resource bindings (including 77) from
b7 rows. ProxyTranslation writes one row, while the same supplier's other two
matrix payloads can write four rows each. Those matrix names are absent from the
current selected pairs; their spans are still retained in the inspection.
Garment IsBound and DismParams each write one row; their associated resource
slots are separate. The proposed MotionMatrix reserves all four rows 24..27.

No inspected direct write overlaps those reserved rows. 2,018 pairs have complete
direct-span classifications. Six pairs retain modifier 0, whose supplier tail
calls context virtual method +0x20; its actual target remains unresolved. These
six are not silently removed from the target set. Even the other 2,018 do not
establish recursive callee safety, broad live upload, grouped history or FG.
`native_writer_spans_verified` and `runtime_admitted` remain false.

The pending export records this separate result only when cache, exact-pair and
inspection hashes match; it reports stale evidence after an input change and
rejects an observed reserved-row overlap. Twelve owned checks cover multi-row
overlap, 448-byte bounds, signed row bytes, the absent-name sentinel, resource
bindings, constructor gates, unknown/indirect suppliers and conflicting VS/PS
names. No DLL was deployed and no new game MV was produced in this step.

Local evidence: `audit_native_writer_layouts.py`, `native-writer-layouts.json`,
`motion-writer-slot-audit.json`, and `pending-motion-declarations/manifest.json`
under `glass-native-material-v1/`.

## Live supplier context target, 2026-09-13

The existing bounded supplier-output diagnostic was extended locally to record
the first evaluator context argument and its owner/vtable/virtual-0x20 target.
It performs the original pointer loads without invoking that virtual function.
Owned valid/invalid-pointer checks and the preceding output checks passed. The
original 142-byte evaluator entry and executable identity were verified before
attachment to the existing process 24404. No restart or menu operation was used.

The diagnostic captured 4,096 normal evaluator calls and stopped successfully.
3,988 records resolved context vtable RVA `0x2ac8688`, whose +0x20 target is
`0x18ec810`. Its five live bytes matched the original executable exactly:
`lea rax, [rcx+0x10]; ret`. It returns an address and performs no memory write.
The other 108 records did not resolve a target. Crucially, none of the captured
record lists executed modifier 0. This identifies a real context implementation
but does not certify that the six unresolved shader pairs use that context, or
that every possible context has the same virtual method. Their admission remains
unresolved rather than inferred from this sample.

The same recording contains 180 original row-24 outputs from the previously
installed selected declaration, and 302 array calls without a MotionMatrix
record. These are present-process supply observations, not new transparent MV
coverage. No matrix was injected by the recorder, no GPU command was added, and
the installed FG correction remains unchanged. The diagnostic is pinned and only
forwards calls after Save.

Local evidence: `MotionContextProbe.cpp`, `context-probe-latest.json`, and
`context-live-1789265826318681500/{preflight.json,analysis.json,capture/}` under
`glass-native-material-v1/`. Loaded diagnostic SHA-256:
`d9872c13268986d1075989de51acfada5acbe969a8d1ce00bb58078d1181fede`.

## Original preskinned input bridge, 2026-09-13

All ten resource-contract rejections share the same missing pair: raw SRV t9
and a 176-byte b3 constant buffer in register space 0. The original native prior
position calculation uses these inputs; the target has no declarations for them.
Its matching current-position graph and required vertex input semantics remain
available. This is an absent shader declaration, not proof that the engine never
binds the resources. The original 420 unmatched plus five multiple-store targets
remain separate unresolved shader paths.

The native geometry binding path `0x1f1e44 -> 0x1f2b68` reaches `0x76d868` with
the engine's deformation owner, chunk and selector. That function selects an
owner+0x40 handle at `3*chunk + (selector+1)%2`, stores it at descriptor+0x84,
zeros descriptor+0x88, and copies the corresponding two original float4 blocks to
descriptor+0x50/+0x60. The binder sends a valid descriptor+0x84 to resource index
9 through `0x1f5438`, and writes logical constant row 8 and rows 9..10 through
`0x1f5b24`. No per-object name filter occurs in this inspected path.

These are static executable observations. Native prior VS reads b3 rows 1..3;
the logical-bank-to-b3 mapping, selector-to-consecutive-rendered-frame identity,
actual bound resource and lifetime still need verification. The CPU selector
alone is not accepted as N-1 proof. Four original function bodies totaling 1,995
bytes are fingerprinted in local `native-preskinned-supply.json`.

`bridge_native_preskinned.py` adds those exact two shader resource declarations
in a separate candidate directory and reuses `graft_native_motion.py` unchanged
for the original prior arithmetic. It neither allocates previous geometry nor
changes the native binding or selector. Required t9/b3 inputs remain explicit
and unverified in each candidate record. Existing conflicting resource ranges
are rejected instead of overwritten.

All ten resulting candidates passed DXIL validation, exact original definition,
output and branch preservation, current clip convention, and complete native
prior graph comparison. The existing 240 passed the same checks after the
verifier was extracted into shared `native_graft_checks.py`. The pending runtime
export remains the preceding 240-graft set; the ten are not silently admitted.
Full-screen MV and FG input substitution are still incomplete.

The bridge reserves metadata numbers including distinct control-flow nodes and
adds only the required raw-buffer flag, preserving existing flags. This follows
the [DXC shader flag definition](https://raw.githubusercontent.com/microsoft/DirectXShaderCompiler/main/include/dxc/DXIL/DxilShaderFlags.h).
The ten-shader check also rejects repeated/conflicting input declarations and a
deliberately wrong native previous-matrix row. No new DLL was deployed in this
step. Local artifacts are `missing-native-contract-audit.json`,
`native-preskinned-supply.json` and `native-preskinned-grafted/` under
`glass-native-material-v1/`.

## Framework investigation

- [RED4ext Hooking API](https://raw.githubusercontent.com/WopsS/RED4ext.SDK/master/include/RED4ext/Api/v1/Hooking.hpp)
  provides native attach/detach with an original-function trampoline. It can host
  the common engine adapter, but requires a verified function address and ABI.
  [Plugin lifecycle](https://docs.red4ext.com/mod-developers/creating-a-plugin)
  specifies where hooks can be attached and the plugin loading directory.
- [Current render-proxy SDK definitions](https://raw.githubusercontent.com/WopsS/RED4ext.SDK/master/include/RED4ext/Rendering/RenderProxy.hpp)
  leave much of the mesh proxy opaque. They do not expose the complete previous
  transform or material-declaration supply used here as a documented API.
- [Codeware's native hook backend](https://raw.githubusercontent.com/psiberx/cp2077-codeware/main/lib/Support/MinHook/MinHookProvider.cpp)
  also accepts target addresses. Its [mesh script additions](https://raw.githubusercontent.com/psiberx/cp2077-codeware/main/scripts/Base/Addons/MeshComponent.reds)
  expose component settings, not this GPU constant supply.
- [CET's override implementation](https://raw.githubusercontent.com/maximegmd/CyberEngineTweaks/master/src/scripting/FunctionOverride.cpp)
  operates on reflected script/native function objects. No binding for this
  renderer-private supply was established. Adding a native C++ bridge is feasible;
  moving per-draw processing into Lua is not justified by the inspected APIs.

Keep the current native diagnostic/OptiScaler module while proving the common
engine route. RED4ext remains an optional loader/adapter; changing loaders alone
does not resolve the missing metadata or history. Preserve `version.dll`/MFG unlock.

## Local evidence and remaining proof

Local-only artifacts under the workspace `work/` directory:

- `glass-motion-output-1789228413684041200/capture/`: actual call records and output analysis.
- `glass-native-material-v1/MotionSupplierOutputProbe.cpp`: bounded before/after observer; independent self-test passed.
- `glass-native-material-v1/probe_motion_supplier_owned.py`: isolated engine-code fixture.
- `glass-native-material-v1/motion-supplier-owned-analysis.json`: eight fixture results.
- `glass-native-material-v1/capture_motion_declarations.py`: bounded read-only provider inventory.
- `glass-native-material-v1/motion-declarations-p21760.json`: stable loaded metadata snapshot.
- `glass-native-material-v1/motion-cache-provider-live.json`: actual provider and method provenance.
- `glass-native-material-v1/motion-supply-route.json`: preceding static supply path; the live output above extends it.
- `glass-motion-supplement-1789229277036547800/capture/`: missing-record native private-block supply and original-destination comparisons.
- `glass-native-material-v1/MotionSupplierSupplementProbe.cpp`: second observer with bounded native supplier calls; self-test passed, including array exclusion.

Executable SHA-256: `a7de82945c03e041fc7339fcf9066224d98db2f5d80fea50f7947bb350a60991`.
Diagnostic DLL SHA-256: `6119d579e44ae5b0ced0b977d2a64cb5bf521324fb624cf5a18d354d344afdcc`.
Supplemental diagnostic SHA-256: `f5085200d541cccd9809fbea0ff89eb3407c2cfe5492f4013016e15059c6bcf0`.
Recording was stopped successfully; the pinned observer only forwards calls.
No GPU commands, material changes or new FG inputs were introduced by this probe.

Outstanding proof includes native rendered-frame correspondence, original array
elements, skinning/deformation/particle inputs, final GPU binding, exact material
boundaries, FG substitution and generated-frame quality. Correct CPU root output
alone does not complete any of those requirements.

## Product integration and the array convention, 2026-09-23

The supply described above is now the product's source of previous positions.
The module vertex history stays only as the off-by-default
`VertexHistoryFallback`. Architecture and settings are in [README.md](README.md).
Line numbers of the workspace `research/ACTIVE.md` refer to its 2026-09-23
16:30 revision.

Declaration supply. `NativeMotionDeclarations.cpp` replaces the RED4ext build
of `CyberpunkDeclarationProbe.cpp`, which was deleted (commits `f07b40c28`,
`592de369b`). It is installed from `DllMain` at process attach, before the
shader-cache provider builds any compiled layout. It detours the same two
engine entries: the provider metadata getter and the vertex-stage resolution.
Its plans come from `Glass/motion-declarations.bin` (a copy of
`pending-motion-declarations/declarations.bin`) and
`Glass/motion-shader-pairs.bin`, a copy of
`shader-pairs-2018-direct-span-clear.bin` (SHA-256 `827bf29b…`). That file holds
the 2,018 pairs with complete direct-span classifications; the six modifier-0
pairs of "Exact pair direct-writer audit" are excluded (`research/ACTIVE.md:126`).
The status line `DECL` reports `hook`, `seen`, `matched`, `rejected`,
`stage_seen`, `stage_selected` and the step that stopped installation. The
RED4ext plugin must not be loaded in the same process; the integration deploy
moved it to a backup. The integration build (commit `592de369b`) reported
`DECL hook=1 seen=7728 matched=250 rejected=0 status=installed`
(`research/ACTIVE.md:127`). The earlier plugin session (DLL `585b8df5`) had
reported `matched=0`, presumably from cached layout reuse
(`research/ACTIVE.md:49`).

Graft catalog (`glass-native-material-v1/native-grafted/index.json` summary;
exported `index.bin`, 641 `GGRAFT02` records, SHA-256 `52e45006…`):

- 250 transparent VS with a native twin: 226 root grafts (82 class 1, 144
  class 2), 14 `factory_mismatch`, 10 roots not validated. The 10 have preskinned
  (class 4) twins that fail with `missing native resource contract` for t9/b3.
  All 250 camera variants validate. They use b1 rows 16..19 with current rows
  28..31 in 215 cases, and rows 12..15 with 0..3 in 35.
- 445 transparent VS without a twin: 391 camera-only records from two
  canonical templates (`50ba90d4…` for 28..31 → 16..19, `79f7efb4…` for
  0..3 → 12..15) and 54 unsupported. The unsupported reasons are screen space
  11, two-stage projection 17, no `SV_Position` 6, multiple stores 7, and other
  non-VP clips 13 (`research/ACTIVE.md:107`).

A camera variant copies the native previous camera multiply node for node and
substitutes the target's own current world operands. It adds no arithmetic and
reads no b7 row. The verifier requires unchanged original definitions and
stores, the current clip `XY − b1[51].xy·W`, no added b7 load, and a previous
position graph equal to the target's current projection with the camera rows
renamed (`tools/verify_native_grafts.py`, `tools/native_graft_checks.py`).

Array convention. The preceding section left open whether an array proxy has
per-element history ("Current clip convention and original batch scheduling",
steps 4-5). The 2026-09-23 runs answer it for ordinary instanced arrays:

- A live probe (`graftarray=on`, commit `8ff0eaa35`, DLL `538e463e`) applied the
  root graft to array draws. It produced 81 px of error on a still camera
  (`research/ACTIVE.md:69`, `77`). With the root graft, every element moves by
  the proxy root's previous transform because the element-to-root
  `INSTANCE_TRANSFORM` factor is missing.
- Opaque arrays are also drawn as one instanced draw
  (`GATE_DETAIL reason=array instances=40 blend=0`, `research/ACTIVE.md:77`).
- Static evidence agrees. The append copies current transforms only (steps
  4-5 above). The feature `0x20` is not added for 192 of 192 array conditions
  without special input (`CompletionPlan.md:130`). 725 of 879 native velocity
  VS do not read `INSTANCE_TRANSFORM` in the previous graph (720 + 5 above).
  The camera-default velocity initialization applies the previous camera to
  depth (`CompletionPlan.md:142`, `171`). [INFERENCE] The ordinary array
  elements take that camera-only velocity. The technique of the observed glass
  draws was not captured directly.

The product therefore gives array, grouped and multi-instance draws the camera
variant (commit `8ad300854`). Glass-table substitution went from 0 to all array
draws (`array_draws=51,718`). On a still camera the delivered p50 was 0.28 px
against 0.33 px for the engine; while moving both were 67 px
(`research/ACTIVE.md:78`). Independently moving elements (physics arrays)
receive camera motion only, as in the engine.

Factory rule. A current-position graph match is not evidence for the previous
input (`CompletionPlan.md:27`, `48`). Eighteen skinned or garment
targets had current graphs that fold to the rigid path, matched MeshStatic
twins, and received their root-only previous arithmetic. NPC glasses then took
the proxy root's previous transform: 6 px of error with a still camera
(`research/ACTIVE.md:114`) and 6–82 px in the first session
(`research/ACTIVE.md:53`). `graft_native_motion.py` now refuses, for a skinned
factory target, a twin whose previous graph is root-only unless the twin shares
a vertex factory (commits `680525741`, `2bef442ff`). The 14 refused targets
(`cloak`, `cloak_v2`, `optical_camouflage`, `cloak_*_forward`) became
camera-only records. Raising the default `GraftClassMask` to 3 (commit
`592de369b`) was tried on DLL `0c5bb34e` and withdrawn. In all six still frames
the NPC head region (hair and glasses, skinned class 2) delivered about 77 px
where the engine had about 3.5 px. [INFERENCE, `research/ACTIVE.md:128`] The
transparent pass does not keep the velocity pass's previous skinning supply
(previous `INSTANCE_SKINNING_DATA` offset, previous t10 bones). With the default
1 (DLL `580b24ad`) the rerun no longer showed that region
(`class_disabled=348~367`).

Remaining proof for this supply: a previous skinning supply for skinned
transparent draws (class 2), a dedicated recheck of the 6 px glasses error, a
preskinned (t9/b3) previous supply, the 54 unsupported VS, and opaque-probe
equivalence (`research/native-supply-integration-plan.md:90`).

## Vehicle object motion, 2026-09-24

Observation. The City Center street session (DLL `3c33ec9f`, `GraftClassMask`
3, skinned-current catalog; workspace
`glass-live-tools/scene-20260924-skin-street/dumps`, log
`artifacts/glass-log-pid79760.txt`) drew six vehicle VS through camera-only
records: `296676c3`, `bb99bbe6` (`vehicle_destr_blendshape`), `7a31295c`,
`9d76d15a` (`vehicle_glass*`), `f8fbc912` (`glass` on MeshStaticVehicle) and
`c39622e9` (`vehicle_destr_blendshape` on MeshSkinned). Per object id of the
packed record, the delivered motion was the camera's: frame-1 ids 11, 13, 16,
17, 474 and 477 (`296676c3`) and 413, 651 (`bb99bbe6`) delivered 0.00 px where
the engine had 21–23 px; frame-3 moving cars delivered 0.00 px where the engine
had −17.9 to −22.2 px (e.g. ids 151, 199, 212, 25, 28, 30) or 2.2–3.0 px, and
`7a31295c` id 624 delivered 0 px against −21.3 px. Parked cars agreed (≤0.13 px).

Engine route for the same objects. `vehicle_destr_blendshape` has a
`gbuffer_velbuff_regular` technique per vertex factory. Its MeshStaticVehicle
velocity VS `c5783867…` reads the MotionMatrix at b7 rows 4..6
(`opaque-velocity-audit/c5783867….ll:176` b7 handle, `:1018-1031` loads) and the
previous camera b1 rows 16..19 (`:1057-1075`) into output 5 (`:1126-1129`). Its
metadata (`shader-modifier-contracts.json`, key `0xdb997baec20a825d`) names
`MatMod_MotionMatrix` at row 4 and requests bit 7 (mask `0xC2000084`). The
MeshStatic velocity VS `001b21f0…` does the same (`.ll:172`, `:949-962`,
`:988-1006`). Both previous graphs are class 1 (`native-instance-input-audit.json`:
no skinning input, t10 or t9/b3). The MeshSkinnedVehicle VS `3bb98b10…` adds
the skinning inputs (class 2). The vehicle's own transform history is the object
motion; camera motion alone is not the engine's velocity for these objects.

Supply at the transparent draw. The transparent and unlit vehicle VS request no
motion: `296676c3` and `bb99bbe6` share key `0xcd4bded0362ae30a` (mask
`0x90000004`, bit 7 clear; rows 1, 2, 4 MaterialParam, 3
`MatMod_VehicleGridCorners`, 5 `MatMod_VehicleMeshPivotInGridSpace`), `7a31295c`
and `d441e9d5` share `0x85d0bd41fedcea8d` (mask `0x10003004`). Only the
declaration hook adds the MotionMatrix (row 24), and only for the VS/PS pairs of
validated root grafts: `shader-pairs-2018-direct-span-clear.bin` has 12 pairs
for `d441e9d5` and none for `296676c3`, `bb99bbe6` or `7a31295c`. Every
transparency-route pair of the 45 vehicle root grafts is in that file.

Why camera-only. `shared-native-motion-matches.json` lists `296676c3`,
`bb99bbe6` and `7a31295c` as "no exact native current-position match": their
position cones read terms the velocity VS lack (`native-motion-gaps.json`: b1
row 37 and b4 rows 0..3 or 1..3 for the unlit variants; b4 rows 4, 5 and b7 rows
1..3 for the glass). The generic phase then applied the canonical camera
template `50ba90d4…` without regard to object motion.

`d441e9d5` (`vehicle_glass_onesided`, MeshStatic) has a class 1 root graft from
`50ba90d4` (MotionMatrix × POSITION, the computation of `001b21f0`) and its
declaration pairs. Its street residual (mean 0.80 px, p95 5.5 px) splits in
frame-1: pixels whose engine value is the car behind the glass, residual mean
0.25 px (p95 0.75, 313 px); pixels whose engine value is the static background
seen through the glass, 4.98 px (261 px), the glass's own motion and the
intended correction; still objects 0.02–0.09 px.

Rule (`tools/graft_native_motion.py`, `tools/export_native_grafts.py`). A
vehicle target is a VS with the VEHICLE_DMG_POS input, a vehicle damage-grid
modifier (`MatMod_VehicleGridCorners`, `MatMod_VehicleMeshPivotInGridSpace`) or
vehicle vertex factories only: 96 transparent VS. Their root grafts stay (45: 21
class 1, 24 class 2) and keep the MotionMatrix previous world in the
skinned-current variant. Their camera-only candidates (45: 25 class 1, 20 class
2) are not exported; `Glass/grafts/refused.bin` lists them, the module counts
`GRAFT refused` and their draws keep the engine's motion. Vehicle root pipelines
keep the camera variant for array draws; none was drawn as an array in the
street session (`array=0` for every vehicle pipeline, pid 79760).

Not covered by this rule: `glass` on MeshStatic car windows (e.g. `39f8b555`)
carries no vehicle data in its VS and stays camera-only. In the street dumps its
pixels on moving cars were delivered by the paired root VS `7448a1d2` (frame-3
ids 638, 691: residual 0.09–0.10 px). A root supply for the refused VS would
need a MotionMatrix graft without a native twin plus declaration pairs.

## Proxy history convention and stale MotionMatrix rows, 2026-09-24

Observation. The Corpo Plaza terminal screen (`parallaxscreen_transparent`,
root pair `de5e58b4…`/`f17cc532…`, DLL `3c33ec9f`,
`glass-live-tools/scene-20260924-rain/mask-dump`, `mask-dump2`) delivered
0.8–0.9 px on a still camera where the engine background had 0.2–0.3 px. The
same pair on a City Center street had 0.04 px. At the substituted pixels the
delivered field of each draw is affine in screen position (least-squares fit
error 0.03–0.08 px; mask-dump2 id 3: x motion −0.046 px per pixel of x): a rigid
difference between the root graft's previous and current world positions, not
noise. `vehicle_glass_proxy` (`c5b56e2c…`, DLL `2ac5ea9f`,
`scene-20260924b-street/dumps`) delivered 15.5 px where the engine had 6.2 px on
one moving car (frame-1 id 310, 146 px).

Static evidence. Executable `a7de8294…`, disassembled from
`glass-decompile/exe-copy-a7de8294`:

1. Evaluation cadence. The batch run `0x1f1208` starts every packet with
   category mask 1. It sets 5 (categories 1 and 4) when the packet's registry
   index (qword 1 bits 0..17) differs from the previous packet's
   (`0x1f152e`–`0x1f155c`), and every category when the material changes to one
   with modifier records (`0x1f16c9`–`0x1f16da`, `0x11574c`). When the packet's
   category bits (qword 0 bits 14..16) meet the mask it flushes pending draws and
   runs the evaluator (`0x1f180c`–`0x1f1846`); `0x1f0418` runs a record whose
   category (`0x1f04a8`; MotionMatrix is 4) is in the mask. The dirty 0x1c0-byte
   block is uploaded before the draw that uses it (`0x1f1889`–`0x1f18a6`, and in
   the flush `0x1f191c` at +0x3d..+0x11d before Rigid at +0xb9 and Skinned at
   +0x135). A draw's rows are therefore its own proxy's supplier output of the
   current batch run.
2. Supplier output (`0x56c194`). Rows 24..26 receive the history pose (record+4,
   48 bytes) when the proxy's virtual +0x130 returns an object whose +0x130
   record is non-null, the weight byte proxy+0x9e is nonzero and the record
   state is ≤ 1 (`0x56c20e`–`0x56c255`, `0x56c408`). Otherwise they receive the
   proxy's current transform, proxy+0x18 (`0x56c37b`). The 48 bytes arrive
   unchanged: the write transposes the transposed expansion back
   (`0x56c38c`–`0x56c3d5`). For mesh proxies the virtual returns the proxy
   (`0x540500`).
3. History lifecycle. SetTransform `0x1e5414` calls the snapshot `0x1e54c8`
   before `0x1da094` writes the new transform. The snapshot copies proxy+0x18
   into the record: a new record gets state 1, an existing one state 0. The scene
   update `0x3a54dc` runs the jobs `CRenderScene_UpdateState_DuplicateRemoval`,
   `CRenderScene_UpdateState` and `CRenderScene_UpdateState_Epilogue` in that
   order when updates are pending. The epilogue job `0x88e524` calls `0x88e638`,
   which raises every state below 2 by one and releases state-2 records
   (proxy+0x130 = 0). The owned run of the original functions
   (`glass-native-material-v1/original-group-pipeline-owned.json`,
   `root_history_lifecycle`) confirms these transitions. At draw time, state 1
   means the transform was set in this frame's update while a record from one of
   the two previous updates existed; the record then holds the transform before
   this frame's update, the one the proxy was drawn with a frame earlier. State
   2 means the first update after rest (the engine drops that frame's motion) or
   no update this frame. No record means the proxy is at rest.
4. Engine velocity. The collector `0x1e9228` adds the velocity feature `0x20`
   (the velocity-buffer technique) for a non-array proxy only when view feature
   bit 0x1a is set and the record state is exactly 1, or when the motion flag
   (proxy+0x9c bit 0) is set together with active skinning or a special input
   (`0x1e92b1`–`0x1e934d`; 512 owned gate cases in
   `glass-native-material-v1/native-group-pass-matrix-owned.json`). A rigid proxy
   outside state 1 is not drawn by the velocity technique, so its pixels keep the
   camera-only velocity initialization (`CompletionPlan.md:142`, `171`). The
   weight and the motion flag are component settings copied at proxy setup
   (`0x237518`: +0x9e = clamp(setting, 0, 1) × 255, +0x9c bit 0 from settings
   flag 2). Proxy+0x90 is the render tick of the last bounds change
   (`0x1da276`–`0x1da287`; the tick `0x3438a30` is the module's layout tick).
5. INSTANCE_TRANSFORM of a single proxy. The single-proxy packet builders copy
   proxy+0x18 into the renderer transform table and put its index in qword 1 bits
   33..49 (`0x1e7b30`–`0x1e7b63`, `0x1ebf50`–`0x1ebf73`). The draw's instance
   transform and the supplier's fallback are the same 48 bytes. Other builders
   differ: `0x1ecbe0` fills the table from a 4x4 of its own object and clears the
   registry index bits.

Convention. For a rigid proxy the MotionMatrix rows hold the previous frame's
transform exactly when the proxy was re-transformed in this frame's update after
an update in one of the two previous frames. In every other frame they hold the
current transform and the engine's velocity for the proxy is camera-only. The
evaluator never leaves rows of an older frame in place. A single-instance root
draw whose instance transform is proxy+0x18 therefore already delivers
camera-only motion in those frames. A non-zero root motion on a still camera
needs one of: (A) the engine re-transformed the proxy this frame to a different
pose (state 1, rows = history pose ≠ instance); (B) the draw's instance transform
is not proxy+0x18; (C) the rows are not this proxy's supplier output. (A) is the
engine's own convention for the proxy; (B) and (C) are not. The static evidence
does not decide which one holds for the terminal or for `vehicle_glass_proxy`.

Diagnostic (`motionprobe=<hex>`, off by default). For draws whose VS hash starts
with the given 1..16 hex digits the capture logs the rows 24..26 the draw
actually used, read from the engine's own modifier block on the CPU. The
module's Rigid/Skinned hooks find that block in the batch run's frame: the flush
saves the run's rbp, which is the block, in its home slot, and the hook checks
both the flush return address and that saved value before it trusts the address
(`CyberpunkDraws.cpp` `modifierBlock`, `resolveProbeFrames`). It also logs the
draw's INSTANCE_TRANSFORM (the flush's upload source, or the global transform
table for a global flush), the owner proxy's +0x18, its history record, state,
weight, flags and bounds stamp, and the rows compared with the transform the
proxy was drawn with one frame earlier. The first 8 probed draws of every two
seconds print `MOTION_PROBE` and `MOTION_PROBE_M`; every window starts with a
`MOTION_PROBE_SUM` of all probed draws since arming. Line format:
[README.md](README.md#live-channel-counters-and-dumps).

Rule (`stalemotion=camera`, default; `stalemotion=off` for A/B). A
single-instance root-graft draw takes the camera-only variant when the
collector (item 4) gives its owner no object velocity: the history record is
not in state 1 and the motion flag is clear (`PackedMotionCapture.cpp` prepare,
`CyberpunkMotionHistory::cameraOnly`). The collector never reads the weight: a
zero weight only makes the supplier write the current transform into the rows,
and a skinned proxy in state 1 still has the motion of its previous bones, so
such a draw keeps the root graft. With the motion flag the engine adds velocity
when a skinning or special-input component is active; the module does not read
those components, so the flag alone keeps the root graft. The camera-only set
is therefore a subset of the proxies the engine gives no velocity. An
unreadable owner keeps the root graft. There is no blend or clamp; a draw
takes one variant. Counters: `GRAFT stale_camera`, dump variant `stale`. The
rule removes (B) and (C) for proxies the engine gives no velocity; it does not
change (A).

Expected probe readings. A proxy at rest: `rows=cur inst=cur`, `record=0` (or
`record=1 state=2` in the two frames after its last move), `supplied=0`,
`earlier=1`, `dt_mm=0.000,0.000,0.000 dr=0`, `variant=stale` (camera) or
`root` (off). A proxy re-transformed every frame: `rows=prev inst=cur record=1
state=1 supplied=1 earlier=1`, `stamp` equal to or one below `frame`, `dt_mm`
its per-frame displacement, `variant=root`. (B): `inst=other`. (C):
`rows=other`, or `rows=prev` with `earlier=0`. Not yet run in game.
