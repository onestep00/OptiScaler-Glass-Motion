# Native material motion supply and creation gates

- Created: 2026-09-13
- Updated: 2026-09-13
- Status: startup declaration consumption and original b7 GPU supply verified on a selected route; 40 native MV grafts validate offline; all-route coverage and FG integration incomplete
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

Local tools/evidence added under `work/glass-native-material-v1/`:

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
All paths in this paragraph are under `work/glass-native-material-v1/`.

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
