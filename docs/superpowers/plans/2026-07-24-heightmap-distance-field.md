# High-Level Heightmap (distance-to-plains) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

## Context

The merged biome generator (`src/mapgen/generator.{hpp,cpp}`) fills `MapArtifacts::heightmap` with zeros. This feature produces the first real relief per the approved spec `docs/superpowers/specs/2026-07-24-heightmap-distance-field-design.md`: for every texel, the exact Euclidean distance to the nearest Plains texel, mapped linearly to height. Plains sit at exactly 0 m (the water datum); ridge crests emerge along mountain-belt medial axes. This is the landmass skeleton that later detail/erosion/water passes refine.

**Goal:** `heightmap = kSlopeMPerM · EDT(biome == Plains)`, distance in WORLD METERS.

**Architecture:** A new `distance_to_plains(biome, texel_m)` function in the generator (Felzenszwalb–Huttenlocher exact EDT, two separable 1D passes, double precision internally, parallel per line via the existing `parallel_tiles`). `generate_map` calls it after `classify_biomes`. No public interface changes beyond the new function; mapview renders the relief with zero viewer work. `hillshade.{hpp,cpp}` moves back from patchgen into `src/mapgen` so `--preview-image-only` can dump `hillshade.png` — the by-eye judging tool for the slope.

**Tech stack:** C++20, CMake/Ninja, Catch2. No new dependencies.

## Global Constraints

- **Units are world-metric**: `kSlopeMPerM` is height meters per meter of horizontal WORLD distance; the EDT scales each axis by that axis's texel size in meters. A texel-unit implementation is the bug class the units-guard test pins (user-mandated correction).
- The `MapData` contract (`src/game/map/map_data.hpp`) is FROZEN — untouched.
- Tunables are fixed `constexpr` (`kSlopeMPerM = 0.15f` starting value) — no knobs/config.
- EDT oracle tests use power-of-two texel values (1.0, 2.0, 0.5) so all double arithmetic is exact and `REQUIRE(a == b)` on floats is legitimate, not flaky.
- `parallel_tiles` lives in `src/mapgen/parallel.hpp`; `Field2D` in `src/mapgen/field2d.hpp` (reuse, don't reinvent).
- Run everything from the repo root; build `cmake --build build`; tests `ctest --test-dir build` (31 targets throughout — the new tests join the existing `badlands_generator_tests`).
- Commits end with `Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>`.
- NOTE: the working tree carries an UNCOMMITTED edit to the spec's Method section (the world-units clarification) — Task 1 commits it; do not discard it.

---

### Task 1: Branch, finish the spec's units fix, save the plan

**Files:**
- Modify: `docs/superpowers/specs/2026-07-24-heightmap-distance-field-design.md` (Testing section)
- Create: `docs/superpowers/plans/2026-07-24-heightmap-distance-field.md` (copy of this plan)

- [ ] **Step 1: Branch** — `git checkout -b feat/heightmap-distance-field` (carries the uncommitted spec edit).

- [ ] **Step 2: Fix the spec's Testing section.** Replace this bullet:

```markdown
- **EDT oracle:** `distance_to_plains` vs a brute-force O(n²) scan on small
  synthetic grids (exact equality in squared-texel space; world-meter scaling
  checked separately).
```

with:

```markdown
- **EDT oracle:** `distance_to_plains` vs a brute-force O(n²) scan on small
  synthetic grids, compared in WORLD METERS, including a non-square-texel
  case — exact equality (power-of-two texel values make every double
  operation exact, so both sides reduce to the same float).
- **Units guard (world-metric slope):** the same world-space plains layout
  sampled at two resolutions yields distances that agree at coinciding world
  points within one coarse texel (boundary discretization); a texel-unit
  implementation would disagree by the resolution ratio and fail loudly.
```

- [ ] **Step 3: Copy this plan file** to `docs/superpowers/plans/2026-07-24-heightmap-distance-field.md`.

- [ ] **Step 4: Commit**

```bash
git add docs/superpowers/specs/2026-07-24-heightmap-distance-field-design.md docs/superpowers/plans/2026-07-24-heightmap-distance-field.md
git commit -m "docs(mapgen): world-metric units in heightmap spec + implementation plan"
```

---

### Task 2: `distance_to_plains` (TDD)

**Files:**
- Modify: `src/mapgen/generator.hpp` (declaration), `src/mapgen/generator.cpp` (dt1d + implementation), `src/mapgen/generator_tests.cpp` (4 new tests)

**Interfaces:**
- Consumes: `Field2D<T>`, `Biome::Plains`, `parallel_tiles`.
- Produces (Task 3 relies on): `Field2D<float> distance_to_plains(const Field2D<uint8_t>& biome, glm::vec2 texel_m)` — WORLD METERS; all-plains → zeros; NO plains → zeros (documented degenerate).

- [ ] **Step 1: Declaration in `generator.hpp`** (after `classify_biomes`):

```cpp
// Exact Euclidean distance (WORLD METERS) from each texel to the nearest
// texel classified Plains, with texel (x, y) at world (x*texel_m.x,
// y*texel_m.y). Felzenszwalb–Huttenlocher two-pass EDT — exact, not a
// chamfer approximation. A map with no plains at all returns all zeros
// (unreachable via generate_map: the quantile cutoffs guarantee a plains
// share). Exposed for unit testing (pattern of compute_cutoffs).
Field2D<float> distance_to_plains(const Field2D<uint8_t>& biome,
                                  glm::vec2 texel_m);
```

- [ ] **Step 2: Write the failing tests** — append to `generator_tests.cpp` (add `using badlands::mapgen::distance_to_plains;` to the using-list and `#include <cmath>`):

```cpp
namespace {
// Brute-force oracle: the definition — min over all plains texels of the
// world-space Euclidean distance, double precision, O(n^2).
Field2D<float> brute_distance(const Field2D<uint8_t>& biome, glm::vec2 texel) {
  Field2D<float> out(biome.width, biome.height, 0.0f);
  for (int y = 0; y < biome.height; ++y) {
    for (int x = 0; x < biome.width; ++x) {
      double best = 1e30;
      for (int py = 0; py < biome.height; ++py) {
        for (int px = 0; px < biome.width; ++px) {
          if (biome.at(px, py) != static_cast<uint8_t>(Biome::Plains)) continue;
          const double dx = (x - px) * static_cast<double>(texel.x);
          const double dy = (y - py) * static_cast<double>(texel.y);
          best = std::min(best, dx * dx + dy * dy);
        }
      }
      out.at(x, y) = best < 1e30 ? static_cast<float>(std::sqrt(best)) : 0.0f;
    }
  }
  return out;
}
}  // namespace

TEST_CASE(
    "distance_to_plains: matches the brute-force oracle (incl. anisotropic "
    "texels)") {
  // Deterministic scattered plains pattern on a 17x11 grid.
  Field2D<uint8_t> biome(17, 11, static_cast<uint8_t>(Biome::Hills));
  for (int y = 0; y < 11; ++y)
    for (int x = 0; x < 17; ++x)
      if ((x * 7 + y * 13) % 9 == 0)
        biome.at(x, y) = static_cast<uint8_t>(Biome::Plains);
  for (glm::vec2 texel : {glm::vec2(1.0f, 1.0f), glm::vec2(2.0f, 0.5f)}) {
    const auto edt = distance_to_plains(biome, texel);
    const auto ref = brute_distance(biome, texel);
    // Power-of-two texels: every double op is exact, so exact float equality.
    REQUIRE(edt.data == ref.data);
  }
}

TEST_CASE("distance_to_plains: single plains texel gives the radial cone") {
  Field2D<uint8_t> biome(7, 7, static_cast<uint8_t>(Biome::Mountain));
  biome.at(3, 3) = static_cast<uint8_t>(Biome::Plains);
  const auto d = distance_to_plains(biome, {2.0f, 2.0f});
  for (int y = 0; y < 7; ++y) {
    for (int x = 0; x < 7; ++x) {
      const double dx = 2.0 * (x - 3), dy = 2.0 * (y - 3);
      REQUIRE(d.at(x, y) == static_cast<float>(std::sqrt(dx * dx + dy * dy)));
    }
  }
}

TEST_CASE("distance_to_plains: world-metric across resolutions (units guard)") {
  // Same world layout — plains where world_x < 128 m — sampled at 4 m and
  // 2 m texels. Distances at coinciding world points agree within one COARSE
  // texel (plains-boundary discretization); a texel-unit implementation would
  // be off by 2x and fail loudly.
  auto make = [](int w, int h, float texel) {
    Field2D<uint8_t> b(w, h, static_cast<uint8_t>(Biome::Hills));
    for (int y = 0; y < h; ++y)
      for (int x = 0; x < w; ++x)
        if (static_cast<float>(x) * texel < 128.0f)
          b.at(x, y) = static_cast<uint8_t>(Biome::Plains);
    return b;
  };
  const auto lo = distance_to_plains(make(64, 16, 4.0f), {4.0f, 4.0f});
  const auto hi = distance_to_plains(make(128, 32, 2.0f), {2.0f, 2.0f});
  for (int x = 40; x < 64; ++x) {  // well inside the non-plains half
    REQUIRE(lo.at(x, 8) == Catch::Approx(hi.at(2 * x, 16)).margin(4.0));
  }
}

TEST_CASE("distance_to_plains: no plains at all -> all zeros") {
  Field2D<uint8_t> biome(5, 4, static_cast<uint8_t>(Biome::Mountain));
  const auto d = distance_to_plains(biome, {1.0f, 1.0f});
  REQUIRE(d.data == std::vector<float>(20, 0.0f));
}
```

- [ ] **Step 3: Stub in `generator.cpp`** (returns `Field2D<float>(biome.width, biome.height, 0.0f)`), build, run:

```bash
cmake --build build --target badlands_generator_tests && ./build/badlands_generator_tests
```
Expected: oracle/cone/units-guard FAIL (all-zero output); no-plains test passes.

- [ ] **Step 4: Real implementation in `generator.cpp`** (anonymous namespace for `dt1d`; add `#include <cmath>` if absent):

```cpp
// One 1D pass of the Felzenszwalb–Huttenlocher squared-distance transform:
// given f[i] = best squared WORLD distance already achieved at sample i
// (kBigD = "no seed"), writes d[i] = min_j(f[j] + (step*(i-j))^2) via the
// parabola lower envelope. Double precision so the exact-vs-brute-force
// test guarantee holds at map-scale magnitudes. kBigD is a large FINITE
// value, not infinity: two "empty" parabolas must intersect at a finite
// point or the envelope math produces NaN.
constexpr double kBigD = 1e30;

void dt1d(const std::vector<double>& f, std::vector<double>& d,
          std::vector<int>& v, std::vector<double>& z, int n, double step) {
  const double s2 = step * step;
  int k = 0;
  v[0] = 0;
  z[0] = -kBigD;
  z[1] = kBigD;
  for (int q = 1; q < n; ++q) {
    const double fq = f[q] + s2 * q * q;
    for (;;) {
      const int p = v[k];
      const double s =
          (fq - (f[p] + s2 * p * p)) / (2.0 * s2 * static_cast<double>(q - p));
      if (k > 0 && s <= z[k]) {
        --k;
        continue;
      }
      ++k;
      v[k] = q;
      z[k] = s;
      z[k + 1] = kBigD;
      break;
    }
  }
  k = 0;
  for (int q = 0; q < n; ++q) {
    while (z[k + 1] < static_cast<double>(q)) ++k;
    const int p = v[k];
    const double dq = step * static_cast<double>(q - p);
    d[q] = f[p] + dq * dq;
  }
}
```

```cpp
Field2D<float> distance_to_plains(const Field2D<uint8_t>& biome,
                                  glm::vec2 texel_m) {
  const int w = biome.width, h = biome.height;
  Field2D<float> out(w, h, 0.0f);
  if (w <= 0 || h <= 0) return out;

  const auto kPlains = static_cast<uint8_t>(Biome::Plains);
  bool any_plains = false;
  for (uint8_t b : biome.data) {
    if (b == kPlains) {
      any_plains = true;
      break;
    }
  }
  if (!any_plains) return out;  // documented degenerate: all zeros

  // Squared world distances between the two passes.
  std::vector<double> g(static_cast<size_t>(w) * h);

  struct Scratch {
    std::vector<double> f, d, z;
    std::vector<int> v;
  };
  const int n_max = std::max(w, h);
  auto make_scratch = [n_max] {
    Scratch s;
    s.f.resize(static_cast<size_t>(n_max));
    s.d.resize(static_cast<size_t>(n_max));
    s.z.resize(static_cast<size_t>(n_max) + 1);
    s.v.resize(static_cast<size_t>(n_max));
    return s;
  };

  // Pass 1: per COLUMN over y (step = texel_m.y). Columns are independent;
  // parallel_tiles with height 1 hands out x-ranges.
  parallel_tiles(w, 1, 64, make_scratch,
                 [&](Scratch& s, int x0, int, int x1, int) {
                   for (int x = x0; x < x1; ++x) {
                     for (int y = 0; y < h; ++y)
                       s.f[y] = biome.at(x, y) == kPlains ? 0.0 : kBigD;
                     dt1d(s.f, s.d, s.v, s.z, h, texel_m.y);
                     for (int y = 0; y < h; ++y)
                       g[static_cast<size_t>(y) * w + x] = s.d[y];
                   }
                 });

  // Pass 2: per ROW over x (step = texel_m.x) on pass 1's result; sqrt out.
  parallel_tiles(h, 1, 64, make_scratch,
                 [&](Scratch& s, int y0, int, int y1, int) {
                   for (int y = y0; y < y1; ++y) {
                     for (int x = 0; x < w; ++x)
                       s.f[x] = g[static_cast<size_t>(y) * w + x];
                     dt1d(s.f, s.d, s.v, s.z, w, texel_m.x);
                     for (int x = 0; x < w; ++x)
                       out.at(x, y) = static_cast<float>(std::sqrt(s.d[x]));
                   }
                 });

  return out;
}
```

(Note the `k > 0 &&` guard in `dt1d`'s deletion branch — with k == 0 the candidate must replace/extend, never underflow the stack.)

- [ ] **Step 5: Run tests → PASS**, then full build:

```bash
cmake --build build --target badlands_generator_tests && ./build/badlands_generator_tests
cmake --build build
```
Expected: 9 test cases, all green (5 existing + 4 new); tree green.

- [ ] **Step 6: Commit**

```bash
git add src/mapgen/generator.hpp src/mapgen/generator.cpp src/mapgen/generator_tests.cpp
git commit -m "feat(mapgen): exact distance-to-plains transform (F-H EDT, world meters)"
```

---

### Task 3: Wire the height pass into `generate_map`

**Files:**
- Modify: `src/mapgen/generator.cpp` (constant + call), `src/mapgen/generator_tests.cpp` (determinism edit + 1 new test)

**Interfaces:**
- Consumes: `distance_to_plains` (Task 2).
- Produces: `MapArtifacts.heightmap` filled (plains exactly 0 m, else `kSlopeMPerM · d`).

- [ ] **Step 1: Update the determinism test first** (it currently pins heightmap to zeros and will otherwise fail the moment heights appear). In `generator_tests.cpp` replace:

```cpp
  REQUIRE(a.heightmap.data == std::vector<float>(64 * 64, 0.0f));
```
with:
```cpp
  REQUIRE(a.heightmap.data == b.heightmap.data);
```

And append the new test:

```cpp
TEST_CASE("generate_map: plains sit at exactly 0 m, everything else above") {
  MapGenParams p;
  p.seed = 2;
  p.resolution = {96, 96};
  p.size_m = {384.0f, 384.0f};
  const auto a = generate_map(p);
  for (size_t i = 0; i < a.biome.data.size(); ++i) {
    if (a.biome.data[i] == static_cast<uint8_t>(Biome::Plains)) {
      REQUIRE(a.heightmap.data[i] == 0.0f);
    } else {
      REQUIRE(a.heightmap.data[i] > 0.0f);
    }
  }
}
```

- [ ] **Step 2: Run → the new test FAILS** (heightmap still zeros): `cmake --build build --target badlands_generator_tests && ./build/badlands_generator_tests`

- [ ] **Step 3: Implement.** In `generator.cpp`, add to the constants block:

```cpp
// High-level relief slope: height meters per meter of horizontal WORLD
// distance to the nearest plains (NOT per texel — regenerating at another
// resolution must not change slopes; the units-guard test pins this).
// Plains sit at the 0 m water datum; the farthest texel is the highest.
constexpr float kSlopeMPerM = 0.15f;
```

and in `generate_map`, after the `classify_biomes` line:

```cpp
  // First-pass relief: a cone field over the distance to the nearest plains.
  // Ridge crests emerge along the mountain belts' medial axes; detail,
  // erosion and water come later (see the heightmap spec).
  const Field2D<float> dist = distance_to_plains(a.biome, texel);
  for (size_t i = 0; i < dist.data.size(); ++i)
    a.heightmap.data[i] = kSlopeMPerM * dist.data[i];
```

- [ ] **Step 4: Run → all 10 cases PASS**; full `cmake --build build && ctest --test-dir build` (31/31).

- [ ] **Step 5: Commit**

```bash
git add src/mapgen/generator.cpp src/mapgen/generator_tests.cpp
git commit -m "feat(mapgen): high-level heightmap from distance-to-plains cone field"
```

---

### Task 4: Hillshade preview + slope tuning by eye

**Files:**
- Move: `src/executables/patchgen/hillshade.{hpp,cpp}` → `src/mapgen/`
- Modify: `src/mapgen/outputs.{hpp,cpp}`, `src/executables/mapview/main_mapview.cpp`, `src/executables/patchgen/main_patchgen.cpp`, `CMakeLists.txt`
- Possibly modify: `kSlopeMPerM` in `src/mapgen/generator.cpp` (tuning)

- [ ] **Step 1: Move hillshade back into mapgen** (it's generic heightmap relief shading; the biome-gen phase parked it in patchgen as the then-only consumer — the preview dump is now a second consumer):

```bash
git mv src/executables/patchgen/hillshade.hpp src/executables/patchgen/hillshade.cpp src/mapgen/
```
Fix includes: `src/mapgen/hillshade.cpp` and `src/executables/patchgen/main_patchgen.cpp` change `"executables/patchgen/hillshade.hpp"` → `"mapgen/hillshade.hpp"`.

- [ ] **Step 2: CMake** — `badlands_mapgen_lib` sources gain `src/mapgen/hillshade.cpp`; `badlands_patchgen` sources drop `src/executables/patchgen/hillshade.cpp` (it links `badlands_mapgen_lib`, which now carries it). Update the mapgen-lib comment ("+ hillshade relief preview").

- [ ] **Step 3: Preview wiring.** `outputs.hpp`: signature becomes

```cpp
// Dumps the debug rasters for one generated map into out_dir: bedrock.png
// (normalized gray), biome.png (palette), heightmap.png, and hillshade.png
// (relief-shaded heights — grayscale heightmaps are nearly unreadable for
// judging ridge structure by eye). `texel_m` is the horizontal sample
// spacing feeding the hillshade's slope computation.
void write_preview_images(const std::string& out_dir, const MapArtifacts& a,
                          float texel_m);
```

`outputs.cpp`: add `#include "mapgen/hillshade.hpp"` and append to the body:

```cpp
  write_hillshade_png(a.heightmap, out_dir + "/hillshade.png", texel_m);
```

`main_mapview.cpp` `RunPreviewOnly`: pass the texel size (square texels are CLI-validated before this runs):

```cpp
  badlands::mapgen::write_preview_images(
      out_dir, artifacts,
      params.size_m.x / static_cast<float>(params.resolution.x));
```

- [ ] **Step 4: Build + full tests** — `cmake --build build && ctest --test-dir build` (31/31; patchgen still builds and its smoke path is covered by `badlands_patch_eval_tests`).

- [ ] **Step 5: Tune the slope by eye.** For seeds 1, 2, 3, 9:

```bash
./build/badlands_mapview --preview-image-only --seed $S --out /private/tmp/claude-501/-Users-jakub-repos-badlands/eaeb0836-0812-470c-827a-3c7c4fe31d0f/scratchpad/hm_s$S
```
View `hillshade.png` + `biome.png` (Read tool). Acceptance: plains read dead-flat; ridge crests read as connected lines along the mountain belts (medial axes); flanks neither cliff-like nor imperceptible (peaks roughly 30–60 m on a 512 m map — check `heightmap.png`'s implied range or log the max). Adjust ONLY `kSlopeMPerM` (sensible range 0.10–0.25) and re-check; note the final value in the commit message. Then one bounded interactive sanity run: `perl -e 'alarm 15; exec @ARGV' ./build/badlands_mapview --seed 2` (relief renders, no crash; SIGALRM exit expected).

- [ ] **Step 6: Commit**

```bash
git add -A
git commit -m "feat(mapview): hillshade preview; tune relief slope (kSlopeMPerM=<final>)"
```

---

### Task 5: Docs + end-to-end verification

**Files:**
- Modify: `CLAUDE.md` (mapview paragraph: preview dump list gains hillshade)

- [ ] **Step 1:** In `CLAUDE.md`, update "(bedrock/biome/heightmap PNGs)" → "(bedrock/biome/heightmap/hillshade PNGs)".

- [ ] **Step 2: Verification battery**

```bash
cmake --build build && ctest --test-dir build          # 31/31
./build/badlands_mapview --preview-image-only --seed 2 --out <scratch>/final && ls <scratch>/final   # 4 PNGs incl. hillshade.png
./build/badlands_mapview --resolution 256x256 --size 512x512 --preview-image-only --seed 2 --out <scratch>/final_coarse   # same map coarser; hillshade slopes comparable (units guard, by eye)
./build/badlands_game --screenshot <scratch>/game.png  # game app unaffected
```

- [ ] **Step 3: Commit**

```bash
git add CLAUDE.md
git commit -m "docs: mapview preview dump includes hillshade"
```

---

## Verification (end-to-end recap)

1. `ctest --test-dir build` — generator suite grows 5 → 10 cases: EDT oracle (exact, incl. anisotropic texels), radial cone, units guard (world-metric), no-plains degenerate, plains-at-0m, determinism now covering real heights.
2. Preview at seeds 1/2/3/9: `hillshade.png` shows flat plains + connected ridge crests along mountain belts; `heightmap.png` dark in plains, bright at belt interiors.
3. Coarse re-render of the same seed/size: slopes/peaks visually match (world-metric units).
4. Interactive mapview: 3D relief renders, hover/raycast works on the new heights.
5. Patchgen unaffected (links mapgen lib for the moved hillshade; `badlands_patch_eval_tests` green).
