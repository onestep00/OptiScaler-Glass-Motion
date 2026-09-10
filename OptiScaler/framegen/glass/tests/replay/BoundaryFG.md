# Boundary width tests through actual DLSS Frame Generation

- Created: 2026-09-11
- Updated: 2026-09-11
- Status: bounded synthetic and recorded-game comparisons complete; ghosting fix not accepted
- Deployment: standalone test tools only; no installed DLL or game setting changes
- Deprecated: no
- Scope: exact synthetic geometry motion, followed by actual recorded Cyberpunk Backbuffer/HUDless/MV/depth inputs

## What the two experiments establish

The geometry fixture can isolate camera motion, independent object translation/rotation, overlapping transparent coverage and edge width. It cannot reproduce Cyberpunk material/refraction behavior. A favorable synthetic number is therefore insufficient for deployment. The recorded-game experiment keeps the actual images and camera packets, but lacks per-object identity, previous object transforms and per-object material alpha. Its static-world auxiliary-depth motion must not be represented as the newly validated full engine object-motion path.

Both experiments load the locally supplied native FG provider and save its actual 25%, 50% and 75% outputs. Neither uses a substitute image interpolator. All input/output identities and evaluation results are checked by `run.py`. No NVIDIA binary, game recording or shader dump is distributed in this repository.

## Synthetic 3D control

`object_motion_fixture.py` generates opaque background geometry, an independently moving background rectangle, two overlapping low-opacity surfaces and an opaque phase control. `test_object_motion_fg.py` gets endpoint MV/coverage/depth from the actual `ObjectMotion.exe` GPU rasterizer. The camera and object histories match each endpoint. Fractional-time truth uses a separate inverse projective-plane calculation. These are rigid quads; this FG experiment does not add game animation, refraction, SR/RR or tone mapping.

Camera packets are rebuilt consistently: row-vector current-to-previous clip matrices and inverse, view projection and inverse, camera position/basis, near/far 0.1/100, forward-Z, zero jitter and normalized current-to-previous MV. The observed private native adapter uses pixel MV scales and degree FOV; public `sl::Constants` has different units. Do not copy native packet constants directly into the public API. The native parameter ABI is observation-based and not a public compatibility guarantee.

The two scenes use 12 rendered endpoints at 1280x720 with equally sized MV/depth. Boundary widths 1, 2 and 4 therefore refer to output pixels too. Every admitted edge selects the object's full vector. Ambiguous overlapping edge ownership is bypassed. The interior either blends by actual fixture scalar alpha or retains the original background MV. Each edge policy is tested with original depth and with the selected edge's surface depth. This is a diagnostic choice; a single depth/MV pair does not physically encode two independently moving transparent layers.

Thirty native runs produced 1,080 outputs, including 72 repeated baseline outputs that matched bit-for-bit. GPU geometric MV error against the independent reference was at most 0.000493 output pixels. Selected FP16 vectors matched the intended FP16 object vectors exactly; quantization error was at most 0.002466 pixels. The opaque phase-control mean centroid errors were 0.2355 and 0.2687 pixels. Input correctness did not ensure acceptable FG quality.

Across frames 4-11 and all three generated phases, the small object's edge-band RGB MAE was:

| Policy | Stationary surface, moving background | Camera plus object motion |
| --- | ---: | ---: |
| Original background MV/depth | 3.0673 | 5.7837 |
| Alpha-blended interior, 1 px edge MV | 3.8072 | 6.3114 |
| Alpha-blended interior, 2 px edge MV | 3.8736 | 6.3564 |
| Alpha-blended interior, 4 px edge MV | 3.9290 | 6.4156 |
| Background interior, 1 px edge MV + depth | 2.9691 | 5.4112 |
| Background interior, 2 px edge MV + depth | 2.8962 | 5.2692 |
| Background interior, 4 px edge MV + depth | 3.0090 | 5.3516 |

MAE is in 8-bit RGB levels. The 2-pixel edge/depth variant reduced this edge score about 5.6% and 8.9%, but the stationary object's interior error increased from 1.8030 to 1.9294. Alpha blending damaged transmitted background detail. No setting passed all surface/interior/background requirements. Widening the edge is not monotonically beneficial.

Run preparation, execution and analysis separately with `--stage prepare`, `run` and `analyze`, or use the default `all`. Required preparation arguments are `--template <local-manifest> --geometry <ObjectMotion.exe> --executable <replay-exe> --output <fresh-directory>`. Use `--stage extend --output <directory>` to add the six `boundary1/2/4[_depth]` policies with no interior blend, then run those names with `--variants`. Their inputs reuse the same geometry and color; extension source identities are stored separately. Repeat baselines through `run.py --compare ... --expect-identical`.

Local evidence: `work/glass-object-boundary-fg-v1/`, including source/binary identities, GPU raw outputs, exact uploaded MV/depth, input audits, manifests, native reports and `analysis.json`. Synthetic images were directly reviewed before switching to the real capture.

## Actual Cyberpunk input sequence

The recording contains 32 rendered frames, original Backbuffer (slot 0), original HUDless (slot 7), original MV/depth (4/5), same-frame auxiliary surface depth (6), and captured deployed corrections (8/9). Color is 3840x2160; MV/depth is 2560x1440. Both original color buffers are passed to their corresponding `DLSSG.*` resource slots without modification. The final generated output can still contain HUD from Backbuffer. A shown Backbuffer screenshot is not evidence that HUDless was omitted.

All compared Backbuffer and HUDless files have identical hashes. In frame 12, HUDless SHA-256 is `64cd4ed4447edb1437f32ce4b4698935ccbb5b75b3da4e4e552f0c927486bb91`; Backbuffer is `c1b485fd1f1a53fc449d9cabe24cdd48488365e62e0afab7ad5f36d2fe25b19f`. Replay uses the original recorded GFR1 packet, including camera matrices, jitter and extents. Original and previously deployed variants reproduced all 192 earlier standalone outputs bit-for-bit. Creation defaults and internal provider history remain fresh, not restored from the live game.

`recorded_boundary_ablation.py` identifies the nearer side of a jump in the auxiliary depth, requiring separation from opaque depth. It widens inward through compatible auxiliary depth, gates previous-depth consistency and derives static-world MV from the captured clip transform. A cut in the foreground mask alone does not create an edge. Outside the admitted mask, original MV/depth and MV Z/W remain unchanged. The CPU distance transform is a diagnostic, not a production pass or a 1 ms claim.

The 1/2/4 render-pixel bands span approximately 1.5/3/6 final-output pixels. Frame 12 selected 8,438 / 17,192 / 31,165 pixels, with median actual MV changes of 28.44 / 27.11 / 29.62 render pixels. Thus the test really changes the FG input. This is a depth-derived visible silhouette, not a verified per-object mesh outline. Nearer transparent layers can hide other outlines, and moving eyewear can violate the static-world assumption. No material alpha or exact independent object animation was recovered by this experiment.

Ten variants produced 960 native FG outputs: original, captured deployed correction, six width/depth combinations, and two combinations that add 2/4-pixel edge/depth bands to the captured deployed correction. The latter preparation was reproduced by the committed combination helper; all 128 MV/depth files matched the evaluated inputs. To prepare that control, add `--prepared <boundary-directory> --base-overrides <captured-deployed-inputs>` with a fresh output directory.

Repreparing all six boundary-only input sets with the final source also reproduced all 384 MV/depth files exactly. `depth-reproduced/` contains this check and the corrected applied-change preview, which masks out unselected candidate vectors. `final-validation.json` records both reproduction counts.

Full frames and the actual three generated phases were directly inspected. In frames 4 and 12, original FG has large duplicated/warped glass outlines. Wider edge/depth input makes some stems/bases more coherent, but leaves displaced cup-body contours and broken rims. The captured deployed body correction remains materially different from edge-only selection. Adding wider boundaries to it does not provide a convincing complete fix. Frame 21 shows the same unresolved behavior from the other camera position. These are visual observations, not a numerical game-quality improvement claim; there is no rendered ground truth at the generated times.

`review_recorded_boundaries.py` checks identical color identities and successful native reports before producing endpoint, generated-phase and full-frame comparisons. `--roi-map` supplies per-frame rectangles when the camera moves the cups to another screen location. Difference values describe output sensitivity, not quality. The optional `--video-variant <manifest-stem>` writes a slow full-frame comparison with actual 25/50/75 outputs and rendered endpoints; the 125-frame lossy preview is separate from authoritative raw outputs.

Local evidence: `work/glass-recorded-boundary-fg-v1/` and `depth-boundary/` for native reports/input audits. `review-final/` contains the selected whole-frame/phase comparisons, actual HUDless preview, reviewed moving-location crops, color-identity checks and video. The earlier `review/` used a fixed screen rectangle; its frame-21 crop mostly shows people and must not be treated as a cup metric.

## Admission decision

Neither boundary widening nor the synthetic geometry helper is enabled in the installed game. The real capture does not yet contain the exact per-object motion/coverage required by the full proposed method. Obtain and verify those inputs before claiming that method has been tested end-to-end in Cyberpunk. Continue to evaluate separate surface/background preservation; single-MV selection has not established that constraint in these recordings.

Official input contracts checked during this work: [DLSS-G resources](https://github.com/NVIDIA-RTX/Streamline/blob/main/docs/ProgrammingGuideDLSS_G.md#51-required-and-optional-resources) and [common constants](https://github.com/NVIDIA-RTX/Streamline/blob/main/include/sl_consts.h). The public API requires consistent depth/motion and frame-specific camera constants; it does not guarantee that a transparent boundary becomes a hard interpolation barrier.
