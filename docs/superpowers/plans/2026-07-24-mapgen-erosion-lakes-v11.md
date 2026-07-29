# Mapgen Erosion v1.1 Implementation Plan (plains relief + lake cleanups)

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Implement the spec's v1.1 addendum (docs/superpowers/specs/2026-07-24-mapgen-erosion-lakes-design.md, "v1.1 addendum" section): bedrock-potential plains relief, capped micro-fill, lake deposition pour, Lake stamp threshold — then regenerate judging previews incl. an iterations A/B.

**Architecture:** All changes inside `src/mapgen/` (generator.cpp orchestration, erosion.cpp passes) on branch feat/mapgen-erosion-lakes. No interface changes beyond one new `erosion.hpp` function and constexpr constants.

**Tech Stack:** as v1 (C++20, FastNoiseLite, Catch2, glm).

## Global Constraints

- Deterministic: `generate_map` twice → byte-identical (existing test pins it).
- World-metric units; sub-knobs `constexpr`; no new `ErosionParams` fields except noted default change (`sediment_noise_m` 1.0 → 0.3).
- Structural tests only (no seed/golden pinning). Test binaries: `./build/badlands_generator_tests`, `./build/badlands_erosion_tests`.
- Commit style `feat(mapgen):` / `test(mapgen):` + Claude co-author line. No `third_party/` staging.

---

### Task 1: Plains drainage relief

**Files:** Modify `src/mapgen/generator.cpp` (+`generator.hpp` if helper exposed), `src/mapgen/erosion.hpp` (default change), `src/mapgen/generator_tests.cpp`.

**Interfaces:** Produces constexpr `kPlainsReliefM = 2.0f` (generator.cpp anonymous namespace) and a quantile helper if needed: `float bedrock_quantile(const Field2D<float>&, float frac)` (nth_element-on-copy, the `compute_cutoffs` pattern — reuse by `carve_cavities` is NOT required; keep carve's internal computation as-is to avoid churn).

- [ ] **Step 1: Failing tests** — in `generator_tests.cpp`: (a) plains texels are no longer all-zero-based flats: generate (res 96, sim 96, iterations 0 via `p.erosion.iterations = 0`) and REQUIRE the heightmap variance over Plains-classified texels > 0 and max-min over plains ∈ [1.0, 4.0] m (the 2 m term ± cavity effects); (b) smooth blend: no neighbor step in the pre-erosion heightmap exceeds `kSlopeMPerM * texel + 2.0f` (no seam at biome boundaries); (c) determinism unchanged.
- [ ] **Step 2: Verify RED** (variance ≈ 0 today on plains with iterations 0 — sediment noise is on S not the relief... NOTE: heightmap = B+S resampled; S carries fBm noise ±1 m. To make (a) discriminate, assert on a generated map with `sediment_noise_m = 0` override so the only plains variation is the new relief term).
- [ ] **Step 3: Implement** — in `generate_map` sim-grid section, after `cutoffs_sim` and `t_lake` are known (compute `t_lake = bedrock_quantile(bedrock_sim, ep.lake_frac)`), add to `B` for every sim cell: `kPlainsReliefM * glm::smoothstep(t_lake, cutoffs_sim.t_hills, bedrock_sim)`. Order: BEFORE `carve_cavities` (the carve subtracts below `t_lake` where smoothstep = 0 → no interaction). Change `sediment_noise_m` default 1.0f → 0.3f in `erosion.hpp` with a comment (v1.1: texture role only; drainage comes from the relief term).
- [ ] **Step 4: GREEN + full suites** — both binaries.
- [ ] **Step 5: Commit** `feat(mapgen): bedrock-potential plains relief (2 m smoothstep term)`.

### Task 2: Capped micro-fill

**Files:** Modify `src/mapgen/erosion.hpp`, `src/mapgen/erosion.cpp`, `src/mapgen/erosion_tests.cpp`, `src/mapgen/generator.cpp` (call site + sink stage).

**Interfaces:** Produces:
```cpp
inline constexpr float kMicroFillCapM = 0.75f;
// Raise every closed-depression component of h = B + S whose max fill depth
// is <= kMicroFillCapM and which does not touch basin_mask up to its spill
// level (route_flow's epsilon-tilted water_level), crediting S. Deeper
// depressions and seeded cavities are untouched. Returns filled volume (m³).
float micro_fill(Field2D<float>& B, Field2D<float>& S,
                 const Field2D<uint8_t>& basin_mask, float texel_m);
```
Implementation: one `route_flow(h, texel_m, kEpsilonM)`; label 4-connected components of `in_lake` cells (BFS, the `finalize_lakes` pattern); per component compute `max(water_level − h)` and whether any member has `basin_mask != 0`; if depth ≤ cap and mask-free, for each member `S += water_level − h` (water_level from routing, keeps the ε tilt). Deterministic.

- [ ] **Step 1: Failing tests** — (a) shallow bowl (0.5 m deep dent in a tilted plane) → after micro_fill, re-route: no `in_lake` cells remain; volume returned ≈ bowl volume; (b) deep pocket (5 m) untouched (S unchanged); (c) component touching basin_mask untouched even when shallow; (d) determinism.
- [ ] **Step 2: RED** (micro_fill undeclared).
- [ ] **Step 3: Implement** + call from `generate_map` after `init_sediment`, then `sink->dump("micro-fill", seq++, S)` (float stage) — ALSO add the missing post-carve height dump while here: `sink->dump("cavities-height", seq++, B)` right after `carve_cavities` (rendered as hillshade: add `"cavities-height"` to the PngDebugSink relief list in `outputs.cpp`, sim spacing). Update the stage-sequence Recorder test accordingly (order: … "cavities", "cavities-height", "sediment-init", "micro-fill", loop …).
- [ ] **Step 4: GREEN + full suites.**
- [ ] **Step 5: Commit** `feat(mapgen): capped micro-fill conditioning + cavities-height dump`.

### Task 3: Lake deposition pour

**Files:** Modify `src/mapgen/erosion.cpp` (`deposit`), `src/mapgen/erosion_tests.cpp`.

Contract change (see addendum): during the reverse-order sweep, an `in_lake` cell no longer deposits at itself; its available flux (q_in + own eroded volume) is added to its component's bucket and NOT forwarded along the chain. After the sweep, per component (4-connected `in_lake` labeling, deterministic order): pour the bucket bottom-up — sort member cells by ground `h` ascending (ties by linear index); level-set fill: raise the lowest cells to the next cell's level (or the component water level, whichever first) consuming volume, until volume exhausted or all members at water level. Leftover volume walks the component outlet's receiver chain (outlet = follow any member's receiver until `!in_lake`; if that chain enters another lake component, add to THAT bucket only if it has not been poured yet — process components in descending pop-order of their deepest member to make cascade order deterministic; otherwise pass through) applying the normal dry deposition rule per cell; whatever reaches base level exports. Conservation must hold exactly as before.

- [ ] **Step 1: Failing tests** — (a) bottom-up: hand-built lake (3 cells at depths 3/2/1 below water level) with small inflow → only the deepest cell gains S; larger inflow → two deepest equalize before the shallow one gets any; (b) stripes regression: lake of N cells at EQUAL depth with inflow < capacity → S is distributed evenly across members (max−min ≤ ε), NOT concentrated at entry cells; (c) existing conservation + fill-cap tests still pass (the Task-5-strengthened fixture: pit fills exactly to water level, conservation 1%); (d) determinism.
- [ ] **Step 2: RED** (assertions (a)/(b) fail against entry-cell deposition).
- [ ] **Step 3: Implement.**
- [ ] **Step 4: GREEN + full suites.**
- [ ] **Step 5: Commit** `feat(mapgen): lake deposition pours bottom-up per component`.

### Task 4: Lake stamp threshold

**Files:** Modify `src/mapgen/generator.cpp`, `src/mapgen/generator_tests.cpp`.

- [ ] **Step 1: Update tests** — lake-consistency test and the sim≠output seam test: `Lake ⇔ water_depth ≥ kLakeStampMinDepthM`; add boundary case check via a direct assertion that no Lake texel has `0 < W < 0.3`.
- [ ] **Step 2: Implement** — constexpr `kLakeStampMinDepthM = 0.3f` (generator.cpp); stamping condition `>= kLakeStampMinDepthM`. Expose the constant in `generator.hpp` for the tests (pattern of `kPlainsFrac`).
- [ ] **Step 3: GREEN + full suites.**
- [ ] **Step 4: Commit** `feat(mapgen): Lake stamps only at >= 0.3 m standing water`.

### Task 5: Integration — previews + iterations A/B

- [ ] **Step 1:** `ctest --test-dir build` full sweep green.
- [ ] **Step 2:** Preview sweep seeds 1-3 (default 80×dt1) into scratchpad `v11_out_s{1,2,3}`; PLUS seed 1 with `iterations=40, dt=2.0` (needs a temporary param override — mapview has no flag; write a 10-line scratchpad program linking badlands_mapgen_lib pattern from the -O2 bench, or accept a temporary local edit rebuilt-and-reverted, whichever is less invasive — report which) into `v11_out_s1_it40`.
- [ ] **Step 3:** Report evidence paths for user judging (biome + water_depth + final hillshade + flow per dir; A/B pair for the iterations question). NO default retuning without the user.
- [ ] **Step 4:** Commit anything pending; report timings.

## Self-review notes

- Task 1 RED requires `sediment_noise_m = 0` override in the test — the param exists, no scaffolding needed.
- Task 3 cascade rule (descending deepest-member pop-order) is specified to keep pour deterministic when lakes chain into lakes; single-lake maps unaffected.
- Stage sequence changes in Task 2 touch the Recorder test — brief says exactly where.
