// The two TEMPORARY stand-in derivations a heightfield-only map source needs:
// soil estimated from slope, and standing water proposed by flood-fill and
// vetoed by observation. Both are replaced by the simulation's own rasters.

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

// What a real bundle actually looks like, per the measurement in
// standing_water.hpp: a flat PLATE at the water surface, sitting slightly below
// a rim that rises around it. Not a basin -- the survey never saw the bottom.
struct Plate {
  Field2D<float> dtm;
  Field2D<uint8_t> cover;
};

Plate plate(int w, int h, int x0, int y0, int side, float surface,
            float rim_above) {
  Plate p;
  p.dtm = Field2D<float>(w, h, surface + rim_above);
  p.cover = all_cover(w, h, Cover::Grass);
  for (int y = y0; y < y0 + side; ++y) {
    for (int x = x0; x < x0 + side; ++x) {
      p.dtm.at(x, y) = surface;
      p.cover.at(x, y) = static_cast<uint8_t>(Cover::Water);
    }
  }
  return p;
}

}  // namespace

TEST_CASE("an observed plate becomes a flat surface over a carved bed",
          "[water]") {
  Plate p = plate(48, 48, 8, 8, 32, 100.0f, 1.5f);

  const auto out = derive_standing_water(p.dtm, p.cover, 1.0f);

  // EXACTLY flat, and at the plate's own elevation -- not flooded to the rim,
  // which is what a priority-flood would have done (+1.5 m here).
  REQUIRE(out.level.at(10, 10) == 100.0f);
  REQUIRE(out.level.at(30, 30) == 100.0f);
  // The bed is carved beneath it, so the contract can express a lake at all:
  // depth = max(0, level - bed) is positive.
  REQUIRE(out.bed.at(24, 24) < out.level.at(24, 24));
  // Dry outside: level == bed is the contract's dry texel, and the DTM is
  // untouched there.
  REQUIRE(out.level.at(0, 0) == out.bed.at(0, 0));
  REQUIRE(out.bed.at(0, 0) == p.dtm.at(0, 0));
}

TEST_CASE("the bed tapers to the shoreline rather than stepping", "[water]") {
  // A vertical wall at the waterline is visible wherever the water is clear
  // enough to see through, so depth must grow with distance from the shore.
  Plate p = plate(64, 64, 8, 8, 48, 100.0f, 1.5f);

  const auto out = derive_standing_water(p.dtm, p.cover, 1.0f);

  const float near_shore = out.level.at(9, 32) - out.bed.at(9, 32);
  const float mid = out.level.at(16, 32) - out.bed.at(16, 32);
  const float deep = out.level.at(32, 32) - out.bed.at(32, 32);
  REQUIRE(near_shore < mid);
  REQUIRE(mid < deep);
  REQUIRE_THAT(deep, WithinAbs(badlands::mapgen::kAssumedLakeDepthM, 1e-4f));
}

TEST_CASE("unobserved ground stays dry however hollow it is", "[water]") {
  // THE DEFECT THIS DESIGN EXISTS TO AVOID. Real 1 m LiDAR is full of drystone
  // walls, field banks and road embankments that genuinely dam water; a
  // terrain-only flood-fill ponds behind every one of them.
  Field2D<float> dtm(32, 32, 10.0f);
  for (int y = 8; y < 24; ++y) {
    for (int x = 8; x < 24; ++x) dtm.at(x, y) = 7.0f;  // a deep, closed hollow
  }
  const Field2D<uint8_t> cover = all_cover(32, 32, Cover::Grass);

  const auto out = derive_standing_water(dtm, cover, 1.0f);

  for (size_t i = 0; i < out.level.size(); ++i) {
    REQUIRE(out.level.data[i] == dtm.data[i]);
    REQUIRE(out.bed.data[i] == dtm.data[i]);
  }
}

TEST_CASE("a speck of misclassified land cover is not a lake", "[water]") {
  // At 10 m, one misclassified land-cover texel is 100 m^2 of evidence. It must
  // not carve a pond.
  Plate p = plate(32, 32, 5, 5, 2, 100.0f, 1.5f);  // 4 texels, below the floor

  const auto out = derive_standing_water(p.dtm, p.cover, 1.0f);

  for (size_t i = 0; i < out.bed.size(); ++i) {
    REQUIRE(out.bed.data[i] == p.dtm.data[i]);
  }
}

TEST_CASE("the shoreline follows the bed, not the land-cover staircase",
          "[water]") {
  // Land cover is 10 m against 1 m relief, so its edge is a staircase. Ground
  // below the plate's surface belongs to the lake even where cover missed it;
  // ground above it does not, even where cover claimed it.
  Plate p = plate(48, 48, 8, 8, 32, 100.0f, 1.5f);
  // A one-texel trench outside the observed mask, below the surface.
  for (int y = 8; y < 40; ++y) p.dtm.at(7, y) = 99.5f;
  // And a bank INSIDE the observed mask, above the surface.
  for (int y = 20; y < 24; ++y) p.dtm.at(20, y) = 101.0f;

  const auto out = derive_standing_water(p.dtm, p.cover, 1.0f);

  // Below the surface but outside the observed mask -> joins the lake.
  REQUIRE(out.level.at(7, 20) == 100.0f);
  REQUIRE(out.bed.at(7, 20) < out.level.at(7, 20));
  // Above the surface but inside the observed mask -> stays dry ground. Carving
  // it would dig a hole where the survey plainly saw a bank.
  REQUIRE(out.bed.at(20, 21) == 101.0f);
  REQUIRE(out.level.at(20, 21) == out.bed.at(20, 21));
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

TEST_CASE("a degenerate water request returns empty fields", "[water]") {
  const auto out = derive_standing_water(Field2D<float>(), Field2D<uint8_t>(), 1.0f);
  REQUIRE(out.level.size() == 0);
  REQUIRE(out.bed.size() == 0);
}
