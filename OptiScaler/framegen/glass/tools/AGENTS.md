# Native motion tool rules

- Read this file and ../EngineMotionSupply.md before changing these tools.
- Work offline from local cache inventories; never publish extracted game shader binaries.
- Preserve original shader outputs. Compiler or expression checks do not prove live inputs or FG quality.
- Use `--workspace` for the local inventory/output directory. Keep shader analysis out of the render loop.

## Index

- `read_modifier_contracts.py`: cache-wide named modifier rows and shader aliases; no runtime reads.
- `match_shared_native_motion.py`: semantic/resource-aware native position/control graph matching, including loops and split prior-clip outputs.
- `graft_native_motion.py`: transplant native prior-position arithmetic and validate DXIL; engine slot admission remains separate.
- `verify_native_grafts.py`: check original-output preservation and cyclic expression/control equivalence after named-row relocation.
