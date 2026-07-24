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

TEST_CASE("carve_cavities: lake_frac > 1 clamped to 1, no crash") {
  // bedrock ramp 0..1 over 50x50; lake_frac = 2.0f should clamp to 1.0f
  Field2D<float> bedrock(50, 50);
  for (int y = 0; y < 50; ++y)
    for (int x = 0; x < 50; ++x)
      bedrock.at(x, y) = (y * 50 + x) / 2499.0f;
  Field2D<float> B(50, 50, 100.0f);
  const auto B_before = B.data;
  const auto mask = carve_cavities(B, bedrock, 2.0f, 8.0f);  // lake_frac > 1
  double carved = 0.0;
  for (size_t i = 0; i < mask.data.size(); ++i) {
    if (mask.data[i]) carved += 1.0;
    const float cut = B_before[i] - B.data[i];
    REQUIRE(cut >= 0.0f);            // only lowers
    REQUIRE(cut <= 8.0f + 1e-4f);    // bounded by lake_depth_m
  }
  const double coverage = carved / mask.data.size();
  REQUIRE(coverage > 0.9);  // clamped to 1.0: nearly everything carved (all but max)
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
