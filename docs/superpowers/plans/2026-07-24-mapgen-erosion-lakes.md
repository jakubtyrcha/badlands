# Mapgen Erosion + Lakes Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Resculpt the cone-field heightmap with a two-layer implicit stream-power erosion sim, flood pre-seeded bedrock-quantile cavities into per-lake water levels, add a runevision-style gully detail filter, and make every pipeline step dump a debug PNG.

**Architecture:** New pure-CPU modules under `src/mapgen/`: `hydrology` (priority-flood routing + drainage), `erosion` (cavities, sediment, incision, deposition, diffusion, sim loop, lake finalization), `detail_filter` (analytic gully octaves), `resample` (bilinear grid transfer). `generator.cpp` orchestrates on a padded sim grid decoupled from the output grid; `outputs.cpp` gets a PNG `MapDebugSink`. Spec: `docs/superpowers/specs/2026-07-24-mapgen-erosion-lakes-design.md`.

**Tech Stack:** C++20, FastNoiseLite (header-only), Catch2 (amalgamated), CMake/Ninja. No new dependencies.

## Global Constraints

- Run all commands from the repo root. Build: `cmake --build build`. Tests: `ctest --test-dir build -R <name>` or run the test binary directly.
- Everything deterministic: no `std::unordered_*` iteration order in results, no wall-clock, fixed tie-breaks (elevation, then linear index). `generate_map` twice → byte-identical.
- World-metric units everywhere: heights/distances in meters, drainage areas in m². Noise sampled at world coordinates (`x * texel_m + origin_m`).
- CPU-only; sim passes are serial (correctness first), per-texel passes may use `parallel_tiles`. Budget: a few seconds at 512² sim.
- All knobs live in `ErosionParams` with the spec's defaults; sub-knobs are `constexpr` in the implementing `.cpp`/`.hpp`. No UI, no new CLI flags (except none — `--preview-image-only` gains dumps with zero new flags).
- Commit style (from git history): `feat(mapgen): …`, `test(mapgen): …`, `refactor(mapview): …`; end commit messages with the Claude Code co-author line.
- The engine/rendering interface must not change. Everything here is game-side mapgen.

---

### Task 1: Collapse MapGenParams to square/scalar

The spec fixes ONE resolution + ONE world scale per grid. `MapGenParams` loses its `glm::ivec2/vec2`; mapview call sites and tests follow. Purely mechanical, unblocks every later task.

**Files:**
- Modify: `src/mapgen/generator.hpp` (MapGenParams struct)
- Modify: `src/mapgen/generator.cpp` (generate_map body)
- Modify: `src/mapgen/generator_tests.cpp` (param setup in every test)
- Modify: `src/executables/mapview/main_mapview.cpp` (CLI parse, RunPreviewOnly)
- Modify: `src/executables/mapview/map_view_view.cpp` + `.hpp` (params_ uses)

**Interfaces:**
- Produces (all later tasks build on this):
```cpp
struct MapGenParams {
  uint32_t seed = 1;
  int resolution = 512;         // output grid (texels, square)
  float world_size_m = 512.0f;  // world extent (meters, square)
};
```

- [ ] **Step 1: Update the struct and generator**

In `generator.hpp` replace the `MapGenParams` body with the struct above (keep the doc comment; drop the `glm::ivec2/vec2` members). In `generator.cpp` `generate_map`:

```cpp
const int w = params.resolution, h = params.resolution;
...
const glm::vec2 texel(params.world_size_m / static_cast<float>(w),
                      params.world_size_m / static_cast<float>(h));
```

and the belt-noise wavelength becomes `params.world_size_m` (was `max(size_m.x, size_m.y)`).

- [ ] **Step 2: Update tests**

In `generator_tests.cpp` replace every `p.resolution = {N, N}; p.size_m = {M, M};` with `p.resolution = N; p.world_size_m = M;`. The degenerate-resolution test becomes:

```cpp
TEST_CASE("generate_map: degenerate resolution yields empty artifacts, no throw") {
  MapGenParams p;
  p.resolution = 0;
  REQUIRE(generate_map(p).bedrock.size() == 0);
  p.resolution = -1;
  const auto a = generate_map(p);
  REQUIRE(a.bedrock.size() == 0);
  REQUIRE(a.biome.size() == 0);
  REQUIRE(a.heightmap.size() == 0);
}
```

The cross-resolution test keeps its 64/128 pairing (`lo.resolution = 64; hi.resolution = 128;` at `world_size_m = 512`).

- [ ] **Step 3: Update mapview**

`main_mapview.cpp`: `--resolution WxH` and `--size WxH` keep their parsers but reject non-square (`w != h` → error "non-square maps are not supported"), then assign `params.resolution = r->first;` / `params.world_size_m = r->first;`. The existing square-texel check collapses to nothing (always square now) — delete it. `RunPreviewOnly` printf and `write_preview_images` texel arg use the scalars:

```cpp
badlands::mapgen::write_preview_images(
    out_dir, artifacts,
    params.world_size_m / static_cast<float>(params.resolution));
```

`map_view_view.cpp`: `params_.resolution.x/.y` → `params_.resolution`; `params_.size_m.x/.y` → `params_.world_size_m`; `MakeOneHotMapData(map_, params_.size_m)` → `MakeOneHotMapData(map_, glm::vec2(params_.world_size_m))` (the frozen MapData contract keeps its vec2).

- [ ] **Step 4: Build + run tests**

Run: `cmake --build build && ctest --test-dir build -R badlands_generator_tests`
Expected: PASS (all existing cases).

- [ ] **Step 5: Commit**

```bash
git add src/mapgen/generator.hpp src/mapgen/generator.cpp src/mapgen/generator_tests.cpp src/executables/mapview/
git commit -m "refactor(mapgen): square/scalar MapGenParams (one resolution, one world scale)"
```

---

### Task 2: Hydrology — priority-flood flow routing

**Files:**
- Create: `src/mapgen/hydrology.hpp`, `src/mapgen/hydrology.cpp`
- Create: `src/mapgen/hydrology_tests.cpp`
- Modify: `CMakeLists.txt` (add `hydrology.cpp` to `badlands_mapgen_lib`; new test target `badlands_erosion_tests` — pattern of `badlands_generator_tests`)

**Interfaces:**
- Produces:
```cpp
// hydrology.hpp
namespace badlands::mapgen {
// D8 receiver graph from priority-flood (Barnes 2014) with an epsilon
// gradient across flats and flooded depressions. Border cells are base
// level: receiver -1, all flow exits through them. Deterministic: priority
// ties break on linear index.
struct FlowRouting {
  int width = 0, height = 0;
  std::vector<int32_t> receiver;    // linear idx of downhill receiver; -1 = border/base level
  std::vector<int32_t> order;       // pop order; a cell's receiver pops before it (topological)
  std::vector<uint8_t> in_lake;     // 1 = flooded above ground (depression interior)
  std::vector<float> water_level;   // flood level for in_lake cells, else ground height
};
FlowRouting route_flow(const Field2D<float>& h, float texel_m, float epsilon_m);
Field2D<float> accumulate_drainage(const FlowRouting& r, float texel_area_m2);
}
```

- [ ] **Step 1: Write failing tests** (`hydrology_tests.cpp`)

```cpp
#include <catch_amalgamated.hpp>
#include <cmath>
#include "mapgen/hydrology.hpp"

using namespace badlands::mapgen;

namespace {
Field2D<float> tilted_plane(int w, int h, float dz_per_col) {
  Field2D<float> f(w, h);
  for (int y = 0; y < h; ++y)
    for (int x = 0; x < w; ++x) f.at(x, y) = x * dz_per_col;
  return f;
}
}  // namespace

TEST_CASE("route_flow: tilted plane — receivers never uphill, border drains") {
  const auto h = tilted_plane(16, 8, 1.0f);
  const auto r = route_flow(h, 1.0f, 1e-4f);
  REQUIRE(r.order.size() == h.size());
  for (int y = 0; y < 8; ++y)
    for (int x = 0; x < 16; ++x) {
      const int i = y * 16 + x;
      if (x == 0 || y == 0 || x == 15 || y == 7) continue;  // border: seeds
      REQUIRE(r.receiver[i] >= 0);
      REQUIRE(h.data[r.receiver[i]] <= h.data[i]);  // never uphill
      REQUIRE(r.in_lake[i] == 0);
    }
}

TEST_CASE("route_flow: flat plate — epsilon drains everything to the border") {
  Field2D<float> h(12, 12, 5.0f);
  const auto r = route_flow(h, 1.0f, 1e-4f);
  for (int y = 1; y < 11; ++y)
    for (int x = 1; x < 11; ++x) {
      // walk receivers; must reach a border cell (receiver -1) without cycling
      int i = y * 12 + x, steps = 0;
      while (r.receiver[i] >= 0 && steps++ < 12 * 12) i = r.receiver[i];
      REQUIRE(r.receiver[i] == -1);
    }
}

TEST_CASE("route_flow: walled bowl floods to its notch level, uniform per lake") {
  // CAREFUL with synthetic terrains here: the border is the drain (base
  // level), so the bowl's wall must be INTERIOR and the ground outside it
  // must slope freely to the border — a bowl formed by raising the border
  // itself would flood the entire map instead.
  // 11x11: ground 0, a ring wall at 10 (Chebyshev radius 2 around center),
  // bowl floor 2 inside, one notch at 5 in the wall.
  Field2D<float> h(11, 11, 0.0f);
  for (int y = 3; y <= 7; ++y)
    for (int x = 3; x <= 7; ++x)
      if (x == 3 || x == 7 || y == 3 || y == 7) h.at(x, y) = 10.0f;  // wall
      else h.at(x, y) = 2.0f;                                        // floor
  h.at(5, 3) = 5.0f;  // notch: the spill
  const auto r = route_flow(h, 1.0f, 1e-4f);
  for (int y = 4; y <= 6; ++y)
    for (int x = 4; x <= 6; ++x) {
      const int i = y * 11 + x;
      REQUIRE(r.in_lake[i] == 1);
      // flooded to the 5 m notch (+ a few epsilon steps at most)
      REQUIRE(r.water_level[i] == Catch::Approx(5.0f).margin(0.01));
    }
}

TEST_CASE("accumulate_drainage: tilted plane column sums") {
  // flow runs -x (downhill toward x=0); interior cell at x collects the cells
  // to its right in its row (no lateral convergence on a clean tilt)
  const auto h = tilted_plane(16, 8, 1.0f);
  const auto r = route_flow(h, 1.0f, 1e-4f);
  const auto a = accumulate_drainage(r, 4.0f);  // 2 m texels
  double total_at_border = 0.0;
  for (int y = 0; y < 8; ++y) total_at_border += a.at(0, y);
  // every cell's rain (16*8 cells * 4 m²) exits through the x=0 border column
  // plus the border columns' own rain routed along other borders; at minimum
  // the map's interior rain reaches x=0
  REQUIRE(total_at_border >= 14 * 6 * 4.0);
  // and drainage is non-decreasing downstream on one interior row
  for (int x = 14; x > 1; --x) REQUIRE(a.at(x - 1, 4) >= a.at(x, 4));
}

TEST_CASE("route_flow + accumulate_drainage: deterministic") {
  const auto h = tilted_plane(16, 8, 0.0f);  // all-flat: worst case for ties
  const auto r1 = route_flow(h, 1.0f, 1e-4f);
  const auto r2 = route_flow(h, 1.0f, 1e-4f);
  REQUIRE(r1.receiver == r2.receiver);
  REQUIRE(r1.order == r2.order);
  REQUIRE(accumulate_drainage(r1, 1.0f).data == accumulate_drainage(r2, 1.0f).data);
}
```

- [ ] **Step 2: Add the CMake test target and verify the tests fail to build**

`CMakeLists.txt`: append `src/mapgen/hydrology.cpp` to `badlands_mapgen_lib` sources, and after `badlands_generator_tests` add:

```cmake
# badlands_erosion_tests: hydrology + erosion sim + detail filter — structural
# invariants on synthetic terrains (routing, spill levels, solver bounds,
# layer bookkeeping). Pure CPU, pattern of badlands_generator_tests.
add_executable(badlands_erosion_tests
    src/mapgen/hydrology_tests.cpp
    src/mapgen/hydrology.cpp
    third_party/catch2/extras/catch_amalgamated.cpp
)
target_include_directories(badlands_erosion_tests PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/third_party/glm
    ${CMAKE_SOURCE_DIR}/third_party/FastNoiseLite
    ${CMAKE_SOURCE_DIR}/third_party/catch2/extras
)
add_test(NAME badlands_erosion_tests COMMAND badlands_erosion_tests
         WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
```

Run: `cmake --build build 2>&1 | tail -5`
Expected: FAIL — `mapgen/hydrology.hpp` not found.

- [ ] **Step 3: Implement** (`hydrology.hpp` as in Interfaces; `hydrology.cpp`)

```cpp
#include "mapgen/hydrology.hpp"
#include <algorithm>
#include <queue>
#include <utility>

namespace badlands::mapgen {

FlowRouting route_flow(const Field2D<float>& h, float /*texel_m*/, float epsilon_m) {
  const int w = h.width, ht = h.height;
  FlowRouting r;
  r.width = w; r.height = ht;
  if (w <= 0 || ht <= 0) return r;
  const size_t n = h.size();
  r.receiver.assign(n, -1);
  r.in_lake.assign(n, 0);
  r.water_level.assign(n, 0.0f);
  r.order.reserve(n);
  std::vector<uint8_t> visited(n, 0);

  // min-heap on (level, linear index) — the index tie-break is the
  // determinism guarantee on flats
  using Item = std::pair<float, int>;
  std::priority_queue<Item, std::vector<Item>, std::greater<Item>> pq;
  auto seed = [&](int x, int y) {
    const int i = y * w + x;
    if (visited[i]) return;
    visited[i] = 1;
    r.water_level[i] = h.data[i];
    pq.push({h.data[i], i});
  };
  for (int x = 0; x < w; ++x) { seed(x, 0); seed(x, ht - 1); }
  for (int y = 0; y < ht; ++y) { seed(0, y); seed(w - 1, y); }

  static constexpr int dx8[8] = {1, -1, 0, 0, 1, 1, -1, -1};
  static constexpr int dy8[8] = {0, 0, 1, -1, 1, -1, 1, -1};
  while (!pq.empty()) {
    const auto [level, i] = pq.top();
    pq.pop();
    r.order.push_back(i);
    const int cx = i % w, cy = i / w;
    for (int k = 0; k < 8; ++k) {
      const int nx = cx + dx8[k], ny = cy + dy8[k];
      if (nx < 0 || ny < 0 || nx >= w || ny >= ht) continue;
      const int j = ny * w + nx;
      if (visited[j]) continue;
      visited[j] = 1;
      r.receiver[j] = i;
      const float wl = std::max(h.data[j], level + epsilon_m);
      r.water_level[j] = wl;
      r.in_lake[j] = wl > h.data[j] ? 1 : 0;
      pq.push({wl, j});
    }
  }
  return r;
}

Field2D<float> accumulate_drainage(const FlowRouting& r, float texel_area_m2) {
  Field2D<float> a(r.width, r.height, texel_area_m2);
  for (size_t k = r.order.size(); k-- > 0;) {
    const int i = r.order[k];
    const int32_t rcv = r.receiver[i];
    if (rcv >= 0) a.data[rcv] += a.data[i];
  }
  return a;
}

}  // namespace badlands::mapgen
```

Note the heap invariant: a neighbor is pushed with `wl >= level + epsilon_m > level`, so every cell pops after its receiver — `order` is topological by construction.

- [ ] **Step 4: Run tests**

Run: `cmake --build build && ./build/badlands_erosion_tests`
Expected: PASS (all 6 cases).

- [ ] **Step 5: Commit**

```bash
git add src/mapgen/hydrology.hpp src/mapgen/hydrology.cpp src/mapgen/hydrology_tests.cpp CMakeLists.txt
git commit -m "feat(mapgen): priority-flood flow routing + drainage accumulation"
```

---

### Task 3: Erosion init — ErosionParams, cavity carve, sediment init

**Files:**
- Create: `src/mapgen/erosion.hpp`, `src/mapgen/erosion.cpp`
- Create: `src/mapgen/erosion_tests.cpp`
- Modify: `CMakeLists.txt` (add `erosion.cpp` to `badlands_mapgen_lib` and to `badlands_erosion_tests`; add `erosion_tests.cpp` to the test target)

**Interfaces:**
- Consumes: `Field2D`, quantile pattern of `compute_cutoffs` (nth_element on a copy).
- Produces:
```cpp
// erosion.hpp
namespace badlands::mapgen {
struct ErosionParams {
  int sim_resolution = 512;   // sim grid texels (square, excl. pad)
  int iterations = 80;
  float dt = 1.0f;            // nominal time unit
  float m = 0.5f;             // stream-power area exponent (slope exponent n fixed at 1)
  float k_sediment = 5e-3f;
  float k_bedrock = 5e-4f;
  float deposition_g = 1.0f;
  float diffusion = 0.02f;    // D (m²/dt)
  float initial_sediment_m = 4.0f;
  float sediment_taper_m = 60.0f;
  float sediment_noise_m = 1.0f;
  float sediment_noise_wavelength_m = 40.0f;
  float lake_frac = 0.03f;
  float lake_depth_m = 12.0f;
  float min_lake_area_m2 = 400.0f;
  float min_lake_depth_m = 0.5f;
  int dump_every = 10;        // loop dump cadence (0 = off)
  int detail_octaves = 4;
  float detail_wavelength_m = 60.0f;
  float detail_amplitude_m = 2.0f;
};

inline constexpr int kPadTexels = 16;      // sim-grid margin, cropped on output
inline constexpr float kEpsilonM = 1e-4f;  // flood epsilon per step

// Subtract smooth cavity bowls where sim-grid bedrock is in its bottom
// lake_frac quantile. Depth grows quadratically from the quantile rim to
// lake_depth_m at the bedrock minimum. Returns the basin mask (1 = carved).
Field2D<uint8_t> carve_cavities(Field2D<float>& B, const Field2D<float>& bedrock,
                                float lake_frac, float lake_depth_m);

// S0 = initial_sediment_m * clamp(1 - d/taper, 0, 1) + fBm noise, clamped >= 0;
// zero inside basins. Noise is sampled at world meters (x * texel_m + origin_m)
// with seed+3 (seeds 0..2 are taken by base/ridged/belt).
Field2D<float> init_sediment(const Field2D<float>& dist_to_plains,
                             const Field2D<uint8_t>& basin_mask,
                             const ErosionParams& p, float texel_m,
                             float origin_m, uint32_t seed);
}
```

- [ ] **Step 1: Write failing tests** (`erosion_tests.cpp`)

```cpp
#include <catch_amalgamated.hpp>
#include <cmath>
#include "mapgen/erosion.hpp"

using namespace badlands::mapgen;

TEST_CASE("carve_cavities: coverage ~= lake_frac, depth bounded, only lowers") {
  // bedrock ramp 0..1 over 100x100 -> bottom 5% is a crisp quantile
  Field2D<float> bedrock(100, 100);
  for (int y = 0; y < 100; ++y)
    for (int x = 0; x < 100; ++x)
      bedrock.at(x, y) = (y * 100 + x) / 9999.0f;
  Field2D<float> B(100, 100, 50.0f);
  const auto B_before = B.data;
  const auto mask = carve_cavities(B, bedrock, 0.05f, 12.0f);
  double carved = 0.0;
  float max_cut = 0.0f;
  for (size_t i = 0; i < mask.data.size(); ++i) {
    if (mask.data[i]) carved += 1.0;
    const float cut = B_before[i] - B.data[i];
    REQUIRE(cut >= 0.0f);            // carving only lowers
    REQUIRE(cut <= 12.0f + 1e-4f);   // bounded by lake_depth_m
    if (!mask.data[i]) REQUIRE(cut == 0.0f);
    max_cut = std::max(max_cut, cut);
  }
  REQUIRE(carved / mask.data.size() == Catch::Approx(0.05).margin(0.005));
  REQUIRE(max_cut == Catch::Approx(12.0f).margin(0.5));  // minimum gets full depth
}

TEST_CASE("init_sediment: tapers off plains, zero in basins, never negative") {
  Field2D<float> dist(64, 64);
  for (int y = 0; y < 64; ++y)
    for (int x = 0; x < 64; ++x) dist.at(x, y) = static_cast<float>(x);  // 0..63 m
  Field2D<uint8_t> basins(64, 64, 0);
  basins.at(2, 2) = 1;
  ErosionParams p;  // taper 60 m, initial 4 m, noise 1 m
  const auto s = init_sediment(dist, basins, p, 1.0f, 0.0f, 7);
  REQUIRE(s.at(2, 2) == 0.0f);                       // basin floor
  REQUIRE(s.at(1, 30) >= 4.0f - 1.0f - 1e-4f);       // near plains: full blanket ± noise
  REQUIRE(s.at(63, 30) <= 1.0f + 1e-4f);             // past taper: noise only
  for (float v : s.data) REQUIRE(v >= 0.0f);
  // deterministic
  REQUIRE(init_sediment(dist, basins, p, 1.0f, 0.0f, 7).data == s.data);
}
```

- [ ] **Step 2: Run to verify failure**

Run: `cmake --build build 2>&1 | tail -5`
Expected: FAIL — `mapgen/erosion.hpp` not found.

- [ ] **Step 3: Implement** (`erosion.cpp`)

```cpp
#include "mapgen/erosion.hpp"
#include <algorithm>
#include <cmath>
#include <vector>
#include <FastNoiseLite.h>

namespace badlands::mapgen {

Field2D<uint8_t> carve_cavities(Field2D<float>& B, const Field2D<float>& bedrock,
                                float lake_frac, float lake_depth_m) {
  Field2D<uint8_t> mask(bedrock.width, bedrock.height, 0);
  const size_t n = bedrock.size();
  if (n == 0 || lake_frac <= 0.0f) return mask;
  std::vector<float> v = bedrock.data;
  const size_t i_lake = static_cast<size_t>(lake_frac * (n - 1));
  std::nth_element(v.begin(), v.begin() + i_lake, v.end());
  const float t_lake = v[i_lake];
  const float b_min = *std::min_element(bedrock.data.begin(), bedrock.data.end());
  const float span = std::max(t_lake - b_min, 1e-6f);
  for (size_t i = 0; i < n; ++i) {
    const float b = bedrock.data[i];
    if (b >= t_lake) continue;
    mask.data[i] = 1;
    const float u = (t_lake - b) / span;  // 0 at rim, 1 at the minimum
    B.data[i] -= lake_depth_m * u * u;    // smooth bowl: flat rim, deep center
  }
  return mask;
}

Field2D<float> init_sediment(const Field2D<float>& dist_to_plains,
                             const Field2D<uint8_t>& basin_mask,
                             const ErosionParams& p, float texel_m,
                             float origin_m, uint32_t seed) {
  Field2D<float> s(dist_to_plains.width, dist_to_plains.height, 0.0f);
  FastNoiseLite noise(static_cast<int>(seed + 3u));
  noise.SetNoiseType(FastNoiseLite::NoiseType_Perlin);
  noise.SetFractalType(FastNoiseLite::FractalType_FBm);
  noise.SetFractalOctaves(3);
  noise.SetFrequency(1.0f / p.sediment_noise_wavelength_m);
  for (int y = 0; y < s.height; ++y) {
    for (int x = 0; x < s.width; ++x) {
      if (basin_mask.at(x, y)) continue;  // cavities start sediment-free
      const float taper =
          std::clamp(1.0f - dist_to_plains.at(x, y) / p.sediment_taper_m, 0.0f, 1.0f);
      const float wx = static_cast<float>(x) * texel_m + origin_m;
      const float wy = static_cast<float>(y) * texel_m + origin_m;
      const float nse = p.sediment_noise_m * noise.GetNoise(wx, wy);  // ~[-a, a]
      s.at(x, y) = std::max(0.0f, p.initial_sediment_m * taper + nse);
    }
  }
  return s;
}

}  // namespace badlands::mapgen
```

`erosion.hpp` needs `#include <cstdint>`, `<vector>`, `"mapgen/field2d.hpp"`.

- [ ] **Step 4: Run tests**

Run: `cmake --build build && ./build/badlands_erosion_tests`
Expected: PASS.

- [ ] **Step 5: Commit**

```bash
git add src/mapgen/erosion.hpp src/mapgen/erosion.cpp src/mapgen/erosion_tests.cpp CMakeLists.txt
git commit -m "feat(mapgen): ErosionParams + lake cavity carve + sediment init"
```

---

### Task 4: Implicit stream-power incision (two-layer)

**Files:**
- Modify: `src/mapgen/erosion.hpp`, `src/mapgen/erosion.cpp`
- Modify: `src/mapgen/erosion_tests.cpp`

**Interfaces:**
- Consumes: `FlowRouting` (`receiver/order/in_lake/water_level`), `accumulate_drainage` output.
- Produces:
```cpp
// One implicit Braun–Willett pass in routing order over ground h = B + S.
// K per cell: k_sediment while S > 0 else k_bedrock; eroded depth consumes S
// first, the excess rescaled by k_bedrock/k_sediment before cutting B.
// in_lake cells are skipped (no floor incision). Receiver height is the
// EFFECTIVE level max(B+S, water_level) so cells erode toward the water
// surface, not a lake floor. Returns eroded thickness (m) per cell.
Field2D<float> incise(Field2D<float>& B, Field2D<float>& S,
                      const FlowRouting& r, const Field2D<float>& area,
                      const ErosionParams& p, float texel_m);
```

- [ ] **Step 1: Write failing tests** (append to `erosion_tests.cpp`; add `#include "mapgen/hydrology.hpp"`)

```cpp
namespace {
// 1D ramp world: one row high, ground rises +1 m per column from the border.
struct Ramp1D {
  Field2D<float> B, S;
  FlowRouting r;
  Field2D<float> area;
};
Ramp1D make_ramp(int w, float sediment) {
  Ramp1D t{Field2D<float>(w, 3), Field2D<float>(w, 3, sediment), {}, {}};
  for (int y = 0; y < 3; ++y)
    for (int x = 0; x < w; ++x) t.B.at(x, y) = static_cast<float>(x);
  Field2D<float> h(w, 3);
  for (size_t i = 0; i < h.data.size(); ++i) h.data[i] = t.B.data[i] + t.S.data[i];
  t.r = route_flow(h, 1.0f, 1e-4f);
  t.area = accumulate_drainage(t.r, 1.0f);
  return t;
}
}  // namespace

TEST_CASE("incise: matches an explicit-Euler reference on a hand-built chain") {
  // Hand-built 1D chain (NOT route_flow — 2D routing would pick diagonal
  // receivers and muddy the geometry): cell i's receiver is i-1, cell 0 is
  // base level. B ramp 0..n-1, dry, A[i] = upstream cell count.
  const int n = 32;
  FlowRouting r;
  r.width = n; r.height = 1;
  r.receiver.assign(n, -1);
  r.in_lake.assign(n, 0);
  r.water_level.assign(n, 0.0f);
  for (int i = 1; i < n; ++i) r.receiver[i] = i - 1;
  for (int i = 0; i < n; ++i) r.order.push_back(i);
  Field2D<float> B(n, 1), S(n, 1, 0.0f);
  Field2D<float> area(n, 1);
  for (int i = 0; i < n; ++i) {
    B.at(i, 0) = static_cast<float>(i);
    r.water_level[i] = static_cast<float>(i);  // dry: level = ground
    area.at(i, 0) = static_cast<float>(n - i);
  }
  ErosionParams p;
  p.k_bedrock = 1e-3f;  // F <= 1e-3 * sqrt(32) ~ 5.7e-3 << 1
  p.dt = 1.0f;

  // independent reference: explicit Euler, 1000 sub-steps on the same graph
  std::vector<double> href(n);
  for (int i = 0; i < n; ++i) href[i] = B.at(i, 0);
  const int M = 1000;
  for (int step = 0; step < M; ++step) {
    std::vector<double> next = href;
    for (int i = 1; i < n; ++i) {
      const double slope = href[i] - href[i - 1];  // d = 1
      next[i] -= p.k_bedrock * std::pow(area.at(i, 0), p.m) * slope * (p.dt / M);
    }
    href = next;
  }
  const auto eroded = incise(B, S, r, area, p, 1.0f);
  for (int i = 1; i < n; ++i) {
    REQUIRE(eroded.at(i, 0) > 0.0f);
    REQUIRE(B.at(i, 0) == Catch::Approx(href[i]).epsilon(0.02));
  }
  REQUIRE(B.at(0, 0) == 0.0f);  // base level pinned
}

TEST_CASE("incise: never erodes a cell below its receiver") {
  auto t = make_ramp(32, 1.0f);
  ErosionParams p;
  p.k_sediment = 10.0f;  // absurdly strong: the clamp must still hold
  p.k_bedrock = 1.0f;
  incise(t.B, t.S, t.r, t.area, p, 1.0f);
  for (int y = 0; y < 3; ++y)
    for (int x = 0; x < 32; ++x) {
      const int i = y * 32 + x;
      const int32_t rcv = t.r.receiver[i];
      if (rcv < 0) continue;
      const float hi = t.B.data[i] + t.S.data[i];
      const float hr = t.B.data[rcv] + t.S.data[rcv];
      REQUIRE(hi >= hr - 1e-4f);
    }
}

TEST_CASE("incise: sediment strips before bedrock; border cells never erode") {
  auto t = make_ramp(32, 0.5f);
  ErosionParams p;
  p.k_sediment = 5e-2f;
  p.k_bedrock = 0.0f;  // bedrock immune -> only sediment may move
  const auto B_before = t.B.data;
  incise(t.B, t.S, t.r, t.area, p, 1.0f);
  REQUIRE(t.B.data == B_before);                    // bedrock untouched
  REQUIRE(t.S.at(2, 1) < 0.5f);                     // sediment eroded
  // border cells are base level (receiver -1): untouched entirely
  REQUIRE(t.S.at(0, 0) == 0.5f);
}
```

- [ ] **Step 2: Run to verify failure**

Run: `cmake --build build 2>&1 | tail -5`
Expected: FAIL — `incise` not declared.

- [ ] **Step 3: Implement** (append to `erosion.cpp`; include `"mapgen/hydrology.hpp"` in `erosion.hpp` for `FlowRouting`)

```cpp
Field2D<float> incise(Field2D<float>& B, Field2D<float>& S,
                      const FlowRouting& r, const Field2D<float>& area,
                      const ErosionParams& p, float texel_m) {
  Field2D<float> eroded(r.width, r.height, 0.0f);
  const float diag = texel_m * std::sqrt(2.0f);
  for (const int i : r.order) {
    const int32_t rcv = r.receiver[i];
    if (rcv < 0 || r.in_lake[i]) continue;  // base level / lake floor: skip
    const float h_old = B.data[i] + S.data[i];
    // effective receiver level: erode toward the water surface over lakes
    const float z_rcv = std::max(B.data[rcv] + S.data[rcv], r.water_level[rcv]);
    if (h_old <= z_rcv) continue;
    const int dx = std::abs(i % r.width - rcv % r.width);
    const int dy = std::abs(i / r.width - rcv / r.width);
    const float d = (dx + dy == 2) ? diag : texel_m;
    const float K = S.data[i] > 0.0f ? p.k_sediment : p.k_bedrock;
    if (K <= 0.0f) continue;
    const float F = K * std::pow(area.data[i], p.m) * p.dt / d;
    const float h_new = (h_old + F * z_rcv) / (1.0f + F);
    float delta = h_old - h_new;
    if (delta <= 0.0f) continue;
    if (delta <= S.data[i]) {
      S.data[i] -= delta;
    } else {
      // layer transition: the sediment fraction goes at k_sediment's rate,
      // the remainder is rescaled to bedrock's slower rate
      const float into_rock = (delta - S.data[i]) * (p.k_bedrock / p.k_sediment);
      delta = S.data[i] + into_rock;
      S.data[i] = 0.0f;
      B.data[i] -= into_rock;
    }
    eroded.data[i] = delta;
  }
  return eroded;
}
```

- [ ] **Step 4: Run tests**

Run: `cmake --build build && ./build/badlands_erosion_tests`
Expected: PASS.

- [ ] **Step 5: Commit**

```bash
git add src/mapgen/erosion.hpp src/mapgen/erosion.cpp src/mapgen/erosion_tests.cpp
git commit -m "feat(mapgen): two-layer implicit stream-power incision"
```

---

### Task 5: Deposition (G-term downstream flux pass)

**Files:**
- Modify: `src/mapgen/erosion.hpp`, `src/mapgen/erosion.cpp`, `src/mapgen/erosion_tests.cpp`

**Interfaces:**
- Produces:
```cpp
// Route this pass's eroded volume downstream (reverse routing order = donors
// before receivers): each cell receives flux q_in from its donors, deposits
// dep = min(q_in/texel_area, G * q_in / A)   — dry cells
// dep = min(q_in/texel_area, water_level - h) — flooded cells (delta fill)
// into S, then forwards the remainder plus its own erosion. Flux reaching
// base-level cells (receiver -1) leaves the map. Returns exported volume m³.
float deposit(Field2D<float>& B, Field2D<float>& S,
              const Field2D<float>& eroded_m, const FlowRouting& r,
              const Field2D<float>& area, const ErosionParams& p,
              float texel_area_m2);
```
(Note: one explicit pass per sim iteration; the outer loop provides the
relaxation the spec's Gauss–Seidel sweeps describe.)

- [ ] **Step 1: Write failing tests**

```cpp
TEST_CASE("deposit: lake cells fill at most to water level; conservation holds") {
  // 1D ramp with a flooded pocket: reuse the bowl idea in a row
  Field2D<float> B(16, 3);
  for (int y = 0; y < 3; ++y)
    for (int x = 0; x < 16; ++x) B.at(x, y) = static_cast<float>(x);
  B.at(4, 1) = 1.0f;  // pit below its neighbors (floods to ~5 via the rim at x=5)
  Field2D<float> S(16, 3, 0.0f);
  Field2D<float> h(16, 3);
  for (size_t i = 0; i < h.data.size(); ++i) h.data[i] = B.data[i] + S.data[i];
  const auto r = route_flow(h, 1.0f, 1e-4f);
  const auto area = accumulate_drainage(r, 1.0f);
  REQUIRE(r.in_lake[1 * 16 + 4] == 1);  // sanity: the pit is flooded

  Field2D<float> eroded(16, 3, 0.1f);  // uniform artificial erosion this pass
  ErosionParams p;
  p.deposition_g = 1.0f;
  const float S_before = 0.0f;
  const float exported = deposit(B, S, eroded, r, area, p, 1.0f);
  const int pit = 1 * 16 + 4;
  REQUIRE(S.data[pit] >= S_before);  // pit received sediment
  REQUIRE(B.data[pit] + S.data[pit] <= r.water_level[pit] + 1e-4f);  // never above water
  // conservation: total eroded volume = total deposited + exported
  double dep_total = 0.0, ero_total = 0.0;
  for (size_t i = 0; i < S.data.size(); ++i) dep_total += S.data[i];
  for (float e : eroded.data) ero_total += e;
  REQUIRE(dep_total + exported == Catch::Approx(ero_total).epsilon(0.01));
}

TEST_CASE("deposit: G=0 exports everything") {
  auto t = make_ramp(16, 0.0f);
  Field2D<float> eroded(16, 3, 0.2f);
  ErosionParams p;
  p.deposition_g = 0.0f;
  const auto S_before = t.S.data;
  const float exported = deposit(t.B, t.S, eroded, t.r, t.area, p, 1.0f);
  REQUIRE(t.S.data == S_before);  // nothing deposited anywhere (no flooded cells)
  REQUIRE(exported == Catch::Approx(16 * 3 * 0.2).epsilon(0.01));
}
```

- [ ] **Step 2: Run to verify failure** — build fails: `deposit` not declared.

- [ ] **Step 3: Implement**

```cpp
float deposit(Field2D<float>& B, Field2D<float>& S,
              const Field2D<float>& eroded_m, const FlowRouting& r,
              const Field2D<float>& area, const ErosionParams& p,
              float texel_area_m2) {
  std::vector<double> q_in(eroded_m.size(), 0.0);  // m³ arriving from donors
  double exported = 0.0;
  for (size_t k = r.order.size(); k-- > 0;) {  // donors before receivers
    const int i = r.order[k];
    double dep_depth = 0.0;
    if (q_in[i] > 0.0) {
      const double avail_depth = q_in[i] / texel_area_m2;
      if (r.in_lake[i]) {
        const double headroom = r.water_level[i] - (B.data[i] + S.data[i]);
        dep_depth = std::clamp(avail_depth, 0.0, std::max(0.0, headroom));
      } else {
        dep_depth = std::min(avail_depth,
                             static_cast<double>(p.deposition_g) * q_in[i] /
                                 std::max(area.data[i], texel_area_m2));
      }
      S.data[i] += static_cast<float>(dep_depth);
    }
    const double q_out =
        q_in[i] - dep_depth * texel_area_m2 + eroded_m.data[i] * texel_area_m2;
    const int32_t rcv = r.receiver[i];
    if (rcv >= 0) q_in[rcv] += q_out;
    else exported += q_out;
  }
  return static_cast<float>(exported);
}
```

- [ ] **Step 4: Run tests** — `./build/badlands_erosion_tests` → PASS.

- [ ] **Step 5: Commit**

```bash
git add src/mapgen/erosion.hpp src/mapgen/erosion.cpp src/mapgen/erosion_tests.cpp
git commit -m "feat(mapgen): G-term deposition with lake delta filling"
```

---

### Task 6: Hillslope diffusion

**Files:**
- Modify: `src/mapgen/erosion.hpp`, `src/mapgen/erosion.cpp`, `src/mapgen/erosion_tests.cpp`

**Interfaces:**
- Produces:
```cpp
// Explicit 5-point diffusion of h = B + S on interior cells (border pinned),
// sub-stepped so D*dt_sub/texel² <= 0.25. Negative dh draws S before B;
// positive dh credits S (diffused material is loose).
void diffuse(Field2D<float>& B, Field2D<float>& S, const ErosionParams& p,
             float texel_m);
```

- [ ] **Step 1: Write failing tests**

```cpp
TEST_CASE("diffuse: smooths a spike, conserves interior mass, respects layers") {
  Field2D<float> B(9, 9, 10.0f);
  Field2D<float> S(9, 9, 0.0f);
  S.at(4, 4) = 8.0f;  // sediment spike
  ErosionParams p;
  p.diffusion = 2.0f;  // deliberately > stability bound at dt=1 -> must sub-step
  p.dt = 1.0f;
  double mass_before = 0.0;
  for (size_t i = 0; i < S.data.size(); ++i) mass_before += B.data[i] + S.data[i];
  diffuse(B, S, p, 1.0f);
  REQUIRE(S.at(4, 4) < 8.0f);          // spike lowered
  REQUIRE(S.at(3, 4) > 0.0f);          // neighbors received sediment
  for (float v : S.data) REQUIRE(v >= 0.0f);
  REQUIRE(std::isfinite(S.at(4, 4)));  // sub-stepping kept it stable
  double mass_after = 0.0;
  for (size_t i = 0; i < S.data.size(); ++i) mass_after += B.data[i] + S.data[i];
  // border is pinned, and the spike's spread stays interior on one step
  REQUIRE(mass_after == Catch::Approx(mass_before).epsilon(1e-4));
  // bedrock at the spike was never touched (only its sediment moved)
  REQUIRE(B.at(4, 4) == 10.0f);
}
```

- [ ] **Step 2: Run to verify failure** — build fails: `diffuse` not declared.

- [ ] **Step 3: Implement**

```cpp
void diffuse(Field2D<float>& B, Field2D<float>& S, const ErosionParams& p,
             float texel_m) {
  if (p.diffusion <= 0.0f) return;
  const int w = B.width, ht = B.height;
  const float tex2 = texel_m * texel_m;
  const int n_sub = std::max(
      1, static_cast<int>(std::ceil(p.diffusion * p.dt / (0.24f * tex2))));
  const float dt_sub = p.dt / static_cast<float>(n_sub);
  Field2D<float> h(w, ht);
  for (int step = 0; step < n_sub; ++step) {
    for (size_t i = 0; i < h.data.size(); ++i) h.data[i] = B.data[i] + S.data[i];
    for (int y = 1; y < ht - 1; ++y) {
      for (int x = 1; x < w - 1; ++x) {
        const float lap = h.at(x + 1, y) + h.at(x - 1, y) + h.at(x, y + 1) +
                          h.at(x, y - 1) - 4.0f * h.at(x, y);
        const float dh = p.diffusion * dt_sub * lap / tex2;
        if (dh >= 0.0f) {
          S.at(x, y) += dh;
        } else {
          const float from_s = std::min(S.at(x, y), -dh);
          S.at(x, y) -= from_s;
          B.at(x, y) -= (-dh - from_s);
        }
      }
    }
  }
}
```

Note: `h` is rebuilt per sub-step (Jacobi, not Gauss–Seidel) — that plus the
0.24 bound is what makes it stable and symmetric.

- [ ] **Step 4: Run tests** — PASS.

- [ ] **Step 5: Commit**

```bash
git add src/mapgen/erosion.hpp src/mapgen/erosion.cpp src/mapgen/erosion_tests.cpp
git commit -m "feat(mapgen): sub-stepped hillslope diffusion"
```

---

### Task 7: Sim loop + lake finalization + MapDebugSink

**Files:**
- Modify: `src/mapgen/erosion.hpp`, `src/mapgen/erosion.cpp`, `src/mapgen/erosion_tests.cpp`

**Interfaces:**
- Produces:
```cpp
// erosion.hpp
struct MapDebugSink {
  virtual ~MapDebugSink() = default;
  // stages: "loop-height", "loop-flow", "loop-sediment" (float);
  //         "loop-lakes" (uint8). Later tasks add init/output stages.
  virtual void dump(std::string_view stage, int sequence,
                    const Field2D<float>& field) = 0;
  virtual void dump(std::string_view stage, int sequence,
                    const Field2D<uint8_t>& mask) = 0;
};

struct ErosionOutputs {
  Field2D<float> water_depth;  // m of standing water after pruning
  Field2D<float> flow;         // final drainage area (m²)
};

// The full sim: iterations × (route → drain → incise → deposit → diffuse),
// then a final route to flood lakes, measure spill levels, and prune lakes
// under min area/depth. Mutates B and S. sink may be null.
ErosionOutputs erode(Field2D<float>& B, Field2D<float>& S,
                     const ErosionParams& p, float texel_m,
                     MapDebugSink* sink);
```

- [ ] **Step 1: Write failing tests**

```cpp
namespace {
// Hill world: 33x33 cone sloping DOWN to the border (the drain), with a deep
// pocket punched into the summit. The pocket floods to its own rim and spills
// downhill — a genuine cavity lake. (A raised rim at the border would instead
// flood the whole map: the border is base level.)
struct BowlWorld {
  Field2D<float> B, S;
};
BowlWorld make_bowl() {
  BowlWorld t{Field2D<float>(33, 33), Field2D<float>(33, 33, 0.0f)};
  for (int y = 0; y < 33; ++y)
    for (int x = 0; x < 33; ++x) {
      const float dx = x - 16.0f, dy = y - 16.0f;
      const float rad = std::sqrt(dx * dx + dy * dy);
      t.B.at(x, y) = 30.0f - rad;               // hill: high center, low border
      if (rad < 4.0f) t.B.at(x, y) = -10.0f;    // deep summit pocket
    }
  return t;
}
}  // namespace

TEST_CASE("erode: cavity floods, per-lake level uniform, W >= 0") {
  auto t = make_bowl();
  ErosionParams p;
  p.iterations = 10;
  p.dump_every = 0;
  p.min_lake_area_m2 = 4.0f;
  p.min_lake_depth_m = 0.1f;
  const auto out = erode(t.B, t.S, p, 1.0f, nullptr);
  float level_min = 1e30f, level_max = -1e30f;
  int wet = 0;
  for (int y = 0; y < 33; ++y)
    for (int x = 0; x < 33; ++x) {
      const float w = out.water_depth.at(x, y);
      REQUIRE(w >= 0.0f);
      if (w > 0.0f) {
        ++wet;
        const float lvl = t.B.at(x, y) + t.S.at(x, y) + w;
        level_min = std::min(level_min, lvl);
        level_max = std::max(level_max, lvl);
      }
    }
  REQUIRE(wet > 10);                                  // the cavity holds a lake
  REQUIRE(level_max - level_min < 0.05f);             // one flat surface
}

TEST_CASE("erode: pruning removes puddles") {
  auto t = make_bowl();
  ErosionParams p;
  p.iterations = 10;
  p.dump_every = 0;
  p.min_lake_area_m2 = 1e6f;  // nothing can qualify
  const auto out = erode(t.B, t.S, p, 1.0f, nullptr);
  for (float w : out.water_depth.data) REQUIRE(w == 0.0f);
}

TEST_CASE("erode: deterministic, and the sink sees the loop film strip") {
  struct CountingSink : MapDebugSink {
    int floats = 0, masks = 0;
    void dump(std::string_view, int, const Field2D<float>&) override { ++floats; }
    void dump(std::string_view, int, const Field2D<uint8_t>&) override { ++masks; }
  };
  auto t1 = make_bowl();
  auto t2 = make_bowl();
  ErosionParams p;
  p.iterations = 4;
  p.dump_every = 2;  // dumps after iterations 2 and 4
  CountingSink sink;
  const auto o1 = erode(t1.B, t1.S, p, 1.0f, &sink);
  const auto o2 = erode(t2.B, t2.S, p, 1.0f, nullptr);
  REQUIRE(o1.water_depth.data == o2.water_depth.data);
  REQUIRE(o1.flow.data == o2.flow.data);
  REQUIRE(t1.B.data == t2.B.data);
  REQUIRE(t1.S.data == t2.S.data);
  REQUIRE(sink.floats == 2 * 3);  // height+flow+sediment × 2 dumps
  REQUIRE(sink.masks == 2 * 1);   // lakes mask × 2 dumps
}
```

- [ ] **Step 2: Run to verify failure** — build fails: `erode` not declared.

- [ ] **Step 3: Implement**

```cpp
namespace {

// Flood the CURRENT surface and turn flooded cells into water depths, then
// prune lakes (4-connected components) below the area/depth thresholds.
Field2D<float> finalize_lakes(const Field2D<float>& B, const Field2D<float>& S,
                              const FlowRouting& r, const ErosionParams& p,
                              float texel_m) {
  const int w = r.width, ht = r.height;
  Field2D<float> depth(w, ht, 0.0f);
  for (int i = 0; i < w * ht; ++i)
    if (r.in_lake[i])
      depth.data[i] = r.water_level[i] - (B.data[i] + S.data[i]);
  // component label + prune
  const float texel_area = texel_m * texel_m;
  std::vector<uint8_t> seen(depth.size(), 0);
  std::vector<int> stack, member;
  for (int start = 0; start < w * ht; ++start) {
    if (seen[start] || depth.data[start] <= 0.0f) continue;
    stack.assign(1, start);
    member.clear();
    seen[start] = 1;
    float max_depth = 0.0f;
    while (!stack.empty()) {
      const int i = stack.back();
      stack.pop_back();
      member.push_back(i);
      max_depth = std::max(max_depth, depth.data[i]);
      const int x = i % w, y = i / w;
      const int nb[4] = {i - 1, i + 1, i - w, i + w};
      const bool ok[4] = {x > 0, x < w - 1, y > 0, y < ht - 1};
      for (int k = 0; k < 4; ++k)
        if (ok[k] && !seen[nb[k]] && depth.data[nb[k]] > 0.0f) {
          seen[nb[k]] = 1;
          stack.push_back(nb[k]);
        }
    }
    const float area = static_cast<float>(member.size()) * texel_area;
    if (area < p.min_lake_area_m2 || max_depth < p.min_lake_depth_m)
      for (const int i : member) depth.data[i] = 0.0f;
  }
  return depth;
}

}  // namespace

ErosionOutputs erode(Field2D<float>& B, Field2D<float>& S,
                     const ErosionParams& p, float texel_m,
                     MapDebugSink* sink) {
  const float texel_area = texel_m * texel_m;
  Field2D<float> h(B.width, B.height);
  auto ground = [&] {
    for (size_t i = 0; i < h.data.size(); ++i) h.data[i] = B.data[i] + S.data[i];
  };
  FlowRouting r;
  Field2D<float> area;
  for (int it = 1; it <= p.iterations; ++it) {
    ground();
    r = route_flow(h, texel_m, kEpsilonM);
    area = accumulate_drainage(r, texel_area);
    const auto eroded = incise(B, S, r, area, p, texel_m);
    deposit(B, S, eroded, r, area, p, texel_area);
    diffuse(B, S, p, texel_m);
    if (sink && p.dump_every > 0 && it % p.dump_every == 0) {
      ground();
      sink->dump("loop-height", it, h);
      sink->dump("loop-flow", it, area);
      sink->dump("loop-sediment", it, S);
      Field2D<uint8_t> lakes(r.width, r.height, 0);
      for (size_t i = 0; i < lakes.data.size(); ++i) lakes.data[i] = r.in_lake[i];
      sink->dump("loop-lakes", it, lakes);
    }
  }
  ground();
  r = route_flow(h, texel_m, kEpsilonM);
  ErosionOutputs out;
  out.flow = accumulate_drainage(r, texel_area);
  out.water_depth = finalize_lakes(B, S, r, p, texel_m);
  return out;
}
```

`erosion.hpp` gains `#include <string_view>`.

- [ ] **Step 4: Run tests** — PASS.

- [ ] **Step 5: Commit**

```bash
git add src/mapgen/erosion.hpp src/mapgen/erosion.cpp src/mapgen/erosion_tests.cpp
git commit -m "feat(mapgen): erosion sim loop + lake finalization + MapDebugSink"
```

---

### Task 8: Generalize the EDT + bilinear resample helper

Two small utilities the wiring task needs: `distance_to_plains` generalized to any mask (the detail filter needs distance-to-water), and world-aligned bilinear resampling (sim grid → output grid).

**Files:**
- Modify: `src/mapgen/generator.hpp`, `src/mapgen/generator.cpp` (EDT refactor)
- Create: `src/mapgen/resample.hpp` (header-only)
- Modify: `src/mapgen/generator_tests.cpp` (mask EDT case), `src/mapgen/erosion_tests.cpp` (resample cases)
- Modify: `CMakeLists.txt` (add `generator.cpp` to `badlands_erosion_tests` — it now needs the EDT; also add `FastNoiseLite` include dir if missing)

**Interfaces:**
- Produces:
```cpp
// generator.hpp — distance_to_plains becomes a wrapper over:
// Exact EDT (world meters) to the nearest nonzero mask texel. All-zero mask
// -> all zeros.
Field2D<float> distance_to_mask(const Field2D<uint8_t>& mask, glm::vec2 texel_m);

// resample.hpp
// Sample src (texel spacing src_texel_m, texel (0,0) at world src_origin_m)
// at the output grid's world positions (texel x at x * dst_texel_m), bilinear,
// clamped at src edges.
Field2D<float> resample_bilinear(const Field2D<float>& src, float src_texel_m,
                                 float src_origin_m, int dst_res,
                                 float dst_texel_m);
```

- [ ] **Step 1: Write failing tests**

Append to `generator_tests.cpp`:

```cpp
TEST_CASE("distance_to_mask: matches distance_to_plains on a plains mask") {
  Field2D<uint8_t> biome(17, 11, static_cast<uint8_t>(Biome::Hills));
  Field2D<uint8_t> mask(17, 11, 0);
  for (int y = 0; y < 11; ++y)
    for (int x = 0; x < 17; ++x)
      if ((x * 7 + y * 13) % 9 == 0) {
        biome.at(x, y) = static_cast<uint8_t>(Biome::Plains);
        mask.at(x, y) = 1;
      }
  REQUIRE(badlands::mapgen::distance_to_mask(mask, {1.0f, 1.0f}).data ==
          distance_to_plains(biome, {1.0f, 1.0f}).data);
}
```

Append to `erosion_tests.cpp` (add `#include "mapgen/resample.hpp"`):

```cpp
TEST_CASE("resample_bilinear: identity when grids coincide, linear in between") {
  Field2D<float> src(8, 8);
  for (int y = 0; y < 8; ++y)
    for (int x = 0; x < 8; ++x) src.at(x, y) = static_cast<float>(x);  // ramp
  // same grid: identity
  const auto same = resample_bilinear(src, 1.0f, 0.0f, 8, 1.0f);
  REQUIRE(same.data == src.data);
  // 2x finer: midpoints are averages of the ramp -> still linear in world x
  const auto fine = resample_bilinear(src, 1.0f, 0.0f, 16, 0.5f);
  REQUIRE(fine.at(3, 4) == Catch::Approx(1.5f));  // world x = 1.5
  // origin shift: src texel 0 sits at world -2 -> world 0 is src coord 2
  const auto shifted = resample_bilinear(src, 1.0f, -2.0f, 8, 1.0f);
  REQUIRE(shifted.at(0, 0) == Catch::Approx(2.0f));
}
```

- [ ] **Step 2: Run to verify failure** — build fails on both.

- [ ] **Step 3: Implement**

`generator.cpp`: rename the body of `distance_to_plains` to `distance_to_mask(const Field2D<uint8_t>& mask, glm::vec2 texel_m)` — replace `biome.at(x, y) == kPlains` with `mask.at(x, y) != 0` (both places, including the any-seed early-out). Then:

```cpp
Field2D<float> distance_to_plains(const Field2D<uint8_t>& biome,
                                  glm::vec2 texel_m) {
  Field2D<uint8_t> mask(biome.width, biome.height, 0);
  const auto kPlains = static_cast<uint8_t>(Biome::Plains);
  for (size_t i = 0; i < mask.data.size(); ++i)
    mask.data[i] = biome.data[i] == kPlains ? 1 : 0;
  return distance_to_mask(mask, texel_m);
}
```

`resample.hpp`:

```cpp
#pragma once
#include <algorithm>
#include <cmath>
#include "mapgen/field2d.hpp"

namespace badlands::mapgen {

inline Field2D<float> resample_bilinear(const Field2D<float>& src,
                                        float src_texel_m, float src_origin_m,
                                        int dst_res, float dst_texel_m) {
  Field2D<float> out(dst_res, dst_res, 0.0f);
  if (src.width <= 0 || src.height <= 0 || dst_res <= 0) return out;
  for (int y = 0; y < dst_res; ++y) {
    for (int x = 0; x < dst_res; ++x) {
      const float sx = (static_cast<float>(x) * dst_texel_m - src_origin_m) / src_texel_m;
      const float sy = (static_cast<float>(y) * dst_texel_m - src_origin_m) / src_texel_m;
      const float cx = std::clamp(sx, 0.0f, static_cast<float>(src.width - 1));
      const float cy = std::clamp(sy, 0.0f, static_cast<float>(src.height - 1));
      const int x0 = static_cast<int>(cx), y0 = static_cast<int>(cy);
      const int x1 = std::min(x0 + 1, src.width - 1);
      const int y1 = std::min(y0 + 1, src.height - 1);
      const float fx = cx - x0, fy = cy - y0;
      const float a = src.at(x0, y0) * (1 - fx) + src.at(x1, y0) * fx;
      const float b = src.at(x0, y1) * (1 - fx) + src.at(x1, y1) * fx;
      out.at(x, y) = a * (1 - fy) + b * fy;
    }
  }
  return out;
}

}  // namespace badlands::mapgen
```

- [ ] **Step 4: Run tests**

Run: `cmake --build build && ./build/badlands_generator_tests && ./build/badlands_erosion_tests`
Expected: PASS (including all pre-existing EDT oracle tests — the refactor must not change results).

- [ ] **Step 5: Commit**

```bash
git add src/mapgen/generator.hpp src/mapgen/generator.cpp src/mapgen/resample.hpp src/mapgen/generator_tests.cpp src/mapgen/erosion_tests.cpp CMakeLists.txt
git commit -m "refactor(mapgen): generic mask EDT + world-aligned bilinear resample"
```

---

### Task 9: Gully detail filter

**Files:**
- Create: `src/mapgen/detail_filter.hpp`, `src/mapgen/detail_filter.cpp`
- Create: `src/mapgen/detail_filter_tests.cpp`
- Modify: `CMakeLists.txt` (add `detail_filter.cpp` to `badlands_mapgen_lib` and to `badlands_erosion_tests`; add the tests file)

**Interfaces:**
- Consumes: `ErosionParams` (detail_* fields), `distance_to_mask`, `parallel_tiles`.
- Produces:
```cpp
// detail_filter.hpp
// Analytic slope-aligned gully octaves (runevision-style, v1 core subset:
// pivot-cell oriented stripes, cos/sin renormalization, stacked ridge fading,
// ease-out slope mask). Returns the CARVE delta (<= 0) to add to `base`.
// Zero where water stands and within kShoreFadeDistM of it; zero on flats.
// Pure per-texel function of (world pos, seed, base gradient): deterministic.
Field2D<float> gully_detail_delta(const Field2D<float>& base,
                                  const Field2D<float>& water_depth,
                                  float texel_m, uint32_t seed,
                                  const ErosionParams& p);
```

- [ ] **Step 1: Write failing tests** (`detail_filter_tests.cpp`)

```cpp
#include <catch_amalgamated.hpp>
#include <cmath>
#include "mapgen/detail_filter.hpp"
#include "mapgen/erosion.hpp"

using namespace badlands::mapgen;

namespace {
Field2D<float> slope_field(int n, float dz) {  // plane rising dz per texel in x
  Field2D<float> f(n, n);
  for (int y = 0; y < n; ++y)
    for (int x = 0; x < n; ++x) f.at(x, y) = x * dz;
  return f;
}
}  // namespace

TEST_CASE("gully_detail_delta: bounded, carve-only, deterministic") {
  const auto base = slope_field(64, 1.0f);  // steep: 45 degrees
  const Field2D<float> dry(64, 64, 0.0f);
  ErosionParams p;
  const auto d1 = gully_detail_delta(base, dry, 1.0f, 9, p);
  const auto d2 = gully_detail_delta(base, dry, 1.0f, 9, p);
  REQUIRE(d1.data == d2.data);
  // amplitude bound: sum of octave amplitudes (a0 * (1 + 1/2 + 1/4 + 1/8))
  const float bound = p.detail_amplitude_m * 2.0f;
  bool any_nonzero = false;
  for (float v : d1.data) {
    REQUIRE(v <= 0.0f);
    REQUIRE(v >= -bound);
    if (v < 0.0f) any_nonzero = true;
  }
  REQUIRE(any_nonzero);  // steep slope MUST get gullies
}

TEST_CASE("gully_detail_delta: flat base gets nothing; water gets nothing") {
  const Field2D<float> flat(64, 64, 5.0f);
  Field2D<float> water(64, 64, 0.0f);
  ErosionParams p;
  const auto d = gully_detail_delta(flat, water, 1.0f, 9, p);
  for (float v : d.data) REQUIRE(v == 0.0f);  // slope mask kills flats

  const auto base = slope_field(64, 1.0f);
  for (int y = 20; y < 40; ++y)
    for (int x = 20; x < 40; ++x) water.at(x, y) = 2.0f;  // a lake patch
  const auto dw = gully_detail_delta(base, water, 1.0f, 9, p);
  for (int y = 20; y < 40; ++y)
    for (int x = 20; x < 40; ++x) REQUIRE(dw.at(x, y) == 0.0f);
}
```

- [ ] **Step 2: Run to verify failure** — build fails: header not found.

- [ ] **Step 3: Implement** (`detail_filter.cpp`)

```cpp
#include "mapgen/detail_filter.hpp"

#include <algorithm>
#include <cmath>
#include <variant>

#include <glm/glm.hpp>

#include "mapgen/generator.hpp"  // distance_to_mask
#include "mapgen/parallel.hpp"

namespace badlands::mapgen {

namespace {

constexpr float kSlopeRef = 0.6f;       // slope (m/m) mapping to full gully strength
constexpr float kShoreFadeDistM = 3.0f; // detail fades in over this distance from water
constexpr float kCellPerWavelength = 2.0f;
constexpr float kPersistence = 0.5f;
constexpr float kTwoPi = 6.28318530718f;

// Deterministic 2D integer hash -> [0,1) floats (PCG-style mix).
uint32_t hash_u32(uint32_t x) {
  x ^= x >> 16; x *= 0x7feb352dU; x ^= x >> 15; x *= 0x846ca68bU; x ^= x >> 16;
  return x;
}
glm::vec2 hash_pivot(uint32_t seed, int octave, int cx, int cy) {
  const uint32_t h1 = hash_u32(seed * 0x9e3779b9U + octave * 0x85ebca6bU +
                               static_cast<uint32_t>(cx) * 0xc2b2ae35U +
                               static_cast<uint32_t>(cy) * 0x27d4eb2fU);
  const uint32_t h2 = hash_u32(h1 + 0x165667b1U);
  return {static_cast<float>(h1 & 0xffffffU) / 16777216.0f,
          static_cast<float>(h2 & 0xffffffU) / 16777216.0f};
}

// Central-difference gradient of base at a (clamped) texel.
glm::vec2 grad_at(const Field2D<float>& f, int x, int y, float texel_m) {
  const int x0 = std::max(x - 1, 0), x1 = std::min(x + 1, f.width - 1);
  const int y0 = std::max(y - 1, 0), y1 = std::min(y + 1, f.height - 1);
  return {(f.at(x1, y) - f.at(x0, y)) / (texel_m * (x1 - x0)),
          (f.at(x, y1) - f.at(x, y0)) / (texel_m * (y1 - y0))};
}

}  // namespace

Field2D<float> gully_detail_delta(const Field2D<float>& base,
                                  const Field2D<float>& water_depth,
                                  float texel_m, uint32_t seed,
                                  const ErosionParams& p) {
  const int n = base.width;
  Field2D<float> delta(n, base.height, 0.0f);
  if (p.detail_octaves <= 0 || p.detail_amplitude_m <= 0.0f) return delta;

  // Distance to standing water, for the shore fade.
  Field2D<uint8_t> wet(n, base.height, 0);
  bool any_wet = false;
  for (size_t i = 0; i < wet.data.size(); ++i) {
    wet.data[i] = water_depth.data[i] > 0.0f ? 1 : 0;
    any_wet |= wet.data[i] != 0;
  }
  const Field2D<float> water_dist =
      any_wet ? distance_to_mask(wet, {texel_m, texel_m}) : Field2D<float>{};

  parallel_tiles(
      n, base.height, 64, [] { return std::monostate{}; },
      [&](std::monostate&, int tx0, int ty0, int tx1, int ty1) {
        for (int y = ty0; y < ty1; ++y) {
          for (int x = tx0; x < tx1; ++x) {
            if (wet.at(x, y)) continue;
            float shore = 1.0f;
            if (any_wet)
              shore = std::clamp(water_dist.at(x, y) / kShoreFadeDistM, 0.0f, 1.0f);
            if (shore <= 0.0f) continue;

            const glm::vec2 g = grad_at(base, x, y, texel_m);
            const float slope = glm::length(g);
            if (slope < 1e-5f) continue;
            const float s01 = std::clamp(slope / kSlopeRef, 0.0f, 1.0f);
            const float slope_mask = 1.0f - (1.0f - s01) * (1.0f - s01);  // ease-out
            if (slope_mask <= 0.0f) continue;
            // across-flow axis: gullies elongate downhill, vary across it
            const glm::vec2 across = glm::normalize(glm::vec2(-g.y, g.x));

            const float wx = static_cast<float>(x) * texel_m;
            const float wy = static_cast<float>(y) * texel_m;
            float carve = 0.0f, ridge_mask = 1.0f;
            float lambda = p.detail_wavelength_m;
            float amp = p.detail_amplitude_m;
            for (int o = 0; o < p.detail_octaves; ++o) {
              const float cell = lambda * kCellPerWavelength;
              const float cxf = wx / cell, cyf = wy / cell;
              const int cx0 = static_cast<int>(std::floor(cxf));
              const int cy0 = static_cast<int>(std::floor(cyf));
              const float fx = cxf - cx0, fy = cyf - cy0;
              // blend the 4 surrounding pivot cells' stripe phases as
              // cos/sin pairs (unit-circle interpolation, then renormalize)
              float bc = 0.0f, bs = 0.0f;
              for (int j = 0; j < 2; ++j) {
                for (int i2 = 0; i2 < 2; ++i2) {
                  const glm::vec2 jit = hash_pivot(seed, o, cx0 + i2, cy0 + j);
                  const glm::vec2 pivot((cx0 + i2 + jit.x) * cell,
                                        (cy0 + j + jit.y) * cell);
                  const float u =
                      glm::dot(glm::vec2(wx, wy) - pivot, across) / lambda;
                  const float wgt = (i2 ? fx : 1.0f - fx) * (j ? fy : 1.0f - fy);
                  bc += wgt * std::cos(kTwoPi * u);
                  bs += wgt * std::sin(kTwoPi * u);
                }
              }
              const float len = std::sqrt(bc * bc + bs * bs);
              const float norm = std::min(1.0f, 2.0f * len);  // blog's k=2 clamp
              const float v = len > 1e-6f ? (bc / len) * norm : 0.0f;  // [-1,1]
              carve += amp * slope_mask * ridge_mask * 0.5f * (v - 1.0f);  // <= 0
              ridge_mask *= std::clamp(1.0f - std::max(0.0f, v), 0.0f, 1.0f);
              lambda *= 0.5f;
              amp *= kPersistence;
            }
            delta.at(x, y) = carve * shore;
          }
        }
      });
  return delta;
}

}  // namespace badlands::mapgen
```

- [ ] **Step 4: Run tests** — `./build/badlands_erosion_tests` → PASS.

- [ ] **Step 5: Commit**

```bash
git add src/mapgen/detail_filter.hpp src/mapgen/detail_filter.cpp src/mapgen/detail_filter_tests.cpp CMakeLists.txt
git commit -m "feat(mapgen): analytic gully detail filter (runevision core subset)"
```

---

### Task 10: Wire the pipeline into generate_map

**Files:**
- Modify: `src/mapgen/generator.hpp` (MapArtifacts + signature), `src/mapgen/generator.cpp`
- Modify: `src/mapgen/generator_tests.cpp`
- Modify: `CMakeLists.txt` (`badlands_generator_tests` now also compiles `hydrology.cpp erosion.cpp detail_filter.cpp`)

**Interfaces:**
- Produces:
```cpp
struct MapArtifacts {
  Field2D<float> bedrock;      // latent field, output res (unchanged)
  Field2D<uint8_t> biome;      // now includes Lake
  Field2D<float> heightmap;    // eroded + detailed ground surface (m)
  Field2D<float> water_depth;  // m standing water; surface = heightmap + water_depth
  Field2D<float> flow;         // drainage area (m²)
  Field2D<float> sediment;     // sediment thickness (m)
};
MapArtifacts generate_map(const MapGenParams& params,
                          MapDebugSink* sink = nullptr);
```
- `MapGenParams` gains `ErosionParams erosion;` (include `"mapgen/erosion.hpp"` from `generator.hpp`).
- Init/output sink stages emitted here: `"bedrock"`, `"biome-sim"`, `"cone"`, `"cavities"` (uint8 mask), `"sediment-init"`, `"water"`, `"detail-delta"`, `"final-height"`, `"biome"` (uint8), sequence numbers 0,1,2,….

- [ ] **Step 1: Update tests first** (`generator_tests.cpp`)

Replace the "plains sit at exactly 0" test (erosion legitimately moves plains — the invariant is gone) and extend determinism; loosen the plains fraction for Lake stamping:

```cpp
TEST_CASE("generate_map: same params -> byte-identical artifacts") {
  MapGenParams p;
  p.seed = 7;
  p.resolution = 64;
  p.world_size_m = 256.0f;
  p.erosion.sim_resolution = 64;
  p.erosion.iterations = 8;   // keep the test fast
  const auto a = generate_map(p);
  const auto b = generate_map(p);
  REQUIRE(a.bedrock.data == b.bedrock.data);
  REQUIRE(a.biome.data == b.biome.data);
  REQUIRE(a.heightmap.data == b.heightmap.data);
  REQUIRE(a.water_depth.data == b.water_depth.data);
  REQUIRE(a.flow.data == b.flow.data);
  REQUIRE(a.sediment.data == b.sediment.data);
}

TEST_CASE("generate_map: lakes are consistent — Lake biome iff standing water") {
  MapGenParams p;
  p.seed = 2;
  p.resolution = 96;
  p.world_size_m = 384.0f;
  p.erosion.sim_resolution = 96;
  p.erosion.iterations = 8;
  const auto a = generate_map(p);
  for (size_t i = 0; i < a.biome.data.size(); ++i) {
    const bool lake = a.biome.data[i] == static_cast<uint8_t>(Biome::Lake);
    REQUIRE(lake == (a.water_depth.data[i] > 0.0f));
    REQUIRE(a.water_depth.data[i] >= 0.0f);
    REQUIRE(a.flow.data[i] > 0.0f);       // every texel drains something
    REQUIRE(a.sediment.data[i] >= 0.0f);
  }
}
```

In the quantile-fraction test, widen the plains margin (Lake stamps over low
ground): `margin(0.02)` → `margin(0.02 + p.erosion.lake_frac)` for plains;
mountain stays `0.02`. Degenerate-resolution test: also require the three new
fields to be empty.

- [ ] **Step 2: Run to verify failure**

Run: `cmake --build build 2>&1 | tail -5`
Expected: FAIL — `MapArtifacts` has no `water_depth` / `generate_map` sink arg.

- [ ] **Step 3: Implement in `generator.cpp`**

Replace the cone-heightmap tail of `generate_map` with the orchestration
(`#include "mapgen/erosion.hpp"`, `"mapgen/hydrology.hpp"`, `"mapgen/detail_filter.hpp"`, `"mapgen/resample.hpp"`):

```cpp
MapArtifacts generate_map(const MapGenParams& params, MapDebugSink* sink) {
  const int w = params.resolution;
  if (w <= 0) return {};
  MapArtifacts a;
  const float texel_out = params.world_size_m / static_cast<float>(w);

  // --- output-res bedrock + biome classification (existing behavior) ---
  a.bedrock = Field2D<float>(w, w);
  // ... (existing three-noise sampling loop, worlds at x * texel_out) ...
  a.biome = classify_biomes(a.bedrock, compute_cutoffs(a.bedrock));

  // --- sim grid: sim_resolution + 2*kPadTexels, world-aligned with pad ---
  const ErosionParams& ep = params.erosion;
  const int sim_n = ep.sim_resolution + 2 * kPadTexels;
  const float texel_sim = params.world_size_m / static_cast<float>(ep.sim_resolution);
  const float origin_sim = -kPadTexels * texel_sim;  // world x of sim texel 0
  Field2D<float> bedrock_sim(sim_n, sim_n);
  // sample the SAME three noise sources at world = x * texel_sim + origin_sim
  // (parallel_tiles loop mirroring the output-res one)
  int seq = 0;
  if (sink) sink->dump("bedrock", seq++, bedrock_sim);
  const auto biome_sim = classify_biomes(bedrock_sim, compute_cutoffs(bedrock_sim));
  if (sink) sink->dump("biome-sim", seq++, biome_sim);

  const auto dist = distance_to_plains(biome_sim, {texel_sim, texel_sim});
  Field2D<float> B(sim_n, sim_n);
  for (size_t i = 0; i < B.data.size(); ++i) B.data[i] = kSlopeMPerM * dist.data[i];
  if (sink) sink->dump("cone", seq++, B);

  const auto basins = carve_cavities(B, bedrock_sim, ep.lake_frac, ep.lake_depth_m);
  if (sink) sink->dump("cavities", seq++, basins);
  auto S = init_sediment(dist, basins, ep, texel_sim, origin_sim, params.seed);
  if (sink) sink->dump("sediment-init", seq++, S);

  const auto sim_out = erode(B, S, ep, texel_sim, sink);

  // --- resample to the output grid (crop = the origin offset) ---
  auto resample = [&](const Field2D<float>& f) {
    return resample_bilinear(f, texel_sim, origin_sim, w, texel_out);
  };
  Field2D<float> ground(sim_n, sim_n);
  for (size_t i = 0; i < ground.data.size(); ++i)
    ground.data[i] = B.data[i] + S.data[i];
  a.heightmap = resample(ground);
  a.sediment = resample(S);
  a.flow = resample(sim_out.flow);

  // water: resample the SURFACE (level where wet, ground where dry) and the
  // depth mask; recompute depth against the output ground so shorelines match
  Field2D<float> surface(sim_n, sim_n);
  for (size_t i = 0; i < surface.data.size(); ++i)
    surface.data[i] = ground.data[i] + sim_out.water_depth.data[i];
  const auto surface_out = resample(surface);
  const auto depth_hint = resample(sim_out.water_depth);
  a.water_depth = Field2D<float>(w, w, 0.0f);
  for (size_t i = 0; i < a.water_depth.data.size(); ++i)
    if (depth_hint.data[i] > 0.01f)
      a.water_depth.data[i] =
          std::max(0.0f, surface_out.data[i] - a.heightmap.data[i]);
  if (sink) sink->dump("water", seq++, a.water_depth);

  // --- detail + biome stamp ---
  const auto delta =
      gully_detail_delta(a.heightmap, a.water_depth, texel_out, params.seed, ep);
  if (sink) sink->dump("detail-delta", seq++, delta);
  for (size_t i = 0; i < delta.data.size(); ++i) a.heightmap.data[i] += delta.data[i];
  for (size_t i = 0; i < a.biome.data.size(); ++i)
    if (a.water_depth.data[i] > 0.0f)
      a.biome.data[i] = static_cast<uint8_t>(Biome::Lake);
  if (sink) {
    sink->dump("final-height", seq++, a.heightmap);
    sink->dump("biome", seq++, a.biome);
  }
  return a;
}
```

The `// ...` marker above is the ONE allowed reference to existing code: keep
the current three-noise `parallel_tiles` sampling loop verbatim, once for the
output grid (as today) and once for the sim grid with `texel_sim`/`origin_sim`
— factor it into a local lambda `sample_bedrock(int n, float texel, float origin)`
so it exists once.

- [ ] **Step 4: Run all tests**

Run: `cmake --build build && ctest --test-dir build -R "badlands_generator_tests|badlands_erosion_tests"`
Expected: PASS. Note the cross-resolution bedrock test still passes untouched
(output-res bedrock sampling did not move).

- [ ] **Step 5: Commit**

```bash
git add src/mapgen/generator.hpp src/mapgen/generator.cpp src/mapgen/generator_tests.cpp CMakeLists.txt
git commit -m "feat(mapgen): erosion+lakes pipeline wired into generate_map"
```

---

### Task 11: PNG debug sink + preview images

**Files:**
- Modify: `src/mapgen/outputs.hpp`, `src/mapgen/outputs.cpp`
- Modify: `src/executables/mapview/main_mapview.cpp` (RunPreviewOnly)
- Modify: `src/mapgen/erosion_tests.cpp` (stage-sequence test via a recording sink)

**Interfaces:**
- Produces:
```cpp
// outputs.hpp
// MapDebugSink that writes each dump as a numbered PNG into out_dir:
//   <NN>-<stage>.png for init/output stages (NN = sequence),
//   loop-<IIII>-<stage>.png for loop stages (IIII = iteration).
// Float fields named *-height / "cone" render as hillshade; "loop-flow"/"flow"
// render log2-scaled; other floats normalized gray. uint8 fields with stage
// "biome"/"biome-sim" use the biome palette; other masks render 0/255 gray.
class PngDebugSink final : public MapDebugSink {
 public:
  PngDebugSink(std::string out_dir, float texel_m);
  void dump(std::string_view stage, int sequence,
            const Field2D<float>& field) override;
  void dump(std::string_view stage, int sequence,
            const Field2D<uint8_t>& mask) override;
 private:
  std::string out_dir_;
  float texel_m_;
};
```
- `write_preview_images` additionally writes `water_depth.png` (gray, normalized), `flow.png` (log2-scaled gray), `sediment.png` (gray, normalized) from the new artifacts.

- [ ] **Step 1: Write the failing test** (append to `erosion_tests.cpp` — tests the SEQUENCE contract with a recorder, no file I/O; PngDebugSink itself is exercised by the preview run in Task 12)

```cpp
#include "mapgen/generator.hpp"

TEST_CASE("generate_map: debug sink sees the full stage sequence") {
  struct Recorder : MapDebugSink {
    std::vector<std::string> stages;
    void dump(std::string_view s, int, const Field2D<float>&) override {
      stages.emplace_back(s);
    }
    void dump(std::string_view s, int, const Field2D<uint8_t>&) override {
      stages.emplace_back(s);
    }
  };
  MapGenParams p;
  p.resolution = 48;
  p.world_size_m = 192.0f;
  p.erosion.sim_resolution = 48;
  p.erosion.iterations = 2;
  p.erosion.dump_every = 1;
  Recorder rec;
  generate_map(p, &rec);
  const std::vector<std::string> expected = {
      "bedrock", "biome-sim", "cone", "cavities", "sediment-init",
      "loop-height", "loop-flow", "loop-sediment", "loop-lakes",
      "loop-height", "loop-flow", "loop-sediment", "loop-lakes",
      "water", "detail-delta", "final-height", "biome"};
  REQUIRE(rec.stages == expected);
}
```

- [ ] **Step 2: Run to verify failure** — this compiles and may already PASS if Task 10 emitted stages in that exact order; if it fails, the fix belongs in `generator.cpp`'s emission order, not the test. Run: `./build/badlands_erosion_tests`.

- [ ] **Step 3: Implement PngDebugSink** (`outputs.cpp`)

```cpp
PngDebugSink::PngDebugSink(std::string out_dir, float texel_m)
    : out_dir_(std::move(out_dir)), texel_m_(texel_m) {}

namespace {
std::string dump_path(const std::string& dir, std::string_view stage, int seq) {
  char buf[32];
  if (stage.rfind("loop-", 0) == 0) {
    std::snprintf(buf, sizeof buf, "loop-%04d-", seq);
    return dir + "/" + buf + std::string(stage.substr(5)) + ".png";
  }
  std::snprintf(buf, sizeof buf, "%02d-", seq);
  return dir + "/" + buf + std::string(stage) + ".png";
}
Field2D<float> log2_scaled(const Field2D<float>& f) {
  Field2D<float> out(f.width, f.height, 0.0f);
  for (size_t i = 0; i < f.data.size(); ++i)
    out.data[i] = std::log2(1.0f + std::max(0.0f, f.data[i]));
  return out;
}
}  // namespace

void PngDebugSink::dump(std::string_view stage, int seq,
                        const Field2D<float>& field) {
  const std::string path = dump_path(out_dir_, stage, seq);
  const bool relief = stage == "cone" || stage == "loop-height" ||
                      stage == "final-height";
  const bool flow = stage == "loop-flow" || stage == "flow";
  if (relief) write_hillshade_png(field, path, texel_m_);
  else if (flow) write_gray_png(log2_scaled(field), path);
  else write_gray_png(field, path);
}

void PngDebugSink::dump(std::string_view stage, int seq,
                        const Field2D<uint8_t>& mask) {
  const std::string path = dump_path(out_dir_, stage, seq);
  if (stage == "biome" || stage == "biome-sim") {
    write_biome_png(mask, path);
    return;
  }
  Field2D<float> f(mask.width, mask.height, 0.0f);
  for (size_t i = 0; i < f.data.size(); ++i) f.data[i] = mask.data[i] ? 1.0f : 0.0f;
  write_gray_png(f, path, /*normalize=*/false);
}
```

Extend `write_preview_images`:

```cpp
  write_gray_png(a.water_depth, out_dir + "/water_depth.png");
  write_gray_png(log2_scaled(a.flow), out_dir + "/flow.png");
  write_gray_png(a.sediment, out_dir + "/sediment.png");
```

(move `log2_scaled` above it). Note `dump("loop-…", it, …)` passes the
ITERATION as sequence — that is what the `loop-%04d` filename wants; init/
output stages pass their running `seq`.

- [ ] **Step 4: Wire into mapview** (`main_mapview.cpp` RunPreviewOnly)

```cpp
  const float texel_m =
      params.world_size_m / static_cast<float>(params.erosion.sim_resolution);
  badlands::mapgen::PngDebugSink sink(out_dir, texel_m);
  const badlands::mapgen::MapArtifacts artifacts =
      badlands::mapgen::generate_map(params, &sink);
```

- [ ] **Step 5: Build, run tests and a smoke preview**

Run: `cmake --build build && ./build/badlands_erosion_tests && ./build/badlands_mapview --preview-image-only --seed 1 --out /tmp/mapgen_smoke && ls /tmp/mapgen_smoke | head -30`
Expected: tests PASS; the dir contains `00-bedrock.png … `, `loop-0010-*.png` frames, `water_depth.png`, `flow.png`, `sediment.png`, and the four legacy rasters.

- [ ] **Step 6: Commit**

```bash
git add src/mapgen/outputs.hpp src/mapgen/outputs.cpp src/executables/mapview/main_mapview.cpp src/mapgen/erosion_tests.cpp
git commit -m "feat(mapgen): PNG debug sink — every pipeline step dumps a raster"
```

---

### Task 12: Integration — full test sweep, preview across seeds, docs

**Files:**
- Modify: `CLAUDE.md` (mapview description), possibly `ErosionParams` defaults after eyeballing.

- [ ] **Step 1: Full build + all tests**

Run: `cmake --build build && ctest --test-dir build`
Expected: all targets PASS (note: `badlands_game_tests` has a pre-existing seed-dependent flake — rerun with `--rng-seed` if it wedges; it is unrelated).

- [ ] **Step 2: Timing + preview sweep**

Run: `for s in 1 2 3; do time ./build/badlands_mapview --preview-image-only --seed $s --out mapgen_out_s$s; done`
Expected: each run completes in a few seconds (the budget). Report the timings.

- [ ] **Step 3: Report for visual judging**

List the output paths (`mapgen_out_s1/…` etc.) for the user to eyeball:
final hillshade vs `02-cone.png` (did erosion carve dendritic valleys?),
`loop-*` film strip (is the sim converging, not exploding?), `water_depth.png`
+ `biome.png` (did lakes survive with sane shorelines?), `flow.png` (dendritic
networks?). Tuning of `ErosionParams` defaults happens HERE with the user —
do not silently retune.

- [ ] **Step 4: Update CLAUDE.md**

In the `badlands_mapview` paragraph, replace the parenthetical pipeline
description with: "it generates a map procedurally (bedrock field →
quantile-cut biomes → stream-power erosion + lakes) and renders it as
biome-colored terrain. `--preview-image-only` instead dumps the debug rasters
(bedrock/biome/heightmap/hillshade/flow/water/sediment PNGs plus a numbered
per-stage + per-N-iterations film strip) to `--out` and exits (pure CPU, no
window)."

- [ ] **Step 5: Commit**

```bash
git add CLAUDE.md
git commit -m "docs: mapview preview dumps the erosion pipeline film strip"
```

---

## Deviations from the spec (recorded during planning)

- **Deposition sweeps:** the spec says "2–3 Gauss–Seidel sweeps per step"; the
  plan implements ONE explicit downstream flux pass per sim iteration — the 80
  outer iterations provide the same relaxation, and the explicit pass is
  simpler and testable in isolation. Update the spec wording when this lands.
- **Sink stage names:** the spec sketches `"loop-hillshade"`; the plan dumps
  raw fields (`"loop-height"`) and lets the PNG sink choose hillshade
  rendering — keeps the generator I/O-free and the sink presentational.
