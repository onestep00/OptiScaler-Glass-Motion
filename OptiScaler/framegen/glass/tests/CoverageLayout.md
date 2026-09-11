# Bounded geometry coverage storage

- Created: 2026-09-11
- Updated: 2026-09-11
- Status: layout implementation and CPU address checks pass; runtime bounds producer absent
- Deployment: none; existing game correction unchanged
- Deprecated: no
- Scope: packing supplied current geometry rectangles into the existing per-instance bit-mask ABI

`GeometryCoverageLayout.h` packs one status word and tightly packed coverage bits
per admitted rectangle. It allocates nothing, preserves inactive draw positions,
checks viewport/address/capacity limits, and leaves output unchanged on rejection.
It produces `GeometryInstance` entries with zero vertex history; mask layout alone
does not authorize object MV. CPU work is two linear passes over instance metadata.

The caller must provide conservative bounds for the current geometry, including
clipping and deformation. The helper does not estimate bounds from an image or
reuse a previous-frame rectangle. It must not run while the output mapping is in
GPU use. Reference masks, GPU resources, ordering and allocator ownership are
separate caller responsibilities. No production call site is connected yet.

A same-draw live diagnostic captured original VS outputs and per-instance material
coverage for 40 cups. In sampled frames 53616, 53691 and 53770, projected positive-W
vertex bounds expanded by one pixel contained all 455,846 / 457,961 / 457,509
contributing samples (overlap counted per instance). Estimated packed bit storage
was 129,708 / 130,492 / 130,656 bytes, excluding status words, compared with
18,432,160 bytes for full-screen per-instance masks including status. This verifies
three recorded samples only; near-plane crossing, arbitrary displacement, temporal
ownership and the live bounds producer remain unverified. The rectangles were
computed from actual VS geometry, with material masks used only to check coverage.

Evidence is local `work/glass-array-bounds-live-v1/bounds-analysis.json` and raw
paired captures. Generation 13 completed/unloaded all 64 captures, with 261 total
jobs retired and none pending. These are diagnostic readbacks, not a continuous
runtime design or GPU timing measurement.

Build `GeometryCoverageLayout.cpp` using MSVC C++20 `/O2 /W4 /WX`. Its independent
CPU fixture checks non-word-aligned rows, disjoint status/pixel addresses, outer
guards, exact-budget success, insufficient capacity, overflow and transactional
rejection. It passed with `COVERAGE_LAYOUT_OK`. It does not execute the shader,
derive engine bounds or prove FG quality.
