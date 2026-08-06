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

  float texel_m = kSrcTexelM;

  CoarseFixture(int n, float bed_slope_m_per_m, float soil_m, Biome b,
                float texel = kSrcTexelM)
      : bed(n, n),
        soil(n, n, soil_m),
        depth(n, n, 0.0f),
        biome(n, n, static_cast<uint8_t>(b)),
        texel_m(texel) {
    for (int y = 0; y < n; ++y)
      for (int x = 0; x < n; ++x)
        bed.at(x, y) = bed_slope_m_per_m * (x * texel_m);
  }

  ReliefContext ctx() const {
    return {&bed, &soil, &biome, &depth, texel_m, seed};
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

TEST_CASE("relief gradient is live, finite and bounded", "[relief]") {
  // The reported gradient is APPROXIMATE by design (wave terms only, no
  // mask/target spatial derivatives -- see relief_filter.hpp), so an FD
  // equality pin would be wrong. What must still hold: it is finite, it is
  // nonzero where detail is carved, and it never exceeds the analytic bound
  // sum_k(amp_k * gullyWeight * 2*pi / lambda_k) of the terms it carries.
  const CoarseFixture fx(32, 0.6f, 0.2f, Biome::Mountain);
  const ReliefContext ctx = fx.ctx();
  float max_grad = 0.0f;
  for (int i = 0; i < 200; ++i) {
    const double x = 133.0 + (i % 20) * 11.9, y = 141.0 + (i / 20) * 13.3;
    const ReliefSample s = sample_relief_delta(ctx, {x, y}, 1.0f);
    REQUIRE(std::isfinite(s.grad.x));
    REQUIRE(std::isfinite(s.grad.y));
    const float g = std::sqrt(s.grad.x * s.grad.x + s.grad.y * s.grad.y);
    REQUIRE(g <= 1.0f);
    max_grad = std::max(max_grad, g);
  }
  REQUIRE(max_grad > 0.01f);
}

TEST_CASE("relief strength feathers down approaching a shoreline", "[relief]") {
  // Between open ground and the exact-zero wet mask sits the feather: a dry
  // texel whose coarse neighborhood is partly wet must carry LESS detail
  // than the same texel in an identical world without the lake -- and a
  // texel with no wet neighborhood at all must carry exactly the same.
  CoarseFixture dry_fx(32, 0.6f, 0.2f, Biome::Mountain);
  CoarseFixture lake_fx(32, 0.6f, 0.2f, Biome::Mountain);
  for (int y = 10; y < 22; ++y)
    for (int x = 10; x < 22; ++x) lake_fx.depth.at(x, y) = 3.0f;
  const ReliefContext dry = dry_fx.ctx();
  const ReliefContext lake = lake_fx.ctx();

  // The dry ring half a coarse cell outside the lake: wet fraction in (0,1).
  double rms_dry = 0.0, rms_lake = 0.0;
  int count = 0;
  for (int i = 0; i < 32; ++i) {
    const double along = 10.0 * kSrcTexelM + i * 3.7;
    const glm::dvec2 p{along, 9.4 * kSrcTexelM};  // just north of the lake
    const float a = sample_relief_delta(dry, p, 1.0f).delta_m;
    const float b = sample_relief_delta(lake, p, 1.0f).delta_m;
    rms_dry += a * a;
    rms_lake += b * b;
    ++count;
  }
  REQUIRE(rms_dry > 0.0);
  REQUIRE(rms_lake < rms_dry);

  // Far from the lake (no wet cell within the bilinear footprint): identical.
  const glm::dvec2 far{4.0 * kSrcTexelM + 3.3, 4.0 * kSrcTexelM + 7.7};
  REQUIRE(sample_relief_delta(dry, far, 1.0f).delta_m ==
          sample_relief_delta(lake, far, 1.0f).delta_m);
}

TEST_CASE("relief detail is stable across output resolutions", "[relief]") {
  // 0.5 m and 1 m requests reveal the same four octaves, so the fine field
  // box-downsampled must agree with the coarse field up to the profile's
  // curvature inside one texel -- resolution sharpens detail, never moves it.
  const CoarseFixture fx(32, 0.6f, 0.2f, Biome::Mountain);
  const ReliefContext ctx = fx.ctx();
  const glm::dvec2 origin{128.0, 128.0};
  Field2D<float> fine(128, 128, 0.0f), coarse(64, 64, 0.0f);
  apply_relief(ctx, origin, 0.5f, fine);
  apply_relief(ctx, origin, 1.0f, coarse);

  float max_abs = 0.0f, worst = 0.0f;
  for (int y = 2; y < 62; ++y) {
    for (int x = 2; x < 62; ++x) {
      // The fine field's node at (2x, 2y) IS the coarse node's world
      // position; average the 2x2 block around it for a fair box compare.
      const float avg = 0.25f * (fine.at(2 * x, 2 * y) + fine.at(2 * x + 1, 2 * y) +
                                 fine.at(2 * x, 2 * y + 1) +
                                 fine.at(2 * x + 1, 2 * y + 1));
      worst = std::max(worst, std::fabs(avg - coarse.at(x, y)));
      max_abs = std::max(max_abs, std::fabs(coarse.at(x, y)));
    }
  }
  REQUIRE(max_abs > 0.05f);  // the comparison must not be vacuous
  REQUIRE(worst < 0.25f);
}

TEST_CASE("relief octaves respect the SOURCE Nyquist too", "[relief]") {
  // Wavelengths the coarse grid could represent belong to stage 1 -- adding
  // them back would overprint simulated erosion. At an 8 m source, stage 1
  // owns everything >= 16 m, so requesting output AT source density leaves
  // no octave between the two Nyquists and the delta must be exactly zero,
  // while a fine request still gets the sub-16 m octaves.
  const CoarseFixture fx(32, 0.6f, 0.2f, Biome::Mountain, 8.0f);
  const ReliefContext ctx = fx.ctx();
  for (int i = 0; i < 50; ++i) {
    const double x = 64.0 + (i % 10) * 11.3, y = 64.0 + (i / 10) * 13.7;
    REQUIRE(sample_relief_delta(ctx, {x, y}, 8.0f).delta_m == 0.0f);
  }
  float max_abs = 0.0f;
  for (int i = 0; i < 100; ++i) {
    const double x = 64.0 + (i % 10) * 11.3, y = 64.0 + (i / 10) * 13.7;
    max_abs = std::max(
        max_abs, std::fabs(sample_relief_delta(ctx, {x, y}, 1.0f).delta_m));
  }
  REQUIRE(max_abs > 0.02f);
}
