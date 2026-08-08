// The two TEMPORARY stand-in derivations a bundle-backed map source needs: soil
// estimated from slope, and a lake BED modelled under water whose extent and
// surface both came from the data. Both are replaced by the simulation's own
// rasters.

#include <catch_amalgamated.hpp>

#include <cmath>

#include "mapgen/cover.hpp"
#include "mapgen/soil_estimate.hpp"
#include "mapgen/standing_water.hpp"

using badlands::mapgen::Cover;
using badlands::mapgen::derive_standing_water;
using badlands::mapgen::estimate_soil;
using badlands::mapgen::Field2D;
using Catch::Matchers::WithinAbs;
using Catch::Matchers::WithinRel;

namespace {

// A field tilted by `deg` along +x, so every interior texel has exactly that
// slope and the estimate can be checked against the calibration directly.
Field2D<float> tilted(int w, int h, float deg, float texel_m) {
  const float rise = std::tan(deg * 3.14159265358979f / 180.0f) * texel_m;
  Field2D<float> f(w, h, 0.0f);
  for (int y = 0; y < h; ++y) {
    for (int x = 0; x < w; ++x) f.at(x, y) = static_cast<float>(x) * rise;
  }
  return f;
}

Field2D<uint8_t> all_cover(int w, int h, Cover c) {
  return Field2D<uint8_t>(w, h, static_cast<uint8_t>(c));
}

}  // namespace

// ---------------------------------------------------------------- soil ------

TEST_CASE("the soil fit passes through both measured points", "[soil]") {
  // The whole claim the estimate makes: it interpolates between two MEASURED
  // slope/depth pairs from the coarse world. If it misses them it is inventing.
  const Field2D<float> deep = tilted(8, 8, 7.1f, 1.0f);
  const Field2D<float> thin = tilted(8, 8, 31.6f, 1.0f);

  REQUIRE_THAT(estimate_soil(deep, 1.0f).at(4, 4), WithinRel(4.0f, 0.01f));
  REQUIRE_THAT(estimate_soil(thin, 1.0f).at(4, 4), WithinRel(0.4f, 0.01f));
}

TEST_CASE("soil decreases monotonically with slope", "[soil]") {
  float previous = 1e9f;
  for (float deg : {0.0f, 5.0f, 15.0f, 30.0f, 45.0f, 60.0f}) {
    const float s = estimate_soil(tilted(8, 8, deg, 1.0f), 1.0f).at(4, 4);
    REQUIRE(s < previous);
    REQUIRE(s >= 0.0f);
    previous = s;
  }
  // A cliff is close to bedrock, which is the entire point of the relation.
  REQUIRE(estimate_soil(tilted(8, 8, 60.0f, 1.0f), 1.0f).at(4, 4) < 0.1f);
}

TEST_CASE("soil is resolution independent", "[soil]") {
  // The same terrain sampled more finely must give the same soil -- the slope
  // is per METRE, not per texel. A gradient divided by texels would make this
  // fail by exactly the resolution ratio.
  const float coarse = estimate_soil(tilted(8, 8, 20.0f, 4.0f), 4.0f).at(4, 4);
  const float fine = estimate_soil(tilted(32, 32, 20.0f, 0.5f), 0.5f).at(16, 16);
  REQUIRE_THAT(coarse, WithinRel(fine, 0.001f));
}

TEST_CASE("a degenerate soil request returns an empty field", "[soil]") {
  REQUIRE(estimate_soil(Field2D<float>(), 1.0f).size() == 0);
  REQUIRE(estimate_soil(tilted(4, 4, 10.0f, 1.0f), 0.0f).size() == 0);
}

// --------------------------------------------------------------- water ------

namespace {

// What a real bundle looks like, per the measurement in standing_water.hpp: a
// flat PLATE at the water surface with a rim rising around it. Not a basin --
// the survey never saw the bottom. `noise` reproduces the plate's own scatter
// (measured at 0.28 m std), which is what makes an elevation filter over the
// extent a mistake.
struct Plate {
  Field2D<float> dtm;
  Field2D<uint8_t> cover;
};

Plate plate(int w, int h, int x0, int y0, int side, float surface,
            float rim_above, float noise = 0.0f) {
  Plate p;
  p.dtm = Field2D<float>(w, h, surface + rim_above);
  p.cover = all_cover(w, h, Cover::Grass);
  int k = 0;
  for (int y = y0; y < y0 + side; ++y) {
    for (int x = x0; x < x0 + side; ++x, ++k) {
      // Deterministic alternating scatter about the surface.
      p.dtm.at(x, y) = surface + ((k % 2) ? noise : -noise);
      p.cover.at(x, y) = static_cast<uint8_t>(Cover::Water);
    }
  }
  return p;
}

int WetCount(const badlands::mapgen::StandingWater& out) {
  int n = 0;
  for (size_t i = 0; i < out.level.size(); ++i)
    n += (out.level.data[i] > out.bed.data[i]);
  return n;
}

}  // namespace

TEST_CASE("the extent is the observed mask, exactly", "[water]") {
  // THE DEFECT THIS PINS. Taking the plate's median as a surface and then
  // dropping every texel above it discards half the plate by definition --
  // measured once as one lake becoming ten fragments and 13% water rendering as
  // 9.5%. Every observed texel is water, whatever its own elevation.
  Plate p = plate(48, 48, 8, 8, 32, 100.0f, 1.5f, /*noise=*/0.28f);

  const auto out = derive_standing_water(p.dtm, p.cover, 1.0f);

  REQUIRE(WetCount(out) == 32 * 32);
}

TEST_CASE("an observed plate becomes a flat surface over a carved bed",
          "[water]") {
  Plate p = plate(48, 48, 8, 8, 32, 100.0f, 1.5f, 0.28f);

  const auto out = derive_standing_water(p.dtm, p.cover, 1.0f);

  // EXACTLY flat, and at the plate's own elevation -- not raised to the rim,
  // which is what a terrain flood would have done (+1.5 m here).
  REQUIRE(out.level.at(10, 10) == out.level.at(30, 30));
  REQUIRE_THAT(out.level.at(24, 24), WithinAbs(100.0f, 0.3f));
  // A bed beneath it, so the contract can express a lake at all.
  REQUIRE(out.bed.at(24, 24) < out.level.at(24, 24));
  // Dry outside: level == bed is the contract's dry texel, and the raster is
  // untouched there.
  REQUIRE(out.level.at(0, 0) == out.bed.at(0, 0));
  REQUIRE(out.bed.at(0, 0) == p.dtm.at(0, 0));
}

TEST_CASE("depth follows distance from the nearest non-water texel", "[water]") {
  Plate p = plate(80, 80, 8, 8, 64, 100.0f, 1.5f);

  const auto out = derive_standing_water(p.dtm, p.cover, 1.0f);

  const auto depth_at = [&](int x, int y) {
    return out.level.at(x, y) - out.bed.at(x, y);
  };
  // One texel in from the shore is one texel deep at the modelled slope.
  REQUIRE_THAT(depth_at(8, 40), WithinAbs(badlands::mapgen::kLakeBedSlope, 1e-4f));
  REQUIRE(depth_at(8, 40) < depth_at(16, 40));
  REQUIRE(depth_at(16, 40) < depth_at(32, 40));
  // And it is capped, so a wide lake is a basin rather than a canyon.
  REQUIRE(depth_at(40, 40) <= badlands::mapgen::kMaxLakeDepthM + 1e-4f);
}

TEST_CASE("a misclassified speck becomes a puddle, not a pond", "[water]") {
  // At 10 m, one bad cover pixel covers 10x10 texels of a 1 m grid. The model
  // self-limits: nothing in it is more than a few texels from dry ground.
  Plate p = plate(48, 48, 20, 20, 10, 100.0f, 1.5f);

  const auto out = derive_standing_water(p.dtm, p.cover, 1.0f);

  const float deepest = out.level.at(24, 24) - out.bed.at(24, 24);
  REQUIRE(deepest > 0.0f);
  REQUIRE(deepest < 1.0f);
}

TEST_CASE("unobserved ground stays dry however hollow it is", "[water]") {
  // Real 1 m LiDAR is full of drystone walls, field banks and road embankments
  // that genuinely dam water; a terrain-only flood ponds behind every one.
  Field2D<float> dtm(32, 32, 10.0f);
  for (int y = 8; y < 24; ++y)
    for (int x = 8; x < 24; ++x) dtm.at(x, y) = 7.0f;  // a deep, closed hollow
  const Field2D<uint8_t> cover = all_cover(32, 32, Cover::Grass);

  const auto out = derive_standing_water(dtm, cover, 1.0f);

  for (size_t i = 0; i < out.level.size(); ++i) {
    REQUIRE(out.level.data[i] == dtm.data[i]);
    REQUIRE(out.bed.data[i] == dtm.data[i]);
  }
}

TEST_CASE("a bank inside the mask is still water", "[water]") {
  // The mask is 10 m against 1 m relief, so it over-claims. Trusting the data
  // means trusting it here too; the never-raise rule keeps the bank's own
  // survey height wherever that is already below the modelled bed.
  Plate p = plate(48, 48, 8, 8, 32, 100.0f, 1.5f);
  for (int y = 20; y < 24; ++y) p.dtm.at(20, y) = 101.0f;

  const auto out = derive_standing_water(p.dtm, p.cover, 1.0f);

  REQUIRE(out.level.at(20, 21) > out.bed.at(20, 21));
  REQUIRE(out.level.at(20, 21) == out.level.at(24, 24));  // one flat surface
}

TEST_CASE("the patch edge is not a shore", "[water]") {
  // Water leaving the frame carries on in the real world. Tapering it to zero
  // against the boundary would invent a shore that is not there.
  Field2D<float> dtm(48, 48, 100.0f);
  Field2D<uint8_t> cover = all_cover(48, 48, Cover::Grass);
  for (int y = 0; y < 48; ++y)
    for (int x = 0; x < 24; ++x) cover.at(x, y) = static_cast<uint8_t>(Cover::Water);

  const auto out = derive_standing_water(dtm, cover, 1.0f);

  // Against the frame, depth is set by the distance to the REAL shore at x=24,
  // not by proximity to the edge -- so the far corner is the deepest point.
  const float at_edge = out.level.at(0, 24) - out.bed.at(0, 24);
  const float at_shore = out.level.at(23, 24) - out.bed.at(23, 24);
  REQUIRE(at_edge > at_shore);
}

TEST_CASE("two lakes at different elevations keep their own surfaces",
          "[water]") {
  Field2D<float> dtm(96, 40, 200.0f);
  Field2D<uint8_t> cover = all_cover(96, 40, Cover::Grass);
  for (int y = 8; y < 32; ++y) {
    for (int x = 8; x < 32; ++x) {
      dtm.at(x, y) = 100.0f;
      cover.at(x, y) = static_cast<uint8_t>(Cover::Water);
    }
    for (int x = 60; x < 84; ++x) {
      dtm.at(x, y) = 150.0f;
      cover.at(x, y) = static_cast<uint8_t>(Cover::Water);
    }
  }

  const auto out = derive_standing_water(dtm, cover, 1.0f);

  REQUIRE(out.level.at(20, 20) == 100.0f);
  REQUIRE(out.level.at(72, 20) == 150.0f);
}

TEST_CASE("water needs observation, so a missing cover raster yields dry ground",
          "[water]") {
  Plate p = plate(32, 32, 8, 8, 16, 100.0f, 1.5f);

  const auto out = derive_standing_water(p.dtm, Field2D<uint8_t>(), 1.0f);

  REQUIRE(out.level.size() == p.dtm.size());
  for (size_t i = 0; i < out.level.size(); ++i) {
    REQUIRE(out.level.data[i] == p.dtm.data[i]);
    REQUIRE(out.bed.data[i] == p.dtm.data[i]);
  }
}

TEST_CASE("an all-water raster has no shore to measure from", "[water]") {
  Field2D<float> dtm(16, 16, 50.0f);
  const Field2D<uint8_t> cover = all_cover(16, 16, Cover::Water);

  const auto out = derive_standing_water(dtm, cover, 1.0f);

  // No dry texel anywhere, so nothing seeds the distance field. Full depth is
  // the honest answer; zero would render an ocean as a dry plate.
  REQUIRE(out.level.at(8, 8) - out.bed.at(8, 8) ==
          Catch::Approx(badlands::mapgen::kMaxLakeDepthM));
}

TEST_CASE("a degenerate water request returns empty fields", "[water]") {
  const auto out = derive_standing_water(Field2D<float>(), Field2D<uint8_t>(), 1.0f);
  REQUIRE(out.level.size() == 0);
  REQUIRE(out.bed.size() == 0);
}
