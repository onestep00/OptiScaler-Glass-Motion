# Recorded FG replay rules

- Read this file, the parent test rules and README.md before changing the harness.
- Never attach to the game, send input, install DLLs or write into game/MO2 paths.
- Use caller-supplied manifests and local binaries. Do not distribute NVIDIA binaries, game buffers or shader dumps.
- Preserve frame order, pointer-payload remapping and reset semantics. Reject unknown non-null resource pointers.
- Keep creation defaults visibly distinct from captured evaluation parameters and unavailable provider history.
- API success and reproducibility are not visual quality acceptance. Record actual input and generated-output differences.
- Build and generated files belong under artifacts or a caller-supplied fresh output directory.
- Update document metadata and this index when changing documentation status.
- material_fixture.py and test_material_motion_blend.py are optional synthetic-scene experiments with caller-supplied frozen material buffers and extra Python dependencies. They do not reconstruct live object motion or game refraction. Treat preservation of foreground material and independent transmitted background motion as separate quality requirements; lower whole-image error alone is insufficient.
- Optional UI-alpha/recomposition manifests are standalone only. Validate R8 input sizes and preserve the default replay path. A tagged material mask is not enough: correct final/HUDless composition and color domain must be supplied. Do not infer optical-flow-only selection from a zero motion vector.

## Document index

- [README.md](README.md): Manifest-based standalone provider harness. Repeated 24-frame 4x outputs match the previous replay; eight-frame 2x check passes. No completed ghosting fix or broad scene acceptance.
- The README also records a separated-layer synthetic FG experiment and the independent GPU compositor contract, including a distinct correction footprint. Known synthetic correspondence or footprint is not evidence for a game capture/admission path.
- The same README defines per-input allocation/extent mapping and normalized endpoint offsets. `test_layer_spaces.py` verifies different resolutions and padded subrectangles; it does not validate internal DLSS-stage access or a color-space conversion. Old absolute-UV fixtures must be regenerated.
- The continuous material-motion section records six rejected weights/depth policies across 504 actual FG outputs, including a diagnostic for the moving background erasing stationary material. No new game candidate was deployed.
- Alpha/zero-motion results distinguish unchanged RGB from ordinary RGBA A edits, rejected MV-zero edits, and a separate UI recomposition path that improved stationary scalar-alpha surfaces but worsened moving material. Runtime integration remains incomplete.
