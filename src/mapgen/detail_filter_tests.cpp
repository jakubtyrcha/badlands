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

// The four cases below cover the v1.2 "height-above-water" shore fade that
// replaces the old kShoreFadeDistM distance band. Shared fixture for (a)/(b):
// a straight pond edge (wet x < pond_w, all rows) on a base sloped 0.05 m/m
// (dz=0.25 per texel at texel_m=5, matching real map texel spacing rather
// than the 1m used by the other tests here) -- a straight wall maximizes the
// chance that some row hits a near-worst-case octave alignment right at the
// shoreline, which is what the old code's RED violation needs to show up
// reliably.
namespace {
struct ShoreFixture {
  int n = 128;
  float texel_m = 5.0f;
  int pond_w = 20;
  float depth = 1.5f;
  float dz = 0.25f;  // 0.05 m/m at texel_m = 5
  Field2D<float> base{n, n};
  Field2D<float> water{n, n, 0.0f};
  ShoreFixture() {
    for (int y = 0; y < n; ++y)
      for (int x = 0; x < n; ++x) {
        base.at(x, y) = x * dz;
        if (x < pond_w) water.at(x, y) = depth;
      }
  }
};
}  // namespace

TEST_CASE("gully_detail_delta: shore fade has no band-edge jump (ring regression)") {
  // Old behaviour (distance-based shore = clamp(dist/kShoreFadeDistM=3, 0, 1)):
  // once a dry texel's distance to water reaches 3m, shore saturates to 1 --
  // and at texel_m=5 (a realistic map texel spacing) that happens in the
  // VERY FIRST dry texel. So the fade ramp collapses entirely: the
  // force-zeroed wet cell sits directly next to a full-strength carved dry
  // cell. Measured against the pre-fix implementation on this fixture: max
  // adjacent-texel delta jump = 0.5811 at the shoreline itself, which is
  // this test's RED evidence (see task-2-report.md). The new height-based
  // fade (kShoreFadeHeightM=2m) needs a full 2m of relief to ramp, which at
  // this gentle slope spans ~40 texels -- so it stays smooth through the
  // whole shore-adjacent window (measured 0.1106).
  ShoreFixture f;
  ErosionParams p;
  const auto d = gully_detail_delta(f.base, f.water, f.texel_m, 9, p);

  const float bound = 0.25f * p.detail_amplitude_m;  // practical smoothness bound
  float max_jump = 0.0f;
  for (int y = 0; y < f.n; ++y)
    for (int x = f.pond_w - 1; x < f.pond_w + 10 && x + 1 < f.n; ++x)
      max_jump = std::max(max_jump, std::abs(d.at(x + 1, y) - d.at(x, y)));

  REQUIRE(max_jump <= bound);
}

TEST_CASE("gully_detail_delta: dry texels at/below the local water surface carve nothing") {
  // In the shared fixture the pond's surface (at its last wet column) is
  // 6.25m; texels x=20..23 stay below that surface (height above water still
  // negative), so the fade must be exactly 0 there regardless of the
  // underlying carve/noise value.
  ShoreFixture f;
  ErosionParams p;
  const auto d = gully_detail_delta(f.base, f.water, f.texel_m, 9, p);
  for (int y = 0; y < f.n; ++y)
    for (int x = f.pond_w; x < f.pond_w + 4; ++x) REQUIRE(d.at(x, y) == 0.0f);
}

TEST_CASE("gully_detail_delta: a high bank near shore carves at full strength") {
  // A steep bank (slope 1.0 m/m, well past kSlopeRef) starting right at a
  // shallow pond's edge: by 4 texels from shore, base is already 3.9m above
  // the pond surface (>> kShoreFadeHeightM=2m), so the fade must saturate to
  // 1 and carve EXACTLY like an unfaded (no-water, "far field") cell at the
  // same position/seed -- not just "nonzero", but bit-identical to the
  // fade=1 case.
  const int n = 64;
  const float texel_m = 1.0f;
  const int pond_w = 10;
  const float depth = 0.1f;
  const float dz_bank = 1.0f;

  Field2D<float> base(n, n);
  Field2D<float> water(n, n, 0.0f);
  for (int y = 0; y < n; ++y)
    for (int x = 0; x < n; ++x) {
      if (x < pond_w) {
        base.at(x, y) = 0.0f;
        water.at(x, y) = depth;
      } else {
        base.at(x, y) = (x - pond_w + 1) * dz_bank;
      }
    }

  ErosionParams p;
  const Field2D<float> dry(n, n, 0.0f);
  const auto carve_only = gully_detail_delta(base, dry, texel_m, 9, p);
  const auto d = gully_detail_delta(base, water, texel_m, 9, p);

  const int x = pond_w + 3;  // 4 texels from shore
  REQUIRE(base.at(x, 0) - depth >= 3.0f);  // sanity: really >= 3m above the surface
  for (int y = 0; y < n; ++y) {
    REQUIRE(d.at(x, y) == carve_only.at(x, y));
    REQUIRE(d.at(x, y) != 0.0f);
  }
}
