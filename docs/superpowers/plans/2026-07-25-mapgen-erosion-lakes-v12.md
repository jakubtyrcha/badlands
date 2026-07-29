# Mapgen Erosion v1.2 Implementation Plan (conical lakes, rim fix, iteration default)

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Implement the spec's v1.2 addendum (docs/superpowers/specs/2026-07-24-mapgen-erosion-lakes-design.md): conical cavity carve at 1/3 mountain slope, height-above-water detail fade replacing the distance shore band, iterations default 40×dt2 — then a 40/20/10 preview ladder for judging.

**Architecture:** All in `src/mapgen/` on branch feat/mapgen-erosion-lakes. `carve_cavities` signature changes (slope-based); `gully_detail_delta` fade internals change (no interface change); `ErosionParams` loses `lake_depth_m`, defaults change.

## Global Constraints

- Deterministic byte-identical generation; structural tests only; world-metric units.
- Test binaries: `./build/badlands_generator_tests`, `./build/badlands_erosion_tests`.
- Commit style `feat(mapgen):`/`test(mapgen):` + Claude co-author line. No `third_party/` staging.

---

### Task 1: Conical cavity carve

**Files:** Modify `src/mapgen/erosion.hpp`, `src/mapgen/erosion.cpp`, `src/mapgen/erosion_tests.cpp`, `src/mapgen/generator.cpp` (call site), `src/mapgen/generator_tests.cpp` (any lake_depth_m references).

**Interfaces:** New signature:
```cpp
// Carve the bottom lake_frac quantile of bedrock into inverted-cone basins:
// depth = slope_m_per_m * (exact EDT world-meter distance to the nearest
// NON-basin texel). Depth scales with basin size (uncapped), mirroring the
// mountain cone at the caller's slope. Returns the basin mask.
Field2D<uint8_t> carve_cavities(Field2D<float>& B, const Field2D<float>& bedrock,
                                float lake_frac, float slope_m_per_m,
                                glm::vec2 texel_m);
```
Implementation: quantile mask as today; invert it (`mask == 0 → 1`), `distance_to_mask(inverted, texel_m)` (from `mapgen/generator.hpp`) gives distance-to-rim inside basins (0 outside since every non-basin cell is its own seed); `B -= slope_m_per_m * d` for mask cells. `ErosionParams::lake_depth_m` is REMOVED (update every reference; the micro-fill/pour/prune params are untouched). Caller passes `kSlopeMPerM / 3.0f` — add `inline constexpr float kLakeSlopeDivisor = 3.0f;` next to `kSlopeMPerM` in generator.cpp, or simply `kSlopeMPerM / 3.0f` with a comment citing the addendum (user-directed 1/3).

- [ ] **Step 1: Failing tests** — rewrite the two carve tests: (a) on the 100×100 ramp fixture: coverage ≈ lake_frac unchanged; every carved depth == slope · EDT-distance to the mask complement (compare against a brute-force nearest-non-mask scan on the small grid, exact within float); non-mask cells untouched; (b) single circular basin (radius 8 texels, texel 2 m): center depth ≈ slope · 8 · 2 m (margin one texel); (c) determinism. Also update the "lake_frac > 1 clamp" test (drop its lake_depth_m bound assertion; keep coverage + only-lowers).
- [ ] **Step 2: RED** (signature mismatch compile failure).
- [ ] **Step 3: Implement**; update `generate_map` call site; remove `lake_depth_m` from `erosion.hpp` (and its mention in the header comment of `micro_fill` if any).
- [ ] **Step 4: GREEN both binaries + full build** (generator_tests' BiomeSimCapture cavities capture unaffected — mask semantics unchanged).
- [ ] **Step 5: Commit** `feat(mapgen): conical lake cavities at 1/3 mountain slope`.

### Task 2: Height-above-water detail fade

**Files:** Modify `src/mapgen/detail_filter.cpp`, `src/mapgen/detail_filter_tests.cpp`.

Contract: remove the `distance_to_mask` shore-distance fade entirely. New pre-pass (serial, before parallel_tiles): multi-source BFS from all wet cells (`water_depth > 0`), 4-connected, bounded to `kShoreBfsTexels = 16` rings, propagating the SOURCE cell's water surface (`base + water_depth` at the wet seed). Deterministic: process ring by ring; a cell reached by multiple sources in the same ring takes the max surface (max is order-free → deterministic). Store `near_surface` (float, NaN/−inf sentinel for unreached). Per-texel fade: wet → skip (delta 0, unchanged); dry with `near_surface` valid → `fade = smoothstep(0, kShoreFadeHeightM (2.0f), base − near_surface)` clamped ≥ 0; unreached → fade 1. Constants replace `kShoreFadeDistM`.

- [ ] **Step 1: Failing tests** — (a) RING REGRESSION (the headline): sloped base (0.05 m/m) with a water pocket; assert the detail delta has no step discontinuity along a transect crossing the old 3 m band: max |delta(x) − delta(x+1)| over the transect ≤ what the smoothstep bound implies (compute: amplitude · smoothstep slope · per-texel height change + octave noise bound; practical assertion: no adjacent-texel delta jump exceeds 25% of detail_amplitude_m anywhere within 10 texels of the shore, which the old hard band violates at the band edge — verify RED shows a violation there); (b) at-shore zero: dry texels with base ≤ near-water surface get delta == 0; (c) high bank: a cell 3 m above the pocket surface within 5 texels gets full-strength carving (nonzero, magnitude comparable to far-field); (d) zero under water + determinism unchanged (existing tests).
- [ ] **Step 2: RED.**
- [ ] **Step 3: Implement.**
- [ ] **Step 4: GREEN + full build.**
- [ ] **Step 5: Commit** `feat(mapgen): detail fades by height above nearest water (rim fix)`.

### Task 3: Iteration defaults 40 × dt 2

**Files:** Modify `src/mapgen/erosion.hpp` (defaults `iterations = 40`, `dt = 2.0f` with a comment citing the A/B), plus any test that implicitly depended on the old defaults (tests set explicit values — verify by grep).

- [ ] Steps: grep-audit defaults usage → change → both binaries + full ctest green → commit `feat(mapgen): default sim 40 iterations x dt 2 (measured equivalent, 2x faster)`.

### Task 4: Preview ladder + evidence

- [ ] Full `ctest --test-dir build` green.
- [ ] Previews seeds 1-3 at new defaults into scratchpad `v12_out_s{1,2,3}` (timed).
- [ ] Ladder via the -O2 standalone bench pattern (seed 1, res 512, sim 512): (40, dt2), (20, dt4), (10, dt8) → PGM→PNG final-height + water mask + flow per rung into `v12_ladder/it{40,20,10}/` + timings.
- [ ] Report: lake survival counts per seed (vs v1.1's collapse), rim check (transect delta continuity at a shoreline), evidence paths. NO further retuning.

## Self-review notes
- carve signature change ripples to erosion_tests fixtures using lake_depth_m — Task 1 owns ALL of them.
- BFS max-per-ring rule keeps determinism without ordered queues; document in code.
- Task 2's (c) "full strength" bound must tolerate ridge_mask stacking (compare against a far-field cell at similar slope, not an absolute).
