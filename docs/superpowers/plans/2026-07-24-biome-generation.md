# Biome Generation (bedrock + quantile cutoffs) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Replace the noiser/voronoi/authored-image map generation with a from-scratch procedural biome generator (continuous "bedrock" latent field → quantile-cut biomes), and reset `src/mapgen` down to the pieces that survive.

**Architecture:** `generate_map(MapGenParams) → MapArtifacts{bedrock, biome, heightmap}` is a pure function: FastNoiseLite fBm base + belt-masked ridged fractal composed into a bedrock field sampled in world meters, classified into Plains/Hills/Mountain by exact quantile cutoffs. Heightmap is all-zero this phase (derived from bedrock later). Mapview keeps its 3D cluster-LOD viewer, rendering the flat biome-colored map. The blocks→sections tail, the whole noiser field pipeline, and the authored-image path are deleted; patchgen takes ownership of the noiser script-eval harness.

**Tech Stack:** C++20, CMake/Ninja, FastNoiseLite (MIT, vendored single header), Catch2, glm.

**Spec:** `docs/superpowers/specs/2026-07-24-biome-generation-design.md` (approved). One deliberate deviation: cutoffs use exact `nth_element` order statistics instead of the spec's "histogram ~1024 bins" — same intent (quantiles), simpler and exact.

## Global Constraints

- Run all commands from the repo root (`/Users/jakub/repos/badlands`); shaders/assets/scripts resolve relative to cwd.
- The `MapData` contract (`src/game/map/map_data.hpp`) is FROZEN — do not touch it.
- The `Biome` enum keeps all 6 values; the generator only ever emits `Plains`, `Hills`, `Mountain`.
- Generation tunables are fixed `constexpr` constants — no config file, no ImGui knobs, no env vars.
- Noise is sampled in WORLD METERS at `x * texel_m` (node convention) — this is what the resolution-independence test pins.
- Deleting `assets/map/*.png` removes git-LFS files — use `git rm`, normal.
- Do not bump Dawn or touch other third_party submodules (`third_party/noiser` stays).
- Build: `cmake --build build` · tests: `ctest --test-dir build` (or run test binaries directly).
- Commit messages end with `Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>`.

---

### Task 0 (fold into Task 1's commit): Save this plan into the repo

Copy this plan to `docs/superpowers/plans/2026-07-24-biome-generation.md` and include it in Task 1's commit.

---

### Task 1: Vendor FastNoiseLite + the generator (TDD)

**Files:**
- Create: `third_party/FastNoiseLite/FastNoiseLite.h` (downloaded)
- Create: `src/mapgen/generator.hpp`, `src/mapgen/generator.cpp`, `src/mapgen/generator_tests.cpp`
- Create: `docs/superpowers/plans/2026-07-24-biome-generation.md` (Task 0)
- Modify: `CMakeLists.txt` (mapgen lib gains `generator.cpp` + FastNoiseLite include dir; new `badlands_generator_tests` target)

**Interfaces:**
- Consumes: `Field2D<T>` (`src/mapgen/field2d.hpp`), `Biome`/`kBiomeCount` (`src/mapgen/biomes.hpp`), `parallel_tiles` (`src/mapgen/parallel.hpp`).
- Produces (later tasks rely on these exact names):
  - `badlands::mapgen::MapGenParams{ uint32_t seed; glm::ivec2 resolution; glm::vec2 size_m; }`
  - `badlands::mapgen::MapArtifacts{ Field2D<float> bedrock; Field2D<uint8_t> biome; Field2D<float> heightmap; }`
  - `MapArtifacts generate_map(const MapGenParams&)`
  - `BiomeCutoffs compute_cutoffs(const Field2D<float>&)`, `Field2D<uint8_t> classify_biomes(const Field2D<float>&, const BiomeCutoffs&)`
  - `inline constexpr float kPlainsFrac = 0.55f; inline constexpr float kMountainFrac = 0.12f;`

- [ ] **Step 1: Vendor the noise header**

```bash
mkdir -p third_party/FastNoiseLite
curl -fsSL -o third_party/FastNoiseLite/FastNoiseLite.h \
  https://raw.githubusercontent.com/Auburn/FastNoiseLite/v1.1.1/Cpp/FastNoiseLite.h
grep -m1 "MIT License" third_party/FastNoiseLite/FastNoiseLite.h   # expect a hit
```

(`*.h` is not LFS-tracked; plain add is fine.)

- [ ] **Step 2: Write `src/mapgen/generator.hpp`**

```cpp
#pragma once

// Procedural map generation: a continuous "bedrock" latent field (low-frequency
// fBm + belt-masked ridged fractal), classified into biomes by quantile
// cutoffs. See docs/superpowers/specs/2026-07-24-biome-generation-design.md.
//
// Pure function of params — no I/O, no failure path. Noise is sampled in world
// METERS, so the same (seed, size_m) at two resolutions is the same map, just
// sharper.

#include <cstdint>

#include <glm/glm.hpp>

#include "mapgen/field2d.hpp"

namespace badlands::mapgen {

struct MapGenParams {
  uint32_t seed = 1;
  glm::ivec2 resolution{512, 512};   // texels
  glm::vec2 size_m{512.0f, 512.0f};  // world meters
};

// Everything one generation produces. `bedrock` is the latent field the biomes
// were cut from — kept because previews dump it and erosion will consume it.
struct MapArtifacts {
  Field2D<float> bedrock;    // latent field (raw; roughly [0, 1.9])
  Field2D<uint8_t> biome;    // Biome enum values (Plains/Hills/Mountain now)
  Field2D<float> heightmap;  // world meters — all zeros this phase
};

MapArtifacts generate_map(const MapGenParams& params);

// --- exposed for unit tests (threshold logic without the noise) ---

// Target area fractions. Quantile cutoffs make them structural: they hold for
// every seed, not on average.
inline constexpr float kPlainsFrac = 0.55f;
inline constexpr float kMountainFrac = 0.12f;

// Quantile cutoffs over the ACTUAL bedrock raster: t_hills at kPlainsFrac,
// t_mountain at 1 - kMountainFrac (exact order statistics).
struct BiomeCutoffs {
  float t_hills = 0.0f;
  float t_mountain = 0.0f;
};
BiomeCutoffs compute_cutoffs(const Field2D<float>& bedrock);

// bedrock < t_hills -> Plains, < t_mountain -> Hills, else Mountain.
Field2D<uint8_t> classify_biomes(const Field2D<float>& bedrock,
                                 const BiomeCutoffs& cutoffs);

}  // namespace badlands::mapgen
```

- [ ] **Step 3: Write the failing tests `src/mapgen/generator_tests.cpp`**

```cpp
// The bedrock+quantile map generator — mechanisms, not looks: determinism,
// structural area fractions, cutoff/classification logic, and
// resolution-independent world sampling. Ridge/clump "look" is judged by eye
// via --preview-image-only, deliberately not pinned here.

#include <catch_amalgamated.hpp>

#include <cstdint>

#include "mapgen/biomes.hpp"
#include "mapgen/generator.hpp"

using badlands::mapgen::Biome;
using badlands::mapgen::BiomeCutoffs;
using badlands::mapgen::classify_biomes;
using badlands::mapgen::compute_cutoffs;
using badlands::mapgen::Field2D;
using badlands::mapgen::generate_map;
using badlands::mapgen::MapGenParams;

TEST_CASE("generate_map: same params -> byte-identical artifacts") {
  MapGenParams p;
  p.seed = 7;
  p.resolution = {64, 64};
  p.size_m = {256.0f, 256.0f};
  const auto a = generate_map(p);
  const auto b = generate_map(p);
  REQUIRE(a.bedrock.data == b.bedrock.data);
  REQUIRE(a.biome.data == b.biome.data);
  REQUIRE(a.heightmap.data == std::vector<float>(64 * 64, 0.0f));
}

TEST_CASE("generate_map: quantile cutoffs pin the biome area fractions") {
  for (uint32_t seed : {1u, 2u, 3u}) {
    MapGenParams p;
    p.seed = seed;
    p.resolution = {128, 128};
    p.size_m = {512.0f, 512.0f};
    const auto a = generate_map(p);
    const double n = static_cast<double>(a.biome.data.size());
    double plains = 0.0, mountain = 0.0;
    for (uint8_t b : a.biome.data) {
      if (b == static_cast<uint8_t>(Biome::Plains)) plains += 1.0;
      if (b == static_cast<uint8_t>(Biome::Mountain)) mountain += 1.0;
    }
    // Order statistics are exact up to ties (none in float noise), so a tight
    // margin holds for ANY seed — that is the whole point of quantile cutoffs.
    REQUIRE(plains / n ==
            Catch::Approx(badlands::mapgen::kPlainsFrac).margin(0.02));
    REQUIRE(mountain / n ==
            Catch::Approx(badlands::mapgen::kMountainFrac).margin(0.02));
  }
}

TEST_CASE("compute_cutoffs + classify_biomes: exact on a known ramp") {
  // 101 samples 0.00 .. 1.00: rank floor(0.55*100)=55 -> 0.55, rank 88 -> 0.88.
  Field2D<float> ramp(101, 1);
  for (int x = 0; x < 101; ++x) ramp.at(x, 0) = static_cast<float>(x) / 100.0f;
  const BiomeCutoffs c = compute_cutoffs(ramp);
  REQUIRE(c.t_hills == Catch::Approx(0.55f));
  REQUIRE(c.t_mountain == Catch::Approx(0.88f));

  const auto biome = classify_biomes(ramp, c);
  REQUIRE(biome.at(0, 0) == static_cast<uint8_t>(Biome::Plains));
  REQUIRE(biome.at(54, 0) == static_cast<uint8_t>(Biome::Plains));
  REQUIRE(biome.at(55, 0) == static_cast<uint8_t>(Biome::Hills));  // == t_hills
  REQUIRE(biome.at(87, 0) == static_cast<uint8_t>(Biome::Hills));
  REQUIRE(biome.at(88, 0) ==
          static_cast<uint8_t>(Biome::Mountain));  // == t_mountain
  REQUIRE(biome.at(100, 0) == static_cast<uint8_t>(Biome::Mountain));
}

TEST_CASE("generate_map: bedrock is sampled in world meters "
          "(resolution-independent)") {
  MapGenParams lo;
  lo.seed = 5;
  lo.resolution = {64, 64};
  lo.size_m = {512.0f, 512.0f};  // 8 m texels
  MapGenParams hi = lo;
  hi.resolution = {128, 128};  // 4 m texels
  const auto a = generate_map(lo);
  const auto b = generate_map(hi);
  // Texel (x, y) of the coarse map sits at the same world point as (2x, 2y) of
  // the fine one (world = x * texel_m), so bedrock must agree EXACTLY there —
  // identical float inputs into the same noise.
  for (int y = 0; y < 64; y += 7) {
    for (int x = 0; x < 64; x += 7) {
      REQUIRE(a.bedrock.at(x, y) == b.bedrock.at(2 * x, 2 * y));
    }
  }
}
```

- [ ] **Step 4: Wire CMake**

In `CMakeLists.txt`, add `src/mapgen/generator.cpp` to the `badlands_mapgen_lib` source list (line ~388) and give the lib the vendored include dir (after the existing `target_compile_options` for the lib):

```cmake
target_include_directories(badlands_mapgen_lib PRIVATE
    ${CMAKE_SOURCE_DIR}/third_party/FastNoiseLite)
```

Then add the test target next to `badlands_fog_generator_tests` (same pattern — pure CPU, compiles the TU directly, links only Catch2):

```cmake
# badlands_generator_tests: the bedrock+quantile map generator — determinism,
# structural area fractions, cutoff/classification logic, and
# resolution-independent world-meter sampling. Pure CPU (FastNoiseLite is
# header-only), pattern of badlands_fog_generator_tests.
add_executable(badlands_generator_tests
    src/mapgen/generator_tests.cpp
    src/mapgen/generator.cpp
    third_party/catch2/extras/catch_amalgamated.cpp
)
target_include_directories(badlands_generator_tests PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/third_party/glm
    ${CMAKE_SOURCE_DIR}/third_party/FastNoiseLite
    ${CMAKE_SOURCE_DIR}/third_party/catch2/extras
)
add_test(NAME badlands_generator_tests COMMAND badlands_generator_tests
         WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
```

- [ ] **Step 5: Stub `src/mapgen/generator.cpp` so it compiles, run tests, verify they FAIL**

Stub: all three functions return empty/default fields.

```bash
cmake --build build --target badlands_generator_tests && ./build/badlands_generator_tests
```
Expected: FAIL (empty artifacts / zero cutoffs).

- [ ] **Step 6: Real implementation `src/mapgen/generator.cpp`**

```cpp
#include "mapgen/generator.hpp"

#include <algorithm>
#include <cmath>
#include <variant>
#include <vector>

#include <FastNoiseLite.h>

#include "mapgen/biomes.hpp"
#include "mapgen/parallel.hpp"

namespace badlands::mapgen {

namespace {

// Fixed generation constants (tuned by eye via --preview-image-only).
// Wavelengths are world METERS — generation is resolution-independent.
constexpr float kBaseWavelengthM = 250.0f;    // rolling continental base
constexpr int kBaseOctaves = 4;
constexpr float kRidgedWavelengthM = 120.0f;  // elongated crest lines
constexpr int kRidgedOctaves = 3;
constexpr float kRidgeWeight = 0.9f;  // how far crests rise above the base
// The belt mask gates where ridges may appear (a few mountain belts per map,
// not everywhere). Its wavelength is the map's own extent; the smoothstep
// window admits roughly the top third of the belt field.
constexpr float kBeltLo = 0.55f;
constexpr float kBeltHi = 0.75f;

FastNoiseLite make_noise(int seed, float wavelength_m, int octaves,
                         FastNoiseLite::FractalType fractal) {
  FastNoiseLite n(seed);
  n.SetNoiseType(FastNoiseLite::NoiseType_Perlin);
  n.SetFractalType(fractal);
  n.SetFractalOctaves(octaves);
  n.SetFrequency(1.0f / wavelength_m);
  return n;
}

float to01(float v) { return 0.5f * (v + 1.0f); }  // FastNoiseLite is ~[-1,1]

float smoothstep(float lo, float hi, float x) {
  const float t = std::clamp((x - lo) / (hi - lo), 0.0f, 1.0f);
  return t * t * (3.0f - 2.0f * t);
}

}  // namespace

BiomeCutoffs compute_cutoffs(const Field2D<float>& bedrock) {
  // Exact k-th order statistics. nth_element's VALUE at a rank is
  // deterministic even though the permutation around it is not.
  std::vector<float> v = bedrock.data;
  const size_t n = v.size();
  BiomeCutoffs c;
  if (n == 0) return c;
  const size_t i_hills = static_cast<size_t>(kPlainsFrac * (n - 1));
  const size_t i_mtn = static_cast<size_t>((1.0f - kMountainFrac) * (n - 1));
  std::nth_element(v.begin(), v.begin() + i_hills, v.end());
  c.t_hills = v[i_hills];
  std::nth_element(v.begin() + i_hills, v.begin() + i_mtn, v.end());
  c.t_mountain = v[i_mtn];
  return c;
}

Field2D<uint8_t> classify_biomes(const Field2D<float>& bedrock,
                                 const BiomeCutoffs& cutoffs) {
  Field2D<uint8_t> biome(bedrock.width, bedrock.height);
  for (size_t i = 0; i < bedrock.data.size(); ++i) {
    const float b = bedrock.data[i];
    biome.data[i] = static_cast<uint8_t>(
        b < cutoffs.t_hills      ? Biome::Plains
        : b < cutoffs.t_mountain ? Biome::Hills
                                 : Biome::Mountain);
  }
  return biome;
}

MapArtifacts generate_map(const MapGenParams& params) {
  const int w = params.resolution.x, h = params.resolution.y;
  MapArtifacts a;
  a.bedrock = Field2D<float>(w, h);
  a.heightmap = Field2D<float>(w, h, 0.0f);
  a.biome = Field2D<uint8_t>(w, h);
  if (w <= 0 || h <= 0) return a;

  // Sample at world = x * texel_m (node convention): coinciding world points
  // across two resolutions of the same map get identical float inputs.
  const glm::vec2 texel(params.size_m.x / static_cast<float>(w),
                        params.size_m.y / static_cast<float>(h));

  // Distinct derived seeds per layer, all from params.seed.
  const int s = static_cast<int>(params.seed);
  const FastNoiseLite base = make_noise(s, kBaseWavelengthM, kBaseOctaves,
                                        FastNoiseLite::FractalType_FBm);
  const FastNoiseLite ridged =
      make_noise(s + 1, kRidgedWavelengthM, kRidgedOctaves,
                 FastNoiseLite::FractalType_Ridged);
  const FastNoiseLite belt =
      make_noise(s + 2, std::max(params.size_m.x, params.size_m.y), 1,
                 FastNoiseLite::FractalType_FBm);

  // GetNoise is const and stateless per call, so the three sources are shared
  // read-only across the workers; tiles write disjoint pixels.
  parallel_tiles(
      w, h, 64, [] { return std::monostate{}; },
      [&](std::monostate&, int x0, int y0, int x1, int y1) {
        for (int y = y0; y < y1; ++y) {
          for (int x = x0; x < x1; ++x) {
            const float wx = static_cast<float>(x) * texel.x;
            const float wy = static_cast<float>(y) * texel.y;
            const float mask = smoothstep(kBeltLo, kBeltHi,
                                          to01(belt.GetNoise(wx, wy)));
            a.bedrock.at(x, y) =
                to01(base.GetNoise(wx, wy)) +
                kRidgeWeight * mask * to01(ridged.GetNoise(wx, wy));
          }
        }
      });

  a.biome = classify_biomes(a.bedrock, compute_cutoffs(a.bedrock));
  return a;
}

}  // namespace badlands::mapgen
```

- [ ] **Step 7: Run tests, verify PASS**

```bash
cmake --build build --target badlands_generator_tests && ./build/badlands_generator_tests
```
Expected: All tests pass. Also `cmake --build build` (whole tree still green — nothing existing was touched).

- [ ] **Step 8: Commit**

```bash
git add third_party/FastNoiseLite src/mapgen/generator.hpp src/mapgen/generator.cpp \
        src/mapgen/generator_tests.cpp CMakeLists.txt docs/superpowers/plans/2026-07-24-biome-generation.md
git commit -m "feat(mapgen): bedrock+quantile biome generator (FastNoiseLite)"
```

---

### Task 2: Cut mapview over; delete the dead pipeline, authored path, and assets

One atomic cutover: mapview switches to `generate_map`, `outputs` is reset around the new `MapArtifacts` (the old `pipeline.hpp` defines a struct with the same name, so the old files must die in the same commit — never let both headers coexist), and everything orphaned goes.

**Files:**
- Rewrite: `src/executables/mapview/main_mapview.cpp` (full file below)
- Modify: `src/executables/mapview/map_view_view.hpp`, `src/executables/mapview/map_view_view.cpp`, `src/mapgen/outputs.hpp`, `src/mapgen/outputs.cpp`, `src/game/map/symbolic_map_generator.hpp`, `CMakeLists.txt`
- Delete: `src/mapgen/{pipeline,fields,voronoi,biome_assign,heightmap,sections,authored_map,config}.{hpp,cpp}`, `src/mapgen/{noiser_util.hpp,mapgen_constants.hpp,mapgen_tests.cpp,authored_map_tests.cpp}`, `assets/map/` (LFS), `scripts/mapgen/map_from_render.py`, `scripts/mapgen/fields.noiser`

**Interfaces:**
- Consumes: `generate_map` / `MapGenParams` / `MapArtifacts` (Task 1), `MapData(nodes_x, nodes_z, spacing_m)` + `mutable_height`/`mutable_slice`/`DominantBiomeAt` (`src/game/map/map_data.hpp`), `GenerateBiomeFog(biome, height, seed)` + `BuildBorderFog` (`src/mapgen/fog_generator.hpp`), `biome_name` (`src/mapgen/biomes.hpp`).
- Produces: `write_preview_images(const std::string& out_dir, const MapArtifacts&)`; `MapViewView(mapgen::MapGenParams params, float camera_height = 0, int lod_tint = 0, bool serial_build = false)`.

- [ ] **Step 1: Reset `src/mapgen/outputs.hpp`**

```cpp
#pragma once

#include <string>

#include "mapgen/field2d.hpp"
#include "mapgen/generator.hpp"

namespace badlands::mapgen {

// Dumps the debug rasters for one generated map into out_dir: bedrock.png
// (normalized gray), biome.png (palette), heightmap.png.
//
// The caller is responsible for creating out_dir first (see
// std::filesystem::create_directories); a missing directory surfaces as
// per-file write failures.
void write_preview_images(const std::string& out_dir, const MapArtifacts& a);

// Write a float field as an 8-bit grayscale PNG. If `normalize`, the field's
// [min,max] is stretched to [0,255]; otherwise values are clamped to [0,1].
//
// NOTE: `normalize = true` autoscales PER IMAGE, so two images written this way
// are NOT comparable to each other — the same grey means a different value in
// each. Use the explicit-range overload below when images are meant to be
// compared.
void write_gray_png(const Field2D<float>& field, const std::string& path,
                    bool normalize = true);

// Write a float field as grayscale with an EXPLICIT value range: `lo` maps to
// black, `hi` to white, out-of-range clamps. Use this to render several fields
// against one shared range so their greys mean the same thing.
void write_gray_png_range(const Field2D<float>& field, const std::string& path,
                          float lo, float hi);

// Write a per-pixel biome field (values are Biome) as an RGBA PNG using the
// fixed biome palette.
void write_biome_png(const Field2D<uint8_t>& biome, const std::string& path);

}  // namespace badlands::mapgen
```

- [ ] **Step 2: Reset `src/mapgen/outputs.cpp`**

Keep `write_gray_png`, `write_gray_png_range`, `write_biome_png` bodies exactly as they are. Delete `id_color`, `write_hashed_png`, `write_sections_png`, `write_section_graph_json`, and the `<nlohmann/json.hpp>`, `<fstream>`, `"mapgen/mapgen_constants.hpp"` includes. Replace `write_preview_images` with:

```cpp
void write_preview_images(const std::string& out_dir, const MapArtifacts& a) {
  write_gray_png(a.bedrock, out_dir + "/bedrock.png");
  write_biome_png(a.biome, out_dir + "/biome.png");
  write_gray_png(a.heightmap, out_dir + "/heightmap.png");
}
```

- [ ] **Step 3: Rewrite `src/executables/mapview/main_mapview.cpp`** (complete file)

```cpp
// badlands_mapview: the map tool. Two modes, one generator.
//
//   --preview-image-only   run the generator and dump the debug rasters
//                          (bedrock/biome/heightmap PNGs) into --out, then
//                          exit. Pure CPU: no window, no GPU.
//   (default)              generate the map and render it as the in-game
//                          terrain (cluster-LOD, biome-colored) with a
//                          fixed-angle camera.
//
// Run from the repo root (shaders/ and assets/ resolve relative to cwd).
//
// Usage: badlands_mapview [--seed N] [--resolution WxH] [--size WxH] [--out DIR]
//                         [--preview-image-only]
//                         [--screenshot out.png] [--record dir/]
//
//   --resolution WxH  map texels (default 512x512)
//   --size WxH        map extent in world METERS (default 512x512). Texels must
//                     come out square: size.x/res.x == size.y/res.y.
//   --camera-height H starting camera height in metres (headless framing: a
//                     small H for a near shot, a large one for a far shot).
//   --lod-tint N      debug tint mode for cluster terrain: 0 shaded (default),
//                     1 per-triangle position hash, 2 LOD level.
//   --serial-build    build the cluster DAG single-threaded (default: parallel).
//                     The output DAG is bit-identical either way; this is the
//                     perf A/B baseline (build time shows in the stats log).

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <utility>

#include "engine/app/sdl_viewer_app.hpp"
#include "mapgen/generator.hpp"
#include "mapgen/outputs.hpp"
#include "executables/mapview/map_view_view.hpp"

namespace {

using badlands::mapgen::MapGenParams;

// "WxH" -> the two values via `conv` (stoi for texels, stof for meters).
template <typename T, typename Conv>
std::optional<std::pair<T, T>> parse_wxh(const std::string& s, Conv conv) {
  auto x = s.find('x');
  if (x == std::string::npos) return std::nullopt;
  try {
    T w = conv(s.substr(0, x));
    T h = conv(s.substr(x + 1));
    if (w <= 0 || h <= 0) return std::nullopt;
    return std::make_pair(w, h);
  } catch (const std::exception&) {
    return std::nullopt;
  }
}

// Flags owned by the app layer (SdlViewerApp::Run parses these out of the raw
// argv itself). We must skip them + their value rather than reject them as
// unknown, or --screenshot/--record would stop working here.
bool is_app_flag_with_value(const std::string& a) {
  return a == "--screenshot" || a == "--record";
}

// Builds the map and dumps the rasters. Returns a process exit code.
int RunPreviewOnly(const MapGenParams& params, const std::string& out_dir) {
  std::error_code ec;
  std::filesystem::create_directories(out_dir, ec);
  if (ec) {
    std::fprintf(stderr, "mapview: cannot create out dir '%s': %s\n",
                 out_dir.c_str(), ec.message().c_str());
    return 1;
  }
  const badlands::mapgen::MapArtifacts artifacts =
      badlands::mapgen::generate_map(params);
  std::printf("mapview: %dx%d texels, %.0fx%.0f m, seed=%u -> %s\n",
              params.resolution.x, params.resolution.y, params.size_m.x,
              params.size_m.y, params.seed, out_dir.c_str());
  badlands::mapgen::write_preview_images(out_dir, artifacts);
  std::printf("mapview: done (%s)\n", out_dir.c_str());
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  MapGenParams params;
  std::string out_dir = "mapgen_out";
  bool preview_only = false;
  float camera_height = 0.0f;  // 0 = keep the default framing
  int lod_tint = 0;            // 0 shaded / 1 triangle hash / 2 LOD level
  bool serial_build = false;   // force single-threaded DAG build (perf A/B)

  for (int i = 1; i < argc; ++i) {
    std::string a = argv[i];
    auto next = [&](const char* flag) -> std::optional<std::string> {
      if (i + 1 < argc) return std::string(argv[++i]);
      std::fprintf(stderr, "mapview: %s needs an argument\n", flag);
      return std::nullopt;
    };
    if (a == "--preview-image-only") {
      preview_only = true;
    } else if (a == "--serial-build") {
      serial_build = true;
    } else if (a == "--seed") {
      auto v = next("--seed");
      if (!v) return 2;
      try {
        params.seed = static_cast<uint32_t>(std::stoul(*v));
      } catch (const std::exception&) {
        std::fprintf(stderr, "mapview: bad --seed '%s' (want a number)\n",
                     v->c_str());
        return 2;
      }
    } else if (a == "--resolution") {
      auto v = next("--resolution");
      if (!v) return 2;
      auto r = parse_wxh<int>(*v, [](const std::string& t) { return std::stoi(t); });
      if (!r) {
        std::fprintf(stderr, "mapview: bad --resolution '%s' (want WxH texels)\n",
                     v->c_str());
        return 2;
      }
      params.resolution = {r->first, r->second};
    } else if (a == "--size") {
      auto v = next("--size");
      if (!v) return 2;
      auto r = parse_wxh<float>(*v, [](const std::string& t) { return std::stof(t); });
      if (!r) {
        std::fprintf(stderr, "mapview: bad --size '%s' (want WxH meters)\n",
                     v->c_str());
        return 2;
      }
      params.size_m = {r->first, r->second};
    } else if (a == "--out") {
      if (auto v = next("--out")) out_dir = *v; else return 2;
    } else if (a == "--camera-height") {
      auto v = next("--camera-height");
      if (!v) return 2;
      try {
        camera_height = std::stof(*v);
      } catch (const std::exception&) {
        std::fprintf(stderr, "mapview: bad --camera-height '%s' (want metres)\n",
                     v->c_str());
        return 2;
      }
    } else if (a == "--lod-tint") {
      auto v = next("--lod-tint");
      if (!v) return 2;
      try {
        lod_tint = std::stoi(*v);
      } catch (const std::exception&) {
        lod_tint = -1;
      }
      if (lod_tint < 0 || lod_tint > 2) {
        std::fprintf(stderr, "mapview: bad --lod-tint (want 0, 1, or 2)\n");
        return 2;
      }
    } else if (is_app_flag_with_value(a)) {
      if (!next(a.c_str())) return 2;  // consume the value; SdlViewerApp reads it
    } else {
      std::fprintf(stderr, "mapview: unknown arg '%s'\n", a.c_str());
      return 2;
    }
  }

  // The frozen MapData lattice has ONE spacing scalar, so texels must be
  // square. Reject the contradiction instead of silently distorting the map.
  const float tx = params.size_m.x / static_cast<float>(params.resolution.x);
  const float ty = params.size_m.y / static_cast<float>(params.resolution.y);
  if (std::abs(tx - ty) > 1e-4f * std::max(tx, ty)) {
    std::fprintf(stderr,
                 "mapview: non-square texels (%.4f x %.4f m) — pick "
                 "--resolution/--size with matching aspect\n",
                 tx, ty);
    return 2;
  }

  if (preview_only) return RunPreviewOnly(params, out_dir);

  badlands::SdlViewerApp app({.window_title = "badlands_mapview"});
  return app.Run(argc, argv,
                 [params, camera_height, lod_tint,
                  serial_build](const badlands::RenderContext&) {
                   return std::make_unique<badlands::MapViewView>(
                       params, camera_height, lod_tint, serial_build);
                 });
}
```

- [ ] **Step 4: Update `src/executables/mapview/map_view_view.hpp`**

- Header doc comment: drop the "authored-map load path" and "block/section debug grid" sentences; the view now generates via `mapgen::generate_map` and the mouse-hover shows position + biome.
- Includes: replace `#include "mapgen/config.hpp"` and `#include "mapgen/pipeline.hpp"` with `#include "mapgen/generator.hpp"`.
- Constructor + member: `cfg_` becomes `params_`:

```cpp
  explicit MapViewView(mapgen::MapGenParams params, float camera_height = 0.0f,
                       int lod_tint = 0, bool serial_build = false)
      : params_(params),
        camera_height_override_(camera_height),
        initial_tint_(lod_tint),
        serial_build_(serial_build) {}
```
```cpp
  mapgen::MapGenParams params_;
```
- `map_` keeps its name/type (`mapgen::MapArtifacts map_;`) — the type now comes from generator.hpp; update its comment: heightmap kept for mouse picking, bedrock for previews/erosion later.
- Delete the members `grid_visible_`, `grid_radius_blocks_` and the declarations of `RebuildVisibleGrid()` and `SectionHeight(...)`. Rename `grid_` to `overlay_` (it now only carries the selected fog emitter's OBB) and update its comment.

- [ ] **Step 5: Update `src/executables/mapview/map_view_view.cpp`**

- Delete `constexpr int kSubdiv` and the whole `RebuildVisibleGrid` and `SectionHeight` function bodies.
- Replace `MakeOneHotMapData` (the comment about one-hot slices stays apt — trim the voronoi mention):

```cpp
// Wrap the generator output in the frozen MapData contract at the raster's own
// texel spacing. Slices are ONE-HOT: the hard per-pixel biome assignment, so
// WeightsAtNode(i,j).Dominant() == the single biome and the cluster terrain's
// per-vertex color is the crisp per-texel biome. Blended slices are the game's
// symbolic generator's business.
MapData MakeOneHotMapData(const mapgen::MapArtifacts& art, float texel_m) {
  const int sw = art.bedrock.width, sh = art.bedrock.height;
  if (sw <= 0 || sh <= 0 || texel_m <= 0.0f) return {};
  // One more node than texels per axis: node i sits at i * texel_m, so the
  // lattice spans exactly the map's size_m; edge nodes clamp to the last texel.
  MapData map(sw + 1, sh + 1, texel_m);
  for (int j = 0; j <= sh; ++j) {
    for (int i = 0; i <= sw; ++i) {
      const int sx = std::min(i, sw - 1), sz = std::min(j, sh - 1);
      map.mutable_height(i, j) = art.heightmap.at(sx, sz);
      map.mutable_slice(art.biome.at(sx, sz), i, j) = 255;
    }
  }
  return map;
}
```

- `Initialize`: the pipeline block (lines ~89–114) becomes:

```cpp
  // Build the map in-process — the same generator --preview-image-only dumps,
  // so the rendered terrain and the preview PNGs can never disagree.
  t = clock::now();
  map_ = mapgen::generate_map(params_);
  log_step("mg:generate", since(t));
  map_size_m_ = params_.size_m.x;
```
  (The `spdlog::info("map load profile ...")` line above it now prints `params_.seed`, `params_.resolution.x/y`; the `std::string err;` and the `if (!ok)` block go.)
- `terrain_map_` wrap:

```cpp
  t = clock::now();
  terrain_map_ = MakeOneHotMapData(
      map_, params_.size_m.x / static_cast<float>(params_.resolution.x));
  log_step("map->MapData", since(t));
```
- Camera framing: `const float map_depth_m = params_.size_m.y;` (drop the `kMetersPerSample` factor); rest unchanged.
- Fog generation: `fog_emitters_ = mapgen::GenerateBiomeFog(map_.biome, map_.heightmap, params_.seed);` (also in `DrawFogEmitterEditor`'s "Regenerate from biomes" button).
- Final stats log drops sections: `spdlog::info("map load: {:.1f} ms total  ({}x{} texels, {} fog emitters)", since(t_load), params_.resolution.x, params_.resolution.y, fog_emitters_.size());` and the trailing "grid follows the mouse" comment goes.
- `SetFogSources`: `const glm::vec2 map_max(params_.size_m.x, params_.size_m.y);`
- `Update`: the overlay block becomes (grid rebuild gone, emitter OBB stays):

```cpp
  // Rebuild the debug-line overlay each frame: just the selected fog emitter's
  // OBB now. Small and camera-independent, so rebuilding is cheap.
  overlay_.Clear();
  if (selected_emitter_ >= 0 &&
      selected_emitter_ < static_cast<int>(fog_emitters_.size())) {
    const fog::Emitter& e = fog_emitters_[selected_emitter_];
    overlay_.AddOrientedBox(e.center, e.rotation, e.half_extent, e.base_y,
                            e.base_y + e.height, glm::vec3(1.0f, 0.25f, 0.9f),
                            2.5f);
  }
  scene_context_.debug_lines = overlay_.empty() ? nullptr : &overlay_;
```
- `DrawUI` "Map" window: header text + hover become:

```cpp
  ImGui::Text("seed %u  %dx%d texels  %.0fx%.0f m", params_.seed,
              params_.resolution.x, params_.resolution.y, params_.size_m.x,
              params_.size_m.y);
  cluster_terrain_.DrawDebugUI();
  ImGui::Text("focus: (%.0f, %.0f)", gamecam_.focus.x, gamecam_.focus.z);
  if (hover_valid_) {
    const std::string_view bn = mapgen::biome_name(
        terrain_map_.DominantBiomeAt(hover_point_.x, hover_point_.z));
    ImGui::Text("hover: (%.1f, %.1f, %.1f)  %.*s", hover_point_.x,
                hover_point_.y, hover_point_.z, static_cast<int>(bn.size()),
                bn.data());
  } else {
    ImGui::TextUnformatted("hover: (off terrain)");
  }
```
  Delete the `sections:` line, the `Grid (block + section)` checkbox and the `Grid radius (blocks)` slider. Add `#include "mapgen/biomes.hpp"` and drop `#include "mapgen/mapgen_constants.hpp"` and `#include "mapgen/pipeline.hpp"`; `#include "mapgen/fog_generator.hpp"` stays.
- All other `grid_.` references become `overlay_.`.

- [ ] **Step 6: Inline the spacing constant in `src/game/map/symbolic_map_generator.hpp`**

```cpp
  static constexpr float kSpacingM = 4.0f;  // block-edge lattice spacing
```
and delete `#include "mapgen/mapgen_constants.hpp"`.

- [ ] **Step 7: Delete the dead files + assets**

```bash
git rm src/mapgen/pipeline.hpp src/mapgen/pipeline.cpp \
       src/mapgen/fields.hpp src/mapgen/fields.cpp src/mapgen/noiser_util.hpp \
       src/mapgen/voronoi.hpp src/mapgen/voronoi.cpp \
       src/mapgen/biome_assign.hpp src/mapgen/biome_assign.cpp \
       src/mapgen/heightmap.hpp src/mapgen/heightmap.cpp \
       src/mapgen/sections.hpp src/mapgen/sections.cpp \
       src/mapgen/authored_map.hpp src/mapgen/authored_map.cpp \
       src/mapgen/authored_map_tests.cpp src/mapgen/mapgen_tests.cpp \
       src/mapgen/config.hpp src/mapgen/config.cpp src/mapgen/mapgen_constants.hpp
git rm -r assets/map
git rm scripts/mapgen/map_from_render.py scripts/mapgen/fields.noiser
```

- [ ] **Step 8: CMake cleanup**

- `badlands_mapgen_lib` sources shrink to: `src/mapgen/fog_generator.cpp`, `src/mapgen/hillshade.cpp`, `src/mapgen/outputs.cpp`, `src/mapgen/script_eval.cpp`, `src/mapgen/generator.cpp` (script_eval/hillshade and the `badlands_game_lib` link stay until Task 3). Update the lib's comment (generator + outputs + fog; noiser only for patchgen's script_eval until it moves).
- Delete the `badlands_mapgen_tests` and `badlands_authored_map_tests` targets and their `add_test` lines entirely (`badlands_generator_tests` from Task 1 is the replacement).
- Update the `badlands_mapview` target comment (no sections/hover grid).

- [ ] **Step 9: Build everything, run all tests**

```bash
cmake --build build && ctest --test-dir build
```
Expected: green build; `badlands_generator_tests`, `badlands_patch_eval_tests`, `badlands_fog_generator_tests`, `badlands_terrain_cluster_tests`, game/geometry tests all pass; the two deleted test targets are gone from the list.

- [ ] **Step 10: Functional smoke**

```bash
./build/badlands_mapview --preview-image-only --seed 2 --out /private/tmp/claude-501/-Users-jakub-repos-badlands/eaeb0836-0812-470c-827a-3c7c4fe31d0f/scratchpad/mapgen_out
ls /private/tmp/claude-501/-Users-jakub-repos-badlands/eaeb0836-0812-470c-827a-3c7c4fe31d0f/scratchpad/mapgen_out
# expect exactly: bedrock.png  biome.png  heightmap.png
./build/badlands_mapview --seed 2 --screenshot /private/tmp/claude-501/-Users-jakub-repos-badlands/eaeb0836-0812-470c-827a-3c7c4fe31d0f/scratchpad/mapview.png
```
Read both PNGs (Read tool) and eyeball: biome.png shows plains clumps (pale yellow-green) dominating, gray elongated mountain shapes wrapped by brown hills bands; the screenshot shows the flat biome-colored terrain. If mountains read as round blobs rather than ridges, or belts cover everything/nothing, tune `kRidgeWeight`/`kBeltLo`/`kBeltHi`/`kRidgedWavelengthM` in generator.cpp (constants only — no new knobs) and re-check; note what changed in the commit message.

- [ ] **Step 11: Commit**

```bash
git add -A
git commit -m "refactor(mapview)!: generate biomes via bedrock+quantile cutoffs; drop noiser pipeline, sections, authored maps"
```

---

### Task 3: Move the noiser script-eval harness to patchgen; mapgen goes noiser-free

**Files:**
- Move: `src/mapgen/script_eval.{hpp,cpp}` → `src/executables/patchgen/`, `src/mapgen/hillshade.{hpp,cpp}` → `src/executables/patchgen/`, `src/mapgen/patch_eval_tests.cpp` → `src/executables/patchgen/`
- Modify: `src/executables/patchgen/main_patchgen.cpp` (include paths), the moved files' own includes, `CMakeLists.txt`

**Interfaces:**
- Consumes: nothing new; the moved code is unchanged (namespace stays `badlands::mapgen` — pure relocation, no churn).
- Produces: `badlands_mapgen_lib` linking only `badlands_engine` (no noiser anywhere under `src/mapgen/`).

- [ ] **Step 1: Move the files**

```bash
git mv src/mapgen/script_eval.hpp src/mapgen/script_eval.cpp \
       src/mapgen/hillshade.hpp src/mapgen/hillshade.cpp \
       src/mapgen/patch_eval_tests.cpp src/executables/patchgen/
```

- [ ] **Step 2: Fix includes**

In `main_patchgen.cpp`, `script_eval.cpp`, `hillshade.cpp`, `patch_eval_tests.cpp`: `"mapgen/script_eval.hpp"` → `"executables/patchgen/script_eval.hpp"` and `"mapgen/hillshade.hpp"` → `"executables/patchgen/hillshade.hpp"`. (`"mapgen/field2d.hpp"` / `"mapgen/outputs.hpp"` references stay — those files remain in mapgen.)

- [ ] **Step 3: CMake**

- `badlands_mapgen_lib`: sources become just `src/mapgen/fog_generator.cpp`, `src/mapgen/outputs.cpp`, `src/mapgen/generator.cpp`; link becomes `target_link_libraries(badlands_mapgen_lib PUBLIC badlands_engine)`; drop its `-Wno-deprecated-declarations` (that was for the noiser headers); update its comment (map generation + preview writers + fog generator; engine only for CpuImage).
- `badlands_patchgen`: sources become `src/executables/patchgen/main_patchgen.cpp`, `src/executables/patchgen/script_eval.cpp`, `src/executables/patchgen/hillshade.cpp`; link `PRIVATE badlands_mapgen_lib badlands_game_lib` (game_lib carries the noiser VM).
- `badlands_patch_eval_tests`: sources become `src/executables/patchgen/patch_eval_tests.cpp`, `src/executables/patchgen/script_eval.cpp` (rest of the target unchanged).

- [ ] **Step 4: Build + tests + patchgen smoke**

```bash
cmake --build build && ctest --test-dir build
./build/badlands_patchgen --script scripts/mapgen/biomes/hills_ridged_fbm.noiser \
    --out /private/tmp/claude-501/-Users-jakub-repos-badlands/eaeb0836-0812-470c-827a-3c7c4fe31d0f/scratchpad/patches --extent 2000 --res 128 --seed 2
```
Expected: green; patchgen writes its PNG pair (its noiser path is intact).

- [ ] **Step 5: Commit**

```bash
git add -A
git commit -m "refactor(patchgen): own the noiser script-eval harness; mapgen is noiser-free"
```

---

### Task 4: Docs sweep + end-to-end verification

**Files:**
- Modify: `CLAUDE.md` (Build & run section), `src/game/geometry/terrain_clusters.hpp:40` (comment references the deleted `mapgen_constants.hpp`)

- [ ] **Step 1: Update `CLAUDE.md`'s mapview paragraph**

Replace the `badlands_mapview` description + example block with:

```markdown
`badlands_mapview` is the map tool: it generates a map procedurally (bedrock
field → quantile-cut biomes) and renders it as biome-colored terrain.
`--preview-image-only` instead dumps the debug rasters (bedrock/biome/heightmap
PNGs) to `--out` and exits (pure CPU, no window).
```sh
./build/badlands_mapview --seed 2 --resolution 500x500 --size 500x500   # view it
./build/badlands_mapview --preview-image-only --out mapgen_out          # dump PNGs
```
```

- [ ] **Step 2: Fix stale comments**

- `src/game/geometry/terrain_clusters.hpp:40`: reword "pattern as mapgen_constants.hpp" to not reference the deleted file (e.g. "COMPILE-TIME by design: they define the cluster grid structure and never vary per run").
- `src/mapgen/field2d.hpp:9-10`: the doc comment enumerates deleted users — reword to "Used for the bedrock/heightmap (float) and the biome map (Biome)."

- [ ] **Step 3: Full verification**

```bash
cmake --build build && ctest --test-dir build
rg -n "noiser" src/mapgen/            # expect: no hits
rg -ln "authored|map_dir|kSamplesPerBlock" src/ | grep -v patchgen  # expect: no hits
perl -e 'alarm 20; exec @ARGV' ./build/badlands_mapview --seed 3    # bounded interactive run, expect clean SIGALRM exit
./build/badlands_game --screenshot /private/tmp/claude-501/-Users-jakub-repos-badlands/eaeb0836-0812-470c-827a-3c7c4fe31d0f/scratchpad/game.png  # game app unaffected
```

- [ ] **Step 4: Commit**

```bash
git add -A
git commit -m "docs: mapview usage + stale mapgen references"
```

---

## Verification (end-to-end recap)

1. `cmake --build build` — whole tree green after every task.
2. `ctest --test-dir build` — `badlands_generator_tests` (determinism, fractions, cutoffs, resolution-independence) + all surviving suites pass; `badlands_mapgen_tests`/`badlands_authored_map_tests` no longer exist.
3. `./build/badlands_mapview --preview-image-only --seed 2 --out <scratch>` → exactly `bedrock.png`, `biome.png`, `heightmap.png`; biome.png judged by eye: plains clumps dominant, elongated gray ridges wrapped in brown hills bands.
4. `./build/badlands_mapview --seed 2 --screenshot <scratch>/mapview.png` → flat biome-colored 3D terrain, fog border wall, no crash.
5. `./build/badlands_patchgen --script scripts/mapgen/biomes/hills_ridged_fbm.noiser ...` → patchgen's noiser path intact.
6. `--resolution 256x256 --size 512x512` renders the same map coarser (meters-based sampling, texel = 2 m).
