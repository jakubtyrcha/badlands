// The foliage placement library — MECHANISMS, not looks.
//
// The bulk of this file drives each underlying method against an analytic
// answer (the depth curve, the hash, the clump remap, the EDT depth field, the
// slope probe, cell indexing), because those are the pieces that can be wrong
// in a way no screenshot reveals. The end-to-end section then pins the
// guarantees the whole generator makes: determinism, spacing, the depth
// ordering that IS the feature, and the terrain rejections.
//
// Whether the result looks like a forest — clumped rather than carpeted, ragged
// tree line — is judged by eye from mapview screenshots, deliberately not
// asserted here.

#include <catch_amalgamated.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

#include <FastNoiseLite.h>

#include "foliage/depth_field.hpp"
#include "foliage/foliage_types.hpp"
#include "foliage/forest_type.hpp"
#include "foliage/hash.hpp"
#include "foliage/scatter.hpp"
#include "foliage/terrain_query.hpp"

using namespace badlands;
using namespace badlands::foliage;

namespace {

constexpr float kInf = 3.4e38f;

// A scripted world: a disc of forest coverage on a plane of constant slope,
// with a water level that can be raised to flood it. Every quantity the
// generator reads is analytic, so every rejection it makes has a checkable
// answer.
struct StubTerrain : TerrainQuery {
  glm::vec2 disc_centre{64.0f, 64.0f};
  float disc_radius = 40.0f;
  float base_height = 10.0f;
  float slope_tan = 0.0f;  // height = base + x * slope_tan
  float water = -100.0f;

  float HeightAt(float x, float) const override {
    return base_height + x * slope_tan;
  }
  float CoverageAt(float x, float z) const override {
    return glm::distance(glm::vec2(x, z), disc_centre) <= disc_radius ? 1.0f
                                                                     : 0.0f;
  }
  float WaterLevelM() const override { return water; }
};

// A three-layer forest in the shape of the real one (canopy claims space first,
// then saplings, then bushes) but with only one model per layer, so a test can
// reason about which layer an instance came from without a species table.
ForestType MakeTestForest() {
  ForestType f;
  f.models = {
      // 0: canopy — big, interior-loving
      {.radius_m = 3.2f, .height_m = 20.0f, .scale_range = {0.9f, 1.1f},
       .weight = 1.0f, .depth = {1.0f, 14.0f, kInf, kInf}},
      // 1: sapling — mid band
      {.radius_m = 1.6f, .height_m = 4.5f, .scale_range = {0.8f, 1.2f},
       .weight = 1.0f, .depth = {0.0f, 3.0f, 16.0f, 34.0f}},
      // 2: bush — hugs the edge
      {.radius_m = 0.9f, .height_m = 1.4f, .scale_range = {0.8f, 1.3f},
       .weight = 1.0f, .depth = {0.0f, 1.0f, 10.0f, 22.0f}},
  };
  f.layers = {
      {.grid_m = 6.0f, .max_slope_deg = 32.0f,
       .density = {1.0f, 14.0f, kInf, kInf},
       .edge_scale = 0.55f, .edge_scale_depth_m = 25.0f,
       .first_model = 0, .model_count = 1},
      {.grid_m = 3.0f, .max_slope_deg = 36.0f,
       .density = {0.0f, 3.0f, 16.0f, 34.0f},
       .edge_scale = 0.8f, .edge_scale_depth_m = 12.0f,
       .first_model = 1, .model_count = 1},
      {.grid_m = 1.6f, .max_slope_deg = 40.0f,
       .density = {0.0f, 1.0f, 10.0f, 22.0f},
       .first_model = 2, .model_count = 1},
  };
  return f;
}

FoliageGenParams MakeParams(uint32_t seed = 7) {
  FoliageGenParams p;
  p.seed = seed;
  p.origin_m = {0.0f, 0.0f};
  p.size_m = {128.0f, 128.0f};
  p.mask_texel_m = 1.0f;
  return p;
}

std::vector<FoliageInstance> Flatten(const FoliageField& f) {
  std::vector<FoliageInstance> out;
  for (const std::vector<FoliageInstance>& c : f.cells)
    out.insert(out.end(), c.begin(), c.end());
  return out;
}

}  // namespace

// ---------------------------------------------------------------- DepthCurve

TEST_CASE("DepthCurve is a trapezoid with clamped shoulders", "[foliage]") {
  const DepthCurve c{10.0f, 20.0f, 30.0f, 40.0f};

  CHECK(c.Evaluate(0.0f) == 0.0f);
  CHECK(c.Evaluate(10.0f) == 0.0f);          // at rise_start, still zero
  CHECK(c.Evaluate(15.0f) == Catch::Approx(0.5f));
  CHECK(c.Evaluate(20.0f) == Catch::Approx(1.0f));
  CHECK(c.Evaluate(25.0f) == Catch::Approx(1.0f));  // plateau
  CHECK(c.Evaluate(30.0f) == Catch::Approx(1.0f));
  CHECK(c.Evaluate(35.0f) == Catch::Approx(0.5f));
  CHECK(c.Evaluate(40.0f) == 0.0f);
  CHECK(c.Evaluate(1000.0f) == 0.0f);

  // Monotone up then down, and never outside [0,1].
  float prev = -1.0f;
  for (float d = 10.0f; d <= 20.0f; d += 0.5f) {
    const float v = c.Evaluate(d);
    CHECK(v >= prev);
    CHECK(v >= 0.0f);
    CHECK(v <= 1.0f);
    prev = v;
  }
  prev = 2.0f;
  for (float d = 30.0f; d <= 40.0f; d += 0.5f) {
    const float v = c.Evaluate(d);
    CHECK(v <= prev);
    prev = v;
  }
}

TEST_CASE("DepthCurve plateau with fall_end = FLT_MAX never falls", "[foliage]") {
  const DepthCurve c{1.0f, 14.0f, kInf, kInf};
  CHECK(c.Evaluate(14.0f) == Catch::Approx(1.0f));
  CHECK(c.Evaluate(1000.0f) == Catch::Approx(1.0f));
  CHECK(c.Evaluate(kInteriorDepthM) == Catch::Approx(1.0f));
}

TEST_CASE("DepthCurve degenerate ramp is a step, not a divide by zero",
          "[foliage]") {
  const DepthCurve c{5.0f, 5.0f, kInf, kInf};
  CHECK(c.Evaluate(4.9f) == 0.0f);
  CHECK(c.Evaluate(5.1f) == Catch::Approx(1.0f));
  CHECK(std::isfinite(c.Evaluate(5.0f)));
}

// ---------------------------------------------------------------------- Hash

TEST_CASE("FoliageHash is deterministic and sensitive to every input",
          "[foliage]") {
  const uint32_t base = FoliageHash(1, 0, 10, 20);
  CHECK(FoliageHash(1, 0, 10, 20) == base);

  CHECK(FoliageHash(2, 0, 10, 20) != base);  // seed
  CHECK(FoliageHash(1, 1, 10, 20) != base);  // layer
  CHECK(FoliageHash(1, 0, 11, 20) != base);  // gx
  CHECK(FoliageHash(1, 0, 10, 21) != base);  // gz

  // Negative cell coordinates must hash as cleanly as positive ones -- a field
  // whose origin is left of the world origin depends on it.
  CHECK(FoliageHash(1, 0, -10, -20) != FoliageHash(1, 0, 10, 20));
  CHECK(FoliageHash(1, 0, -10, -20) == FoliageHash(1, 0, -10, -20));

  // Transposing the coordinates must not collide -- a symmetric combine would
  // mirror the whole forest about the diagonal.
  CHECK(FoliageHash(1, 0, 10, 20) != FoliageHash(1, 0, 20, 10));
}

TEST_CASE("HashStream draws are uniform on [0,1)", "[foliage]") {
  constexpr int kBuckets = 16;
  constexpr int kDraws = 160000;
  std::vector<int> hist(kBuckets, 0);

  HashStream rng(FoliageHash(99, 3, 7, 11));
  for (int i = 0; i < kDraws; ++i) {
    const float v = rng.Next01();
    REQUIRE(v >= 0.0f);
    REQUIRE(v < 1.0f);
    hist[std::min(kBuckets - 1, static_cast<int>(v * kBuckets))]++;
  }

  const double expected = static_cast<double>(kDraws) / kBuckets;
  for (int b = 0; b < kBuckets; ++b) {
    // 5% of the bucket mean is ~7 sigma at this sample size; a mixer with a
    // real bias fails this by orders of magnitude.
    CHECK(std::abs(hist[b] - expected) < expected * 0.05);
  }
}

TEST_CASE("HashStream successive draws are uncorrelated", "[foliage]") {
  // One grid cell pulls jitter, density roll, model pick, yaw and scale from
  // the same stream. Correlation between consecutive draws would show up as
  // structure -- e.g. every tall tree facing the same way.
  constexpr int kPairs = 100000;
  HashStream rng(FoliageHash(5, 1, 2, 3));

  double sx = 0, sy = 0, sxy = 0, sxx = 0, syy = 0;
  for (int i = 0; i < kPairs; ++i) {
    const double a = rng.Next01();
    const double b = rng.Next01();
    sx += a; sy += b; sxy += a * b; sxx += a * a; syy += b * b;
  }
  const double n = kPairs;
  const double cov = sxy / n - (sx / n) * (sy / n);
  const double sd_a = std::sqrt(sxx / n - (sx / n) * (sx / n));
  const double sd_b = std::sqrt(syy / n - (sy / n) * (sy / n));
  CHECK(std::abs(cov / (sd_a * sd_b)) < 0.01);
}

TEST_CASE("Triple32 is bijective over a sampled range", "[foliage]") {
  // A non-bijective mixer collapses distinct cells onto one seed, which shows
  // up as duplicated clusters. Spot-check injectivity over a dense range.
  std::vector<uint32_t> seen;
  seen.reserve(20000);
  for (uint32_t i = 0; i < 20000; ++i) seen.push_back(Triple32(i));
  std::sort(seen.begin(), seen.end());
  CHECK(std::adjacent_find(seen.begin(), seen.end()) == seen.end());
}

// --------------------------------------------------------------- Clump remap

TEST_CASE("RemapClump windows the noise range", "[foliage]") {
  // Input is raw FastNoiseLite ~[-1,1]; the window is expressed on the
  // rescaled [0,1] range.
  CHECK(RemapClump(-1.0f, 0.35f, 0.80f) == 0.0f);        // n01 = 0.0
  CHECK(RemapClump(-0.3f, 0.35f, 0.80f) == 0.0f);        // n01 = 0.35, at lo
  CHECK(RemapClump(0.6f, 0.35f, 0.80f) == Catch::Approx(1.0f));   // n01 = 0.80
  CHECK(RemapClump(1.0f, 0.35f, 0.80f) == Catch::Approx(1.0f));
  CHECK(RemapClump(0.15f, 0.35f, 0.80f) == Catch::Approx(0.5f));  // n01 = 0.575

  // Monotone, and clamped to [0,1] throughout.
  float prev = -1.0f;
  for (float r = -1.0f; r <= 1.0f; r += 0.05f) {
    const float v = RemapClump(r, 0.35f, 0.80f);
    CHECK(v >= prev - 1e-6f);
    CHECK(v >= 0.0f);
    CHECK(v <= 1.0f);
    prev = v;
  }
}

TEST_CASE("RemapClump degenerate window is a hard threshold", "[foliage]") {
  CHECK(RemapClump(0.5f, 0.5f, 0.5f) == 1.0f);   // n01 = 0.75 >= 0.5
  CHECK(RemapClump(-0.5f, 0.5f, 0.5f) == 0.0f);  // n01 = 0.25 <  0.5
}

// ---------------------------------------------------------------- DepthField

TEST_CASE("Unwarped depth field matches the analytic disc", "[foliage]") {
  StubTerrain terrain;
  ForestNoise noise;
  noise.warp_amp_m = 0.0f;  // isolate the EDT from the warp

  const DepthField df = BuildDepthField(terrain, {0.0f, 0.0f}, {128.0f, 128.0f},
                                        1.0f, noise, 1);
  REQUIRE_FALSE(df.empty());

  // Inside the disc, depth-into-forest is (radius - distance to centre).
  for (float d = 0.0f; d <= 35.0f; d += 5.0f) {
    const float x = terrain.disc_centre.x + d;
    const float z = terrain.disc_centre.y;
    const float expected = terrain.disc_radius - d;
    // Tolerance is the mask's own quantization: the boundary is resolved to
    // the nearest texel, and the EDT measures to a texel centre.
    CHECK(df.DepthAt(x, z) == Catch::Approx(expected).margin(2.0f));
  }

  // Outside the disc, every texel is its own nearest seed, so depth is 0.
  CHECK(df.DepthAt(5.0f, 5.0f) == Catch::Approx(0.0f).margin(1e-5f));
  CHECK(df.DepthAt(120.0f, 120.0f) == Catch::Approx(0.0f).margin(1e-5f));
}

TEST_CASE("Warp perturbs depth without exceeding its amplitude", "[foliage]") {
  StubTerrain terrain;

  ForestNoise plain;
  plain.warp_amp_m = 0.0f;
  ForestNoise warped;
  warped.warp_amp_m = 4.0f;
  warped.warp_wavelength_m = 12.0f;

  const DepthField a =
      BuildDepthField(terrain, {0, 0}, {128, 128}, 1.0f, plain, 1);
  const DepthField b =
      BuildDepthField(terrain, {0, 0}, {128, 128}, 1.0f, warped, 1);

  bool any_differ = false;
  for (size_t i = 0; i < a.depth.size(); ++i) {
    CHECK(b.depth[i] >= 0.0f);  // clamped, never negative
    // Bounded by the amplitude, except where the clamp at 0 truncates it.
    CHECK(b.depth[i] - a.depth[i] <= warped.warp_amp_m + 1e-3f);
    CHECK(a.depth[i] - b.depth[i] <= warped.warp_amp_m + 1e-3f);
    if (std::abs(a.depth[i] - b.depth[i]) > 0.5f) any_differ = true;
  }
  CHECK(any_differ);  // a warp that changes nothing is not a warp
}

TEST_CASE("Fully covered region reports interior depth, not zero", "[foliage]") {
  // The degenerate that would otherwise suppress the canopy entirely: with no
  // non-forest texel anywhere, the EDT has no seed and returns all zeros.
  struct AllForest : TerrainQuery {
    float HeightAt(float, float) const override { return 10.0f; }
    float CoverageAt(float, float) const override { return 1.0f; }
    float WaterLevelM() const override { return -100.0f; }
  } terrain;

  ForestNoise noise;
  noise.warp_amp_m = 0.0f;
  const DepthField df =
      BuildDepthField(terrain, {0, 0}, {64, 64}, 1.0f, noise, 1);

  REQUIRE_FALSE(df.empty());
  CHECK(df.DepthAt(32.0f, 32.0f) == Catch::Approx(kInteriorDepthM));
  // And a canopy curve must therefore be at its plateau there.
  const DepthCurve canopy{1.0f, 14.0f, kInf, kInf};
  CHECK(canopy.Evaluate(df.DepthAt(32.0f, 32.0f)) == Catch::Approx(1.0f));
}

TEST_CASE("DepthAt clamps off-raster queries to the border", "[foliage]") {
  StubTerrain terrain;
  ForestNoise noise;
  noise.warp_amp_m = 0.0f;
  const DepthField df =
      BuildDepthField(terrain, {0, 0}, {128, 128}, 1.0f, noise, 1);

  CHECK(df.DepthAt(-500.0f, -500.0f) == Catch::Approx(df.DepthAt(0.0f, 0.0f)));
  CHECK(df.DepthAt(5000.0f, 64.0f) == Catch::Approx(df.DepthAt(128.0f, 64.0f)));
  CHECK(std::isfinite(df.DepthAt(-1.0f, 200.0f)));
}

// --------------------------------------------------------------------- Slope

TEST_CASE("SlopeDegreesAt matches an analytic ramp", "[foliage]") {
  StubTerrain terrain;

  terrain.slope_tan = 0.0f;
  CHECK(SlopeDegreesAt(terrain, 30.0f, 30.0f) == Catch::Approx(0.0f).margin(1e-3f));

  for (float deg : {10.0f, 30.0f, 45.0f, 60.0f}) {
    terrain.slope_tan = std::tan(glm::radians(deg));
    CHECK(SlopeDegreesAt(terrain, 30.0f, 30.0f) ==
          Catch::Approx(deg).margin(1e-2f));
  }
}

// -------------------------------------------------------------- Cell indexing

TEST_CASE("Cell indexing round-trips, including on boundaries", "[foliage]") {
  FoliageField f;
  f.origin_m = {10.0f, -20.0f};
  f.cells_x = 4;
  f.cells_z = 4;
  f.cells.resize(16);
  f.cell_y.assign(16, CellYBounds::Empty());

  // A point inside cell (c) maps back to c, and the cell's origin maps to
  // itself.
  for (int cz = 0; cz < f.cells_z; ++cz) {
    for (int cx = 0; cx < f.cells_x; ++cx) {
      const glm::vec2 o = f.CellOrigin(cx, cz);
      CHECK(f.CellCoordAt(o.x + 1.0f, o.y + 1.0f) == glm::ivec2(cx, cz));
      CHECK(f.CellCoordAt(o.x, o.y) == glm::ivec2(cx, cz));  // on the boundary
      CHECK(f.CellIndexAt(o.x + 1.0f, o.y + 1.0f) == f.CellIndex(cx, cz));
    }
  }

  // Off-grid queries clamp rather than escaping the array -- the far edge in
  // particular floors one cell past the end.
  const float far_x = f.origin_m.x + f.cells_x * kFoliageCellSizeM;
  const float far_z = f.origin_m.y + f.cells_z * kFoliageCellSizeM;
  CHECK(f.CellCoordAt(far_x, far_z) == glm::ivec2(f.cells_x - 1, f.cells_z - 1));
  CHECK(f.CellCoordAt(-1000.0f, -1000.0f) == glm::ivec2(0, 0));
  CHECK(f.CellIndexAt(far_x, far_z) < static_cast<int>(f.cells.size()));
}

TEST_CASE("CellYBounds::Empty is detectable and absorbs the first value",
          "[foliage]") {
  CellYBounds b = CellYBounds::Empty();
  CHECK(b.empty());
  b.min_y = std::min(b.min_y, 5.0f);
  b.max_y = std::max(b.max_y, 9.0f);
  CHECK_FALSE(b.empty());
  CHECK(b.min_y == 5.0f);
  CHECK(b.max_y == 9.0f);
}

// --------------------------------------------------------------- Malformed in

TEST_CASE("GenerateFoliage refuses malformed input instead of reading OOB",
          "[foliage]") {
  StubTerrain terrain;

  ForestType empty;
  CHECK(GenerateFoliage(empty, terrain, MakeParams()).empty());

  ForestType bad_slice = MakeTestForest();
  bad_slice.layers[0].first_model = 90;  // past the end of models
  CHECK(GenerateFoliage(bad_slice, terrain, MakeParams()).empty());

  ForestType ok = MakeTestForest();
  FoliageGenParams degenerate = MakeParams();
  degenerate.size_m = {0.0f, 0.0f};
  CHECK(GenerateFoliage(ok, terrain, degenerate).empty());
}

// ---------------------------------------------------------------- End to end

TEST_CASE("Generation is deterministic for a seed", "[foliage]") {
  StubTerrain terrain;
  const ForestType forest = MakeTestForest();

  const FoliageField a = GenerateFoliage(forest, terrain, MakeParams(7));
  const FoliageField b = GenerateFoliage(forest, terrain, MakeParams(7));

  REQUIRE(a.InstanceCount() > 0);
  REQUIRE(a.InstanceCount() == b.InstanceCount());
  REQUIRE(a.cells.size() == b.cells.size());

  const std::vector<FoliageInstance> ia = Flatten(a);
  const std::vector<FoliageInstance> ib = Flatten(b);
  for (size_t i = 0; i < ia.size(); ++i) {
    CHECK(ia[i].position == ib[i].position);
    CHECK(ia[i].yaw == ib[i].yaw);
    CHECK(ia[i].scale == ib[i].scale);
    CHECK(ia[i].model == ib[i].model);
    CHECK(ia[i].layer == ib[i].layer);
  }
}

TEST_CASE("A different seed gives a different forest", "[foliage]") {
  StubTerrain terrain;
  const ForestType forest = MakeTestForest();

  const std::vector<FoliageInstance> a =
      Flatten(GenerateFoliage(forest, terrain, MakeParams(7)));
  const std::vector<FoliageInstance> b =
      Flatten(GenerateFoliage(forest, terrain, MakeParams(8)));

  REQUIRE(a.size() > 0);
  REQUIRE(b.size() > 0);
  // Guards a hash that silently ignores the seed: identical counts AND
  // identical first positions would mean the seed never reached the stream.
  bool differs = a.size() != b.size();
  if (!differs) {
    for (size_t i = 0; i < a.size(); ++i) {
      if (a[i].position != b[i].position) { differs = true; break; }
    }
  }
  CHECK(differs);
}

TEST_CASE("No two instances violate the multi-class spacing rule", "[foliage]") {
  // Brute force over the whole output. This is also what verifies the internal
  // spacing acceleration grid: if its 3x3 neighbourhood ever missed a
  // conflicting neighbour, the violation surfaces right here.
  StubTerrain terrain;
  const ForestType forest = MakeTestForest();
  const FoliageField field = GenerateFoliage(forest, terrain, MakeParams(7));
  const std::vector<FoliageInstance> all = Flatten(field);
  REQUIRE(all.size() > 50);

  for (size_t i = 0; i < all.size(); ++i) {
    for (size_t j = i + 1; j < all.size(); ++j) {
      const float ri = forest.models[all[i].model].radius_m;
      const float rj = forest.models[all[j].model].radius_m;
      const float r = std::max(ri, rj);
      const glm::vec2 d(all[i].position.x - all[j].position.x,
                        all[i].position.z - all[j].position.z);
      CHECK(glm::dot(d, d) >= r * r - 1e-3f);
    }
  }
}

TEST_CASE("Layers sort by depth: bushes outside, canopy inside", "[foliage]") {
  // The whole point of the EDT depth field. If this ordering is not there, the
  // edge treatment does not exist.
  StubTerrain terrain;
  const ForestType forest = MakeTestForest();
  const FoliageField field = GenerateFoliage(forest, terrain, MakeParams(7));

  ForestNoise plain = forest.noise;
  plain.warp_amp_m = 0.0f;
  const DepthField df = BuildDepthField(terrain, {0, 0}, {128, 128}, 1.0f,
                                        forest.noise, 7);

  double sum[3] = {0, 0, 0};
  int count[3] = {0, 0, 0};
  for (const FoliageInstance& i : Flatten(field)) {
    REQUIRE(i.layer < 3);
    sum[i.layer] += df.DepthAt(i.position.x, i.position.z);
    count[i.layer]++;
  }

  REQUIRE(count[0] > 0);
  REQUIRE(count[1] > 0);
  REQUIRE(count[2] > 0);

  const double canopy = sum[0] / count[0];
  const double sapling = sum[1] / count[1];
  const double bush = sum[2] / count[2];
  CHECK(bush < sapling);
  CHECK(sapling < canopy);
}

TEST_CASE("Nothing is planted outside the coverage mask", "[foliage]") {
  StubTerrain terrain;
  const ForestType forest = MakeTestForest();
  const FoliageField field = GenerateFoliage(forest, terrain, MakeParams(7));

  for (const FoliageInstance& i : Flatten(field)) {
    CHECK(terrain.CoverageAt(i.position.x, i.position.z) > 0.0f);
  }
}

TEST_CASE("Nothing is planted in or near water", "[foliage]") {
  StubTerrain terrain;
  terrain.base_height = 10.0f;
  terrain.water = 9.9f;  // clearance is 0.3, so the whole plain is too wet

  const ForestType forest = MakeTestForest();
  const FoliageField field = GenerateFoliage(forest, terrain, MakeParams(7));
  CHECK(field.InstanceCount() == 0);

  // Drop the water and the same map fills in, proving the emptiness above was
  // the water test and not something else rejecting everything.
  terrain.water = -100.0f;
  CHECK(GenerateFoliage(forest, terrain, MakeParams(7)).InstanceCount() > 0);
}

TEST_CASE("Nothing is planted on ground steeper than the layer allows",
          "[foliage]") {
  StubTerrain terrain;
  terrain.slope_tan = std::tan(glm::radians(50.0f));  // past every layer's max

  const ForestType forest = MakeTestForest();
  CHECK(GenerateFoliage(forest, terrain, MakeParams(7)).InstanceCount() == 0);

  // A 20 deg slope is inside every layer's limit, so the map fills.
  terrain.slope_tan = std::tan(glm::radians(20.0f));
  const FoliageField ok = GenerateFoliage(forest, terrain, MakeParams(7));
  REQUIRE(ok.InstanceCount() > 0);
  for (const FoliageInstance& i : Flatten(ok)) {
    CHECK(SlopeDegreesAt(terrain, i.position.x, i.position.z) <= 40.0f + 1e-3f);
  }
}

TEST_CASE("Every instance sits in its own cell, and cell Y bounds contain it",
          "[foliage]") {
  StubTerrain terrain;
  const ForestType forest = MakeTestForest();
  const FoliageField field = GenerateFoliage(forest, terrain, MakeParams(7));
  REQUIRE(field.InstanceCount() > 0);

  for (int cz = 0; cz < field.cells_z; ++cz) {
    for (int cx = 0; cx < field.cells_x; ++cx) {
      const int ci = field.CellIndex(cx, cz);
      const CellYBounds& yb = field.cell_y[static_cast<size_t>(ci)];
      const std::vector<FoliageInstance>& cell = field.cells[static_cast<size_t>(ci)];

      if (cell.empty()) {
        CHECK(yb.empty());  // an untouched cell must stay skippable
        continue;
      }
      CHECK_FALSE(yb.empty());

      for (const FoliageInstance& i : cell) {
        // Filed in the cell its own XZ maps to.
        CHECK(field.CellIndexAt(i.position.x, i.position.z) == ci);
        // And that cell's Y span contains the instance's full standing extent.
        const float top =
            i.position.y + forest.models[i.model].height_m * i.scale;
        CHECK(yb.min_y <= i.position.y + 1e-3f);
        CHECK(yb.max_y >= top - 1e-3f);
      }
    }
  }
}

TEST_CASE("Placement density stays in a sane band", "[foliage]") {
  // A retune that empties the forest, or floods it, is a bug that every other
  // test here would pass in silence: they all assert relationships, not
  // amounts. This pins the amount.
  //
  // The band is wide on purpose -- it is a runaway/collapse guard, not a
  // regression lock on the exact numbers. The measured values for this test
  // forest over the 40 m disc (~5027 m^2) are canopy 54, sapling 127, bush 228.
  StubTerrain terrain;
  const ForestType forest = MakeTestForest();
  const FoliageField field = GenerateFoliage(forest, terrain, MakeParams(7));

  int per_layer[3] = {0, 0, 0};
  for (const FoliageInstance& i : Flatten(field)) per_layer[i.layer]++;

  const float disc_area = 3.14159265f * terrain.disc_radius * terrain.disc_radius;

  // Canopy: between one tree per 40 m^2 (a dense closed stand) and one per
  // 250 m^2 (open woodland). Outside that it is not a forest.
  const float m2_per_canopy = disc_area / static_cast<float>(per_layer[0]);
  CHECK(per_layer[0] > 0);
  CHECK(m2_per_canopy > 40.0f);
  CHECK(m2_per_canopy < 250.0f);

  // The undergrowth layers must outnumber the canopy -- that is what the
  // smaller grid spacing and radius are for.
  CHECK(per_layer[1] > per_layer[0]);
  CHECK(per_layer[2] > per_layer[1]);
}

TEST_CASE("The clump field opens real glades, not a uniform thinning",
          "[foliage]") {
  // The failure this guards is subtle and was live at one point: with a window
  // read against a uniform [0,1] assumption instead of fBm's actual
  // distribution, the clump multiplier never reaches 1 anywhere, so the forest
  // is evenly sparse rather than clumped. Measure the multiplier's spread.
  FastNoiseLite n;
  n.SetSeed(7);
  n.SetNoiseType(FastNoiseLite::NoiseType_OpenSimplex2);
  n.SetFractalType(FastNoiseLite::FractalType_FBm);
  const ForestNoise defaults;
  n.SetFractalOctaves(defaults.clump_octaves);
  n.SetFrequency(1.0f / defaults.clump_wavelength_m);

  int fully_open = 0, fully_closed = 0, total = 0;
  for (int j = 0; j < 300; ++j) {
    for (int i = 0; i < 300; ++i) {
      const float v = RemapClump(n.GetNoise(static_cast<float>(i),
                                            static_cast<float>(j)),
                                 defaults.clump_lo, defaults.clump_hi);
      if (v >= 1.0f) fully_open++;
      if (v <= 0.0f) fully_closed++;
      total++;
    }
  }

  const float open_frac = static_cast<float>(fully_open) / total;
  const float closed_frac = static_cast<float>(fully_closed) / total;
  // Both ends must be well represented: substantial closed canopy AND
  // substantial genuine clearing.
  CHECK(open_frac > 0.20f);
  CHECK(closed_frac > 0.05f);
  CHECK(closed_frac < 0.40f);
}

TEST_CASE("Instances near the edge are scaled down", "[foliage]") {
  StubTerrain terrain;
  const ForestType forest = MakeTestForest();
  const FoliageField field = GenerateFoliage(forest, terrain, MakeParams(7));
  const DepthField df =
      BuildDepthField(terrain, {0, 0}, {128, 128}, 1.0f, forest.noise, 7);

  // Canopy only: it has the strongest ramp (0.55 -> 1.0 over 25 m).
  double near_sum = 0, far_sum = 0;
  int near_n = 0, far_n = 0;
  for (const FoliageInstance& i : Flatten(field)) {
    if (i.layer != 0) continue;
    const float d = df.DepthAt(i.position.x, i.position.z);
    if (d < 8.0f) { near_sum += i.scale; near_n++; }
    else if (d > 20.0f) { far_sum += i.scale; far_n++; }
  }

  REQUIRE(near_n > 0);
  REQUIRE(far_n > 0);
  CHECK(near_sum / near_n < far_sum / far_n);
}
