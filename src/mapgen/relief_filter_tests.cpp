// Tests for the stage-2 relief chain's point-sampling and detail filter.
//
// Same hard rule as coarse_world_patch_source_tests.cpp: no test here may run
// the coarse sim; every fixture is hand-written by the test.

#include <catch_amalgamated.hpp>

#include <cmath>

#include "mapgen/biomes.hpp"
#include "mapgen/cubic_sample.hpp"
#include "mapgen/field2d.hpp"
#include "mapgen/relief_filter.hpp"

using namespace badlands::mapgen;

namespace {

constexpr float kSrcTexelM = 16.0f;  // the production coarse cell size

// Deterministic pseudo-random field values without <random>: a small integer
// hash, mapped into [0, amp).
float hash01(int x, int y) {
  uint32_t h = static_cast<uint32_t>(x) * 0x9E3779B9u ^
               static_cast<uint32_t>(y) * 0x85EBCA6Bu;
  h ^= h >> 16;
  h *= 0x7FEB352Du;
  h ^= h >> 15;
  return static_cast<float>(h & 0xFFFFFFu) / static_cast<float>(0x1000000);
}

Field2D<float> hashed_field(int n, float amp) {
  Field2D<float> f(n, n);
  for (int y = 0; y < n; ++y)
    for (int x = 0; x < n; ++x) f.at(x, y) = amp * hash01(x, y);
  return f;
}

}  // namespace

TEST_CASE("cubic_sample reproduces source nodes exactly", "[relief]") {
  // Catmull-Rom is interpolating: weight 1 on the exact sample, 0 on every
  // other integer offset. At a source node's world position the sample IS
  // the raster value -- interior nodes, clear of the clamped border stencil.
  const Field2D<float> f = hashed_field(16, 100.0f);
  for (int y = 2; y < 14; ++y) {
    for (int x = 2; x < 14; ++x) {
      const CubicSample s = cubic_sample(
          f, kSrcTexelM,
          {static_cast<double>(x) * kSrcTexelM,
           static_cast<double>(y) * kSrcTexelM});
      REQUIRE(s.value == Catch::Approx(f.at(x, y)).margin(1e-4));
    }
  }
}

TEST_CASE("cubic_sample reproduces a linear ramp and its gradient", "[relief]") {
  // Every Mitchell-Netravali cubic reproduces a linear ramp to machine
  // epsilon (the reason Catmull-Rom was chosen over Lanczos in the resample).
  // A planar hillside is a ramp, so both value and gradient must be exact at
  // ARBITRARY interior positions, not just nodes.
  const float ax = 0.35f, ay = -0.2f;
  const int n = 16;
  Field2D<float> f(n, n);
  for (int y = 0; y < n; ++y)
    for (int x = 0; x < n; ++x)
      f.at(x, y) = ax * (x * kSrcTexelM) + ay * (y * kSrcTexelM);

  const double probes[][2] = {{40.0, 70.0}, {52.3, 99.1}, {100.7, 37.2},
                              {77.77, 111.11}};
  for (const auto& p : probes) {
    const CubicSample s = cubic_sample(f, kSrcTexelM, {p[0], p[1]});
    REQUIRE(s.value ==
            Catch::Approx(ax * p[0] + ay * p[1]).margin(1e-3));
    REQUIRE(s.grad.x == Catch::Approx(ax).margin(1e-5));
    REQUIRE(s.grad.y == Catch::Approx(ay).margin(1e-5));
  }
}

TEST_CASE("cubic_sample is exact on constants at the clamped border",
          "[relief]") {
  // Border renormalisation keeps the weights a partition of unity, so a
  // constant field must come back exactly constant with zero slope even
  // where the stencil clamps -- including positions outside the source.
  const int n = 8;
  Field2D<float> f(n, n, 7.25f);
  const double probes[][2] = {{0.0, 0.0},   {3.0, 3.0},    {-10.0, 50.0},
                              {119.0, 5.0}, {130.0, 130.0}, {60.0, -4.0}};
  for (const auto& p : probes) {
    const CubicSample s = cubic_sample(f, kSrcTexelM, {p[0], p[1]});
    REQUIRE(s.value == Catch::Approx(7.25f).margin(1e-5));
    REQUIRE(s.grad.x == Catch::Approx(0.0f).margin(1e-5));
    REQUIRE(s.grad.y == Catch::Approx(0.0f).margin(1e-5));
  }
}

TEST_CASE("cubic_sample gradient matches a finite difference off-node",
          "[relief]") {
  // On a smooth non-linear field the analytic gradient must agree with a
  // central difference of the sampler itself -- this pins the world-metre
  // scaling (a d/du derivative missing the 1/texel factor is 16x off).
  const Field2D<float> f = hashed_field(16, 50.0f);
  const double h = 1e-3;
  const double probes[][2] = {{50.0, 50.0}, {83.4, 61.9}, {121.2, 140.6}};
  for (const auto& p : probes) {
    const CubicSample s = cubic_sample(f, kSrcTexelM, {p[0], p[1]});
    const float fx1 = cubic_sample(f, kSrcTexelM, {p[0] + h, p[1]}).value;
    const float fx0 = cubic_sample(f, kSrcTexelM, {p[0] - h, p[1]}).value;
    const float fy1 = cubic_sample(f, kSrcTexelM, {p[0], p[1] + h}).value;
    const float fy0 = cubic_sample(f, kSrcTexelM, {p[0], p[1] - h}).value;
    REQUIRE(s.grad.x ==
            Catch::Approx((fx1 - fx0) / (2 * h)).margin(1e-3));
    REQUIRE(s.grad.y ==
            Catch::Approx((fy1 - fy0) / (2 * h)).margin(1e-3));
  }
}

// ---------------------------------------------------------------------------
// The relief filter proper.
// ---------------------------------------------------------------------------

namespace {

// A coarse fixture the filter reads: every raster hand-written. Owns storage;
// ctx() hands out the pointer view sample_relief_delta wants.
struct CoarseFixture {
  Field2D<float> bed, soil, depth;
  Field2D<uint8_t> biome;
  uint32_t seed = 1;

  CoarseFixture(int n, float bed_slope_m_per_m, float soil_m, Biome b)
      : bed(n, n),
        soil(n, n, soil_m),
        depth(n, n, 0.0f),
        biome(n, n, static_cast<uint8_t>(b)) {
    for (int y = 0; y < n; ++y)
      for (int x = 0; x < n; ++x)
        bed.at(x, y) = bed_slope_m_per_m * (x * kSrcTexelM);
  }

  ReliefContext ctx() const {
    return {&bed, &soil, &biome, &depth, kSrcTexelM, seed};
  }
};

}  // namespace

TEST_CASE("relief adds bounded nonzero detail on thin-soil steep ground",
          "[relief]") {
  // A 31-degree bare-rock mountainside is the filter's home turf: at 1 m
  // output texels it must actually do something, and never more than a few
  // metres -- detail is high-frequency, not landforms.
  const CoarseFixture fx(32, 0.6f, 0.2f, Biome::Mountain);
  const ReliefContext ctx = fx.ctx();
  float max_abs = 0.0f;
  for (int i = 0; i < 400; ++i) {
    const double x = 128.0 + (i % 20) * 12.7, y = 128.0 + (i / 20) * 12.7;
    const ReliefSample s = sample_relief_delta(ctx, {x, y}, 1.0f);
    max_abs = std::max(max_abs, std::fabs(s.delta_m));
    REQUIRE(std::fabs(s.delta_m) <= 4.0f);
  }
  REQUIRE(max_abs > 0.05f);
}

TEST_CASE("relief fades out on deep flat soil", "[relief]") {
  // Deep soil on flat ground is where the physical fade zeroes the filter --
  // farmland stays smooth however close you look.
  const CoarseFixture fx(32, 0.0f, 8.0f, Biome::Plains);
  const ReliefContext ctx = fx.ctx();
  for (int i = 0; i < 100; ++i) {
    const double x = 128.0 + (i % 10) * 23.7, y = 128.0 + (i / 10) * 23.7;
    const ReliefSample s = sample_relief_delta(ctx, {x, y}, 1.0f);
    REQUIRE(std::fabs(s.delta_m) < 1e-3f);
  }
}

TEST_CASE("relief is exactly zero on standing water", "[relief]") {
  // v1 water handling IS the mask: no gully may touch a lake bed or tilt a
  // shoreline. Exactly zero, not merely small.
  CoarseFixture fx(32, 0.6f, 0.2f, Biome::Mountain);
  for (int y = 10; y < 22; ++y)
    for (int x = 10; x < 22; ++x) fx.depth.at(x, y) = 3.0f;
  const ReliefContext ctx = fx.ctx();
  for (int y = 12; y < 20; ++y) {
    for (int x = 12; x < 20; ++x) {
      const ReliefSample s = sample_relief_delta(
          ctx, {x * kSrcTexelM, y * kSrcTexelM}, 1.0f);
      REQUIRE(s.delta_m == 0.0f);
    }
  }
}

TEST_CASE("relief vanishes at coarse output density", "[relief]") {
  // The octave band lives below the source Nyquist (32 m); a 16 m output
  // texel cannot represent any of it, so the filter must contribute exactly
  // nothing -- Box/Crop-branch requests stay bit-identical to the resample.
  const CoarseFixture fx(32, 0.6f, 0.2f, Biome::Mountain);
  const ReliefContext ctx = fx.ctx();
  for (int i = 0; i < 100; ++i) {
    const double x = 128.0 + (i % 10) * 23.7, y = 128.0 + (i / 10) * 23.7;
    const ReliefSample s = sample_relief_delta(ctx, {x, y}, kSrcTexelM);
    REQUIRE(s.delta_m == 0.0f);
  }
}

TEST_CASE("relief detail is seed-live and biome-live", "[relief]") {
  // protogen's hard-won lesson, inverted: a knob that exists must provably
  // move the output. The world seed and the per-biome style tables are the
  // only two inputs beyond geometry -- each must matter.
  CoarseFixture fx(32, 0.6f, 0.2f, Biome::Mountain);
  const auto field_hash = [&](const ReliefContext& c) {
    double acc = 0.0;
    for (int i = 0; i < 200; ++i) {
      const double x = 128.0 + (i % 20) * 11.3, y = 128.0 + (i / 20) * 21.7;
      acc += std::fabs(sample_relief_delta(c, {x, y}, 1.0f).delta_m) * (i + 1);
    }
    return acc;
  };
  const double mountain_s1 = field_hash(fx.ctx());
  fx.seed = 2;
  const double mountain_s2 = field_hash(fx.ctx());
  REQUIRE(mountain_s1 != mountain_s2);

  fx.seed = 1;
  fx.biome = Field2D<uint8_t>(32, 32, static_cast<uint8_t>(Biome::Hills));
  const double hills = field_hash(fx.ctx());
  fx.biome = Field2D<uint8_t>(32, 32, static_cast<uint8_t>(Biome::Plains));
  const double plains = field_hash(fx.ctx());
  REQUIRE(mountain_s1 != hills);
  REQUIRE(hills != plains);
}

TEST_CASE("apply_relief matches the point function and is origin-invariant",
          "[relief]") {
  // The raster driver is only a tiled loop over the pure function; the same
  // world position must get the same detail whatever the patch origin -- the
  // seam guarantee between adjacent patches.
  const CoarseFixture fx(32, 0.6f, 0.2f, Biome::Mountain);
  const ReliefContext ctx = fx.ctx();
  const int n = 64;
  Field2D<float> a(n, n, 0.0f), b(n, n, 0.0f);
  apply_relief(ctx, {128.0, 128.0}, 1.0f, a);
  apply_relief(ctx, {160.0, 144.0}, 1.0f, b);  // overlaps a by 32 x 48 m
  for (int y = 0; y < 48; ++y) {
    for (int x = 0; x < 32; ++x) {
      REQUIRE(a.at(x + 32, y + 16) == b.at(x, y));
    }
  }
  const ReliefSample s =
      sample_relief_delta(ctx, {128.0 + 7.0, 128.0 + 11.0}, 1.0f);
  REQUIRE(a.at(7, 11) == s.delta_m);
}

TEST_CASE("relief gradient matches a finite difference", "[relief]") {
  // The stacked octaves feed later octaves their accumulated slope, so a
  // wrong analytic gradient corrupts orientation silently. Pin it to a
  // central difference of the function itself.
  const CoarseFixture fx(32, 0.6f, 0.2f, Biome::Mountain);
  const ReliefContext ctx = fx.ctx();
  const double h = 1e-3;
  int checked = 0;
  for (int i = 0; i < 40 && checked < 12; ++i) {
    const double x = 133.0 + (i % 8) * 17.9, y = 141.0 + (i / 8) * 13.3;
    const ReliefSample s = sample_relief_delta(ctx, {x, y}, 1.0f);
    if (std::fabs(s.delta_m) < 1e-4f) continue;  // dead zones prove nothing
    const float fx1 = sample_relief_delta(ctx, {x + h, y}, 1.0f).delta_m;
    const float fx0 = sample_relief_delta(ctx, {x - h, y}, 1.0f).delta_m;
    const float fy1 = sample_relief_delta(ctx, {x, y + h}, 1.0f).delta_m;
    const float fy0 = sample_relief_delta(ctx, {x, y - h}, 1.0f).delta_m;
    CHECK(s.grad.x == Catch::Approx((fx1 - fx0) / (2 * h)).margin(2e-2));
    CHECK(s.grad.y == Catch::Approx((fy1 - fy0) / (2 * h)).margin(2e-2));
    ++checked;
  }
  REQUIRE(checked >= 8);
}
