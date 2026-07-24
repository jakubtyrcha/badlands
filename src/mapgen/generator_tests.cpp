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
