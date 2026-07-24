// The bedrock+quantile map generator — mechanisms, not looks: determinism,
// structural area fractions, cutoff/classification logic, and
// resolution-independent world sampling. Ridge/clump "look" is judged by eye
// via --preview-image-only, deliberately not pinned here.

#include <catch_amalgamated.hpp>

#include <cmath>
#include <cstdint>

#include "mapgen/biomes.hpp"
#include "mapgen/generator.hpp"

using badlands::mapgen::Biome;
using badlands::mapgen::BiomeCutoffs;
using badlands::mapgen::classify_biomes;
using badlands::mapgen::compute_cutoffs;
using badlands::mapgen::distance_to_plains;
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
  REQUIRE(a.heightmap.data == b.heightmap.data);
}

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

TEST_CASE("generate_map: degenerate resolution yields empty artifacts, no throw") {
  MapGenParams p;
  p.resolution = {0, 64};
  REQUIRE(generate_map(p).bedrock.size() == 0);
  p.resolution = {-1, 64};
  const auto a = generate_map(p);
  REQUIRE(a.bedrock.size() == 0);
  REQUIRE(a.biome.size() == 0);
  REQUIRE(a.heightmap.size() == 0);
}

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
