# Recorded FG replay rules

- Read this file, the parent test rules and README.md before changing the harness.
- Never attach to the game, send input, install DLLs or write into game/MO2 paths.
- Use caller-supplied manifests and local binaries. Do not distribute NVIDIA binaries, game buffers or shader dumps.
- Preserve frame order, pointer-payload remapping and reset semantics. Reject unknown non-null resource pointers.
- Keep creation defaults visibly distinct from captured evaluation parameters and unavailable provider history.
- API success and reproducibility are not visual quality acceptance. Record actual input and generated-output differences.
- Build and generated files belong under artifacts or a caller-supplied fresh output directory.
- Update document metadata and this index when changing documentation status.

## Document index

- [README.md](README.md): Manifest-based standalone provider harness. Repeated 24-frame 4x outputs match the previous replay; eight-frame 2x check passes. No completed ghosting fix or broad scene acceptance.
- The README also records a separated-layer synthetic FG experiment and the independent GPU compositor contract, including a distinct correction footprint. Known synthetic correspondence or footprint is not evidence for a game capture/admission path.
