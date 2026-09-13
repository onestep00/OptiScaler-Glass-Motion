# Native motion tool rules

- Read this file and ../EngineMotionSupply.md before changing these tools.
- Work offline from local cache inventories; never publish extracted game shader binaries.
- Preserve original shader outputs. Compiler or expression checks do not prove live inputs or FG quality.
- Use `--workspace` for the local inventory/output directory. Keep shader analysis out of the render loop.

## Index

- `export_motion_declarations.py`: pending bounded declarations and exact VS/PS cache pairs; records unpaired variants, no automatic deployment or writer/history admission.
- `check_motion_declarations.py`: batch-build owned declaration/scope/relocation/callback and foreign-startup tests; never attaches to the game.
- `audit_motion_gaps.py`: group all unresolved candidates by position/control dependencies; input presence does not prove history supply.
- `check_native_projection.py`: owned split-output checks for both native camera layouts and invalid candidates.

- `read_modifier_contracts.py`: cache-wide named modifier rows and shader aliases; no runtime reads.
- `match_shared_native_motion.py`: native position/control graph matching, both prior-camera layouts and split outputs, unit-amplitude specialization and coverage-preserving terminal-collapse matching.
- `graft_native_motion.py`: transplant native prior-position arithmetic and validate DXIL; engine slot admission remains separate.
- `verify_native_grafts.py`: check original-output preservation and cyclic expression/control equivalence after named-row relocation.
- `plan_motion_slots.py`: cache-wide shared-declaration VS/PS footprint audit with cached disassembly; free shader rows do not prove native writer or runtime admission.
