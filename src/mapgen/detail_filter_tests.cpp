#include <catch_amalgamated.hpp>
#include <cmath>
#include "mapgen/detail_filter.hpp"
#include "mapgen/erosion.hpp"

using namespace badlands::mapgen;

namespace {
Field2D<float> slope_field(int n, float dz) {  // plane rising dz per texel in x
  Field2D<float> f(n, n);
  for (int y = 0; y < n; ++y)
    for (int x = 0; x < n; ++x) f.at(x, y) = x * dz;
  return f;
}
}  // namespace

TEST_CASE("gully_detail_delta: bounded, carve-only, deterministic") {
  const auto base = slope_field(64, 1.0f);  // steep: 45 degrees
  const Field2D<float> dry(64, 64, 0.0f);
  ErosionParams p;
  const auto d1 = gully_detail_delta(base, dry, 1.0f, 9, p);
  const auto d2 = gully_detail_delta(base, dry, 1.0f, 9, p);
  REQUIRE(d1.data == d2.data);
  // amplitude bound: sum of octave amplitudes (a0 * (1 + 1/2 + 1/4 + 1/8))
  const float bound = p.detail_amplitude_m * 2.0f;
  bool any_nonzero = false;
  for (float v : d1.data) {
    REQUIRE(v <= 0.0f);
    REQUIRE(v >= -bound);
    if (v < 0.0f) any_nonzero = true;
  }
  REQUIRE(any_nonzero);  // steep slope MUST get gullies
}

TEST_CASE("gully_detail_delta: zero wavelength is a no-op guard, not a NaN") {
  const auto base = slope_field(64, 1.0f);  // steep slope, would otherwise carve
  ErosionParams p;
  p.detail_wavelength_m = 0.0f;
  const auto d = gully_detail_delta(base, Field2D<float>(64, 64, 0.0f), 1.0f, 9, p);
  for (float v : d.data) REQUIRE(v == 0.0f);
}

TEST_CASE("gully_detail_delta: flat base gets nothing; water gets nothing") {
  const Field2D<float> flat(64, 64, 5.0f);
  Field2D<float> water(64, 64, 0.0f);
  ErosionParams p;
  const auto d = gully_detail_delta(flat, water, 1.0f, 9, p);
  for (float v : d.data) REQUIRE(v == 0.0f);  // slope mask kills flats

  const auto base = slope_field(64, 1.0f);
  for (int y = 20; y < 40; ++y)
    for (int x = 20; x < 40; ++x) water.at(x, y) = 2.0f;  // a lake patch
  const auto dw = gully_detail_delta(base, water, 1.0f, 9, p);
  for (int y = 20; y < 40; ++y)
    for (int x = 20; x < 40; ++x) REQUIRE(dw.at(x, y) == 0.0f);
}
