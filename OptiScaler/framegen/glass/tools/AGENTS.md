# Native motion tool rules

- Read this file and ../EngineMotionSupply.md before changing these tools.
- Work offline from local cache inventories; never publish extracted game shader binaries.
- Preserve original shader outputs. Compiler or expression checks do not prove live inputs or FG quality.
- Use `--workspace` for the local inventory/output directory. Keep shader analysis out of the render loop.

## Index

- `bridge_native_preskinned.py`: separate offline candidates declaring the original t9/b3 preskinning inputs; no live binding creation or pending runtime export.
- `check_native_preskinned.py`: metadata-node, original-output, binding collision, feature-flag and negative prior-row checks.
- `native_graft_checks.py`: common output/current-clip/prior-graph verifier used by both graft paths.

- `audit_motion_writer_slots.py`: exact VS/PS metadata union and local native constructor/write-range inspection; preserves unresolved suppliers and never grants runtime admission.
- `check_motion_writer_slots.py`: owned checks for multi-row overlap, absent-name gates, resource bindings, signed rows and conflicting stage metadata.

- `export_motion_declarations.py`: pending bounded declarations and exact VS/PS cache pairs; records unpaired variants, no automatic deployment or writer/history admission.
- `export_native_grafts.py`: validated grafts to the local `Glass/grafts` catalog (`GGRAFT02`: root outputs, supply class from the previous-graph union, camera-only variant outputs or 0xFFFFFFFF; camera-only records have root outputs 0xFFFFFFFF, no `<sha>.dxil`, and the target's own current-position class); output is extracted game code and stays in the ignored `artifacts/` tree.
- `check_motion_declarations.py`: batch-build owned declaration/scope/relocation tests; never attaches to the game. The hook itself (`NativeMotionDeclarations.cpp`) is verified live through the status `DECL` line.
- `audit_motion_gaps.py`: group all unresolved candidates by position/control dependencies; input presence does not prove history supply.
- `check_native_projection.py`: owned split-output and exact jitter-subtraction checks; invalid components, sign, W and rows reject.
- `audit_native_instance_inputs.py`: all original velocity VS current/prior data and control inputs; dependency presence is not grouped history admission.

- `read_modifier_contracts.py`: cache-wide named modifier rows and shader aliases; no runtime reads.
- `match_shared_native_motion.py`: offline position/control matching. Its nearest-slot b7 labeling mixes resource namespaces; old matches/grafts require revalidation against native writer spans. See ../NativeOpaqueMvRoutes.md before using these results.
- `graft_native_motion.py`: transplant native prior-position arithmetic and validate DXIL; engine slot admission remains separate. Twin selection is factory-consistent: a target with a skinned vertex factory (`*Skinned*` in `all-cache-techniques.json`) refuses twins of another vertex factory whose previous graph is root-only (class 1); a twin sharing a factory with the target is the engine's own velocity route and stays eligible. If no remaining twin validates, its root `status` is `factory_mismatch` and only the camera variant is written. Also writes the camera-only array variant `<sha>.camera.ll/.dxil`: the native previous camera multiply (b1 16..19 or 12..15) copied node for node onto the target's current world operands (the three values its current projection, b1 28..31 or 0..3, multiplies), no b7. Its generic phase (`generic_camera` in index.json) does the same for every other transparent VS (no native twin) from one canonical native template per camera layout; a target whose clip is not a per-vertex view-projection multiply of a world position is `unsupported` with the reason. `--generic-extra` adds observed VS from `extended-position-inputs`.
- `verify_native_grafts.py`: check original outputs, current jitter convention and cyclic native-prior equivalence; for the camera variant (twin and generic), previous clip equals the target's current projection with the previous camera rows and no b7 read. Declaration and graft export require all checks.
- `plan_motion_slots.py`: cache-wide shared-declaration VS/PS footprint audit with cached disassembly; free shader rows do not prove native writer or runtime admission.
