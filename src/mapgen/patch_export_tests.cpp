// Tests for the image encodings of PatchData: the height/water channel packing,
// the biome palette, and the hillshade that stage-2 iteration is judged from.
//
// These link badlands_patch_providers and NOTHING else -- no `assets` crate, no
// engine. That is the point of keeping the encoders buffer-returning, so this
// target is also the test that the split held.

#include <catch_amalgamated.hpp>

#include <cmath>
#include <cstdint>
#include <vector>

#include "mapgen/biomes.hpp"
#include "mapgen/patch_export.hpp"

using namespace badlands::mapgen;

namespace {

// One RGBA texel out of a tightly packed buffer.
struct Texel {
  uint8_t r, g, b, a;
};

Texel texel_at(const std::vector<uint8_t>& rgba, int w, int x, int y) {
  const size_t i = (static_cast<size_t>(y) * w + x) * 4;
  REQUIRE(i + 3 < rgba.size());
  return {rgba[i], rgba[i + 1], rgba[i + 2], rgba[i + 3]};
}

// A field whose value depends only on x, so a test can state "this column".
Field2D<float> column_field(int n, float lo, float hi) {
  Field2D<float> f(n, n, 0.0f);
  for (int y = 0; y < n; ++y)
    for (int x = 0; x < n; ++x)
      f.at(x, y) = lo + (hi - lo) * (static_cast<float>(x) / (n - 1));
  return f;
}

}  // namespace

TEST_CASE("height encodes to a linear grey ramp across the range", "[export]") {
  const int n = 5;
  const Field2D<float> height = column_field(n, 100.0f, 140.0f);
  const Field2D<float> dry(n, n, 0.0f);

  const std::vector<uint8_t> rgba =
      encode_height_water_rgba(height, dry, {100.0f, 140.0f}, 0.0f);
  REQUIRE(rgba.size() == static_cast<size_t>(n) * n * 4);

  // The range endpoints anchor the mapping; everything else is linear between.
  CHECK(texel_at(rgba, n, 0, 0).r == 0);
  CHECK(texel_at(rgba, n, n - 1, 0).r == 255);
  for (int x = 0; x < n; ++x) {
    const Texel t = texel_at(rgba, n, x, 2);
    const int want = static_cast<int>(std::lround(255.0 * x / (n - 1)));
    CHECK(static_cast<int>(t.r) == want);
    CHECK(t.g == t.r);  // grey: R and G carry the same code
    CHECK(t.a == 255);
  }
}

TEST_CASE("height outside the range clamps instead of wrapping", "[export]") {
  const int n = 3;
  Field2D<float> height(n, n, 0.0f);
  height.at(0, 0) = -500.0f;  // far below lo
  height.at(1, 0) = 120.0f;   // inside
  height.at(2, 0) = 9000.0f;  // far above hi
  const Field2D<float> dry(n, n, 0.0f);

  const std::vector<uint8_t> rgba =
      encode_height_water_rgba(height, dry, {100.0f, 140.0f}, 0.0f);

  CHECK(texel_at(rgba, n, 0, 0).r == 0);
  CHECK(texel_at(rgba, n, 2, 0).r == 255);
  CHECK(texel_at(rgba, n, 1, 0).r > 0);
  CHECK(texel_at(rgba, n, 1, 0).r < 255);
}

TEST_CASE("a degenerate range yields flat mid-grey, not a divide by zero",
          "[export]") {
  const int n = 4;
  const Field2D<float> height = column_field(n, 10.0f, 90.0f);
  const Field2D<float> dry(n, n, 0.0f);

  const std::vector<uint8_t> rgba =
      encode_height_water_rgba(height, dry, {50.0f, 50.0f}, 0.0f);
  REQUIRE(rgba.size() == static_cast<size_t>(n) * n * 4);
  for (int x = 0; x < n; ++x) {
    const Texel t = texel_at(rgba, n, x, 0);
    CHECK(static_cast<int>(t.r) == 128);
    CHECK(t.g == t.r);
    CHECK(t.b == t.r);
  }
}

TEST_CASE("the watermap rides in the blue channel and decodes as B - R",
          "[export]") {
  // Flat low ground so the height code stays far from saturation: the recovery
  // claim is what the whole layering is for, and it must not rot.
  const int n = 4;
  const Field2D<float> height(n, n, 100.0f);  // -> code 0 over [100, 140]
  Field2D<float> water(n, n, 0.0f);
  water.at(1, 1) = 2.0f;  // 2 m deep, over a 8 m water range -> code 64
  water.at(2, 1) = 4.0f;  // 4 m deep                          -> code 128

  const std::vector<uint8_t> rgba =
      encode_height_water_rgba(height, water, {100.0f, 140.0f}, 8.0f);

  const Texel dry = texel_at(rgba, n, 0, 0);
  CHECK(dry.b == dry.r);  // dry ground is NEUTRAL grey: no blue at all

  const Texel shallow = texel_at(rgba, n, 1, 1);
  const Texel deep = texel_at(rgba, n, 2, 1);
  CHECK(shallow.b > shallow.r);
  CHECK(deep.b > shallow.b);
  CHECK(static_cast<int>(shallow.b - shallow.r) == 64);
  CHECK(static_cast<int>(deep.b - deep.r) == 128);
  // The water never touches the grey channels -- height stays readable under it.
  CHECK(shallow.r == dry.r);
  CHECK(deep.g == dry.g);
}

TEST_CASE("deep water on high ground saturates blue without wrapping",
          "[export]") {
  const int n = 2;
  const Field2D<float> height(n, n, 140.0f);  // -> code 255
  Field2D<float> water(n, n, 0.0f);
  water.at(0, 0) = 8.0f;  // full-scale depth on a full-scale height

  const std::vector<uint8_t> rgba =
      encode_height_water_rgba(height, water, {100.0f, 140.0f}, 8.0f);
  const Texel t = texel_at(rgba, n, 0, 0);
  CHECK(t.r == 255);
  CHECK(t.b == 255);  // clamped, NOT wrapped to a dark blue
}

TEST_CASE("a non-positive water scale leaves the image purely grey",
          "[export]") {
  const int n = 3;
  const Field2D<float> height = column_field(n, 0.0f, 10.0f);
  Field2D<float> water(n, n, 3.0f);  // wet everywhere, but no scale to map it

  const std::vector<uint8_t> rgba =
      encode_height_water_rgba(height, water, {0.0f, 10.0f}, 0.0f);
  for (int x = 0; x < n; ++x) {
    const Texel t = texel_at(rgba, n, x, 0);
    CHECK(t.b == t.r);
  }
}

TEST_CASE("a mismatched water field is treated as dry, not read out of bounds",
          "[export]") {
  const int n = 4;
  const Field2D<float> height = column_field(n, 0.0f, 10.0f);
  const Field2D<float> wrong_size(2, 2, 5.0f);

  const std::vector<uint8_t> rgba =
      encode_height_water_rgba(height, wrong_size, {0.0f, 10.0f}, 8.0f);
  REQUIRE(rgba.size() == static_cast<size_t>(n) * n * 4);
  for (int x = 0; x < n; ++x) {
    const Texel t = texel_at(rgba, n, x, 0);
    CHECK(t.b == t.r);
  }
}

TEST_CASE("an empty field encodes to an empty buffer", "[export]") {
  const Field2D<float> empty;
  CHECK(encode_height_water_rgba(empty, empty, {0.0f, 1.0f}, 1.0f).empty());
  CHECK(encode_biome_rgba(Field2D<uint8_t>{}).empty());
}

TEST_CASE("biome ids encode to the shared palette byte for byte", "[export]") {
  Field2D<uint8_t> biome(kBiomeCount, 1, 0);
  for (int i = 0; i < kBiomeCount; ++i)
    biome.at(i, 0) = static_cast<uint8_t>(i);

  const std::vector<uint8_t> rgba = encode_biome_rgba(biome);
  REQUIRE(rgba.size() == static_cast<size_t>(kBiomeCount) * 4);
  for (int i = 0; i < kBiomeCount; ++i) {
    const Texel t = texel_at(rgba, kBiomeCount, i, 0);
    const Rgb want = kBiomePalette[i];
    CHECK(t.r == want.r);
    CHECK(t.g == want.g);
    CHECK(t.b == want.b);
    CHECK(t.a == 255);
  }
}

TEST_CASE("an out-of-range biome id does not read past the palette",
          "[export]") {
  Field2D<uint8_t> biome(2, 1, 0);
  biome.at(0, 0) = static_cast<uint8_t>(kBiomeCount);  // one past the end
  biome.at(1, 0) = 200;

  const std::vector<uint8_t> rgba = encode_biome_rgba(biome);
  for (int x = 0; x < 2; ++x) {
    const Texel t = texel_at(rgba, 2, x, 0);
    CHECK(t.r == 0);
    CHECK(t.g == 0);
    CHECK(t.b == 0);
    CHECK(t.a == 255);
  }
}

namespace {

// A dry patch with the given height field: enough for the hillshade, which reads
// height, water_depth, texel_m and rivers.
PatchData dry_patch(Field2D<float> height, float texel_m = 1.0f) {
  PatchData p;
  p.texel_m = texel_m;
  p.water_depth = Field2D<float>(height.width, height.height, 0.0f);
  p.height = std::move(height);
  return p;
}

}  // namespace

TEST_CASE("a flat field hillshades to a single uniform tone", "[export]") {
  const int n = 6;
  const PatchData p = dry_patch(Field2D<float>(n, n, 42.0f));

  const std::vector<uint8_t> rgba = encode_hillshade_rgba(p);
  REQUIRE(rgba.size() == static_cast<size_t>(n) * n * 4);
  const Texel first = texel_at(rgba, n, 1, 1);
  for (int y = 1; y < n - 1; ++y)
    for (int x = 1; x < n - 1; ++x) {
      const Texel t = texel_at(rgba, n, x, y);
      CHECK(t.r == first.r);
      CHECK(t.g == first.g);
      CHECK(t.b == first.b);
    }
}

TEST_CASE("a slope facing the sun is brighter than one facing away",
          "[export]") {
  // The sun sits in the north-west. A field that RISES with x has a surface
  // normal tilted west (n_x = -dz/dx < 0), so it faces the sun; one that falls
  // with x faces east, away from it.
  const int n = 8;
  const PatchData facing = dry_patch(column_field(n, 0.0f, 20.0f));
  const PatchData turned_away = dry_patch(column_field(n, 20.0f, 0.0f));

  const std::vector<uint8_t> lit = encode_hillshade_rgba(facing);
  const std::vector<uint8_t> dark = encode_hillshade_rgba(turned_away);
  CHECK(texel_at(lit, n, 4, 4).r > texel_at(dark, n, 4, 4).r);
}

TEST_CASE("a wet texel is water-tinted where its dry neighbour is not",
          "[export]") {
  const int n = 5;
  PatchData p = dry_patch(Field2D<float>(n, n, 10.0f));
  p.water_depth.at(2, 2) = 3.0f;

  const std::vector<uint8_t> rgba = encode_hillshade_rgba(p);
  const Texel wet = texel_at(rgba, n, 2, 2);
  const Texel dry = texel_at(rgba, n, 1, 2);
  // Blue DOMINANCE is the claim, not absolute blue: lit ground is bright in
  // every channel, so its blue byte can exceed a dark water surface's.
  CHECK(wet.b > wet.r);
  CHECK(dry.b <= dry.r);
  CHECK(wet.b - wet.r > dry.b - dry.r);
}

TEST_CASE("deeper water reads darker than shallow water", "[export]") {
  const int n = 4;
  PatchData p = dry_patch(Field2D<float>(n, n, 10.0f));
  p.water_depth.at(1, 1) = 0.5f;
  p.water_depth.at(2, 1) = 12.0f;

  const std::vector<uint8_t> rgba = encode_hillshade_rgba(p);
  const Texel shallow = texel_at(rgba, n, 1, 1);
  const Texel deep = texel_at(rgba, n, 2, 1);
  const int shallow_sum = shallow.r + shallow.g + shallow.b;
  const int deep_sum = deep.r + deep.g + deep.b;
  CHECK(deep_sum < shallow_sum);
}

TEST_CASE("a river edge paints its centreline onto the hillshade", "[export]") {
  const int n = 9;
  PatchData p = dry_patch(Field2D<float>(n, n, 10.0f), 1.0f);
  // One straight reach across the middle row, in patch-local metres.
  RiverEdge e;
  e.from = 0;
  e.to = 1;
  e.points_m = {glm::vec2(1.0f, 4.0f), glm::vec2(7.0f, 4.0f)};
  e.discharge_m3_s.assign(e.points_m.size(), 1.0f);
  e.width_m.assign(e.points_m.size(), 1.0f);
  p.rivers.edges.push_back(e);

  const std::vector<uint8_t> rgba = encode_hillshade_rgba(p);
  const Texel on = texel_at(rgba, n, 4, 4);
  const Texel off = texel_at(rgba, n, 4, 1);
  CHECK(on.b > on.r);    // the channel reads as water
  CHECK(on.b > off.b);   // and its surroundings do not
}

TEST_CASE("a river outside the patch does not paint anything", "[export]") {
  const int n = 6;
  PatchData p = dry_patch(Field2D<float>(n, n, 10.0f), 1.0f);
  RiverEdge e;
  e.points_m = {glm::vec2(-40.0f, -40.0f), glm::vec2(-30.0f, -35.0f)};
  p.rivers.edges.push_back(e);

  const std::vector<uint8_t> with_river = encode_hillshade_rgba(p);
  p.rivers.edges.clear();
  const std::vector<uint8_t> without = encode_hillshade_rgba(p);
  CHECK(with_river == without);
}
