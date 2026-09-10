# Glass FG module rules

- Read this file and README.md completely before changing this module.
- Keep algorithms, shaders, game-specific identification and queue bookkeeping in this directory. Keep upstream host changes limited to explicit integration calls and the project import.
- Preserve existing ASI loading, MFG unlock, kernel selection and interpolation-count behavior. Do not duplicate those responsibilities here.
- Never identify transparent depth from dimensions or a resource address alone. Reject unsupported executable fingerprints, ambiguous candidates and stale or unordered snapshots.
- Resource ownership, GPU completion, command-list state restoration and resize/feature teardown must be verified before enabling runtime integration.
- Preserve the repository's PCH conventions. Do not add dependencies to pch.h for this module.
- Distinguish compilation, recorded-input replay, live runtime integration and visual acceptance. Do not label the current candidate a finished ghosting fix.
- Update document metadata and this index when adding or changing documentation status.

## Document index

- [README.md](README.md): Active experimental module. Native session coordinator, two-queue 4x replay equivalence, admission/lifetime tests, Release build, controls and timing verified. Contains OptiScaler reuse and platform callback contracts. Windows hook wiring, game deployment and final visual acceptance remain incomplete.

- [tests/AGENTS.md](tests/AGENTS.md): Standalone GPU ownership/timing and headless settings test boundaries.
