// Tests for the stage-2 relief chain's point-sampling and detail filter.
//
// Same hard rule as coarse_world_patch_source_tests.cpp: no test here may run
// the coarse sim; every fixture is hand-written by the test.

#include <catch_amalgamated.hpp>

#include <cmath>

#include "mapgen/cubic_sample.hpp"
#include "mapgen/field2d.hpp"

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
