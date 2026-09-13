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
- `check_motion_declarations.py`: batch-build owned declaration/scope/relocation/callback and foreign-startup tests; never attaches to the game.
- `audit_motion_gaps.py`: group all unresolved candidates by position/control dependencies; input presence does not prove history supply.
- `check_native_projection.py`: owned split-output and exact jitter-subtraction checks; invalid components, sign, W and rows reject.
- `audit_native_instance_inputs.py`: all original velocity VS current/prior data and control inputs; dependency presence is not grouped history admission.

- `read_modifier_contracts.py`: cache-wide named modifier rows and shader aliases; no runtime reads.
- `match_shared_native_motion.py`: native position/control graph matching, both prior-camera layouts and split outputs, unit-amplitude specialization and coverage-preserving terminal-collapse matching.
- `graft_native_motion.py`: transplant native prior-position arithmetic and validate DXIL; engine slot admission remains separate.
- `verify_native_grafts.py`: check original outputs, current jitter convention and cyclic native-prior equivalence; declaration export requires all checks.
- `plan_motion_slots.py`: cache-wide shared-declaration VS/PS footprint audit with cached disassembly; free shader rows do not prove native writer or runtime admission.
