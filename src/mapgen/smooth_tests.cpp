#include <catch_amalgamated.hpp>

#include <cmath>
#include <numeric>

#include "mapgen/smooth.hpp"

using namespace badlands::mapgen;

TEST_CASE("smooth_heightmap: disabled settings return the input bit-identically") {
  Field2D<float> h(16, 16);
  for (int y = 0; y < 16; ++y)
    for (int x = 0; x < 16; ++x) h.at(x, y) = static_cast<float>(x * y) * 0.37f;

  REQUIRE(smooth_heightmap(h, 1.0f, 0.0f, 1.0f).data == h.data);
  REQUIRE(smooth_heightmap(h, 1.0f, -1.0f, 1.0f).data == h.data);
  REQUIRE(smooth_heightmap(h, 1.0f, 2.0f, 0.0f).data == h.data);
}

TEST_CASE("smooth_heightmap: a constant field is unchanged") {
  // The kernel is normalized to sum to 1, so this must hold exactly rather
  // than approximately — including at the clamp-extended edges.
  Field2D<float> h(24, 24, 7.25f);
  const auto out = smooth_heightmap(h, 1.0f, 3.0f, 1.0f);
  for (float v : out.data) REQUIRE(v == Catch::Approx(7.25f).margin(1e-5));
}

TEST_CASE("smooth_heightmap: a spike spreads symmetrically and conserves its mass") {
  // Interior placement matters: clamp-extended edges do NOT conserve sum, so a
  // spike near the border would legitimately fail this.
  const int n = 41;
  Field2D<float> h(n, n, 0.0f);
  h.at(20, 20) = 100.0f;
  const auto out = smooth_heightmap(h, 1.0f, 2.0f, 1.0f);

  const float total = std::accumulate(out.data.begin(), out.data.end(), 0.0f);
  REQUIRE(total == Catch::Approx(100.0f).epsilon(1e-3));
  REQUIRE(out.at(20, 20) < 100.0f);      // the peak came down
  REQUIRE(out.at(20, 20) > 0.0f);
  // Symmetric about the spike on both axes and both diagonals.
  for (int k = 1; k <= 5; ++k) {
    REQUIRE(out.at(20 + k, 20) == Catch::Approx(out.at(20 - k, 20)));
    REQUIRE(out.at(20, 20 + k) == Catch::Approx(out.at(20, 20 - k)));
    REQUIRE(out.at(20 + k, 20 + k) == Catch::Approx(out.at(20 - k, 20 - k)));
  }
}

TEST_CASE("smooth_heightmap: strength interpolates between input and full blur") {
  const int n = 33;
  Field2D<float> h(n, n, 0.0f);
  for (int y = 0; y < n; ++y)
    for (int x = 0; x < n; ++x)
      h.at(x, y) = ((x + y) % 2 == 0) ? 1.0f : -1.0f;  // worst case for a low-pass

  const auto full = smooth_heightmap(h, 1.0f, 2.0f, 1.0f);
  const auto half = smooth_heightmap(h, 1.0f, 2.0f, 0.5f);
  for (int y = 4; y < n - 4; ++y)
    for (int x = 4; x < n - 4; ++x) {
      const float a = h.at(x, y), b = full.at(x, y), m = half.at(x, y);
      REQUIRE(m == Catch::Approx(a + (b - a) * 0.5f).margin(1e-5));
    }
}

TEST_CASE("smooth_heightmap: sigma is in WORLD METRES, not texels") {
  // The resolution-independence invariant. The same world sigma over the same
  // world extent must blur the same physical distance at two resolutions —
  // which is why the parameter is metres and the kernel radius is derived.
  const float extent = 64.0f;
  auto ramp_with_bump = [&](int n) {
    Field2D<float> h(n, n, 0.0f);
    const float texel = extent / static_cast<float>(n);
    for (int y = 0; y < n; ++y)
      for (int x = 0; x < n; ++x) {
        const float wx = static_cast<float>(x) * texel;
        h.at(x, y) = std::abs(wx - 32.0f) < 4.0f ? 1.0f : 0.0f;  // a 8 m slab
      }
    return h;
  };

  const int lo_n = 64, hi_n = 128;
  const auto lo = smooth_heightmap(ramp_with_bump(lo_n), extent / lo_n, 3.0f, 1.0f);
  const auto hi = smooth_heightmap(ramp_with_bump(hi_n), extent / hi_n, 3.0f, 1.0f);

  // Compare the two profiles at matching WORLD positions.
  for (float wx = 20.0f; wx <= 44.0f; wx += 2.0f) {
    const int lx = static_cast<int>(wx / (extent / lo_n));
    const int hx = static_cast<int>(wx / (extent / hi_n));
    INFO("world x = " << wx);
    REQUIRE(lo.at(lx, lo_n / 2) == Catch::Approx(hi.at(hx, hi_n / 2)).margin(0.05));
  }
}

TEST_CASE("smooth_heightmap: never invents a value outside the input range") {
  // A Gaussian is a convex combination of its inputs, so it cannot overshoot.
  const int n = 32;
  Field2D<float> h(n, n);
  for (int y = 0; y < n; ++y)
    for (int x = 0; x < n; ++x)
      h.at(x, y) = std::sin(static_cast<float>(x) * 0.7f) * 3.0f +
                   std::cos(static_cast<float>(y) * 0.4f) * 2.0f;
  float lo = h.data[0], hi = h.data[0];
  for (float v : h.data) { lo = std::min(lo, v); hi = std::max(hi, v); }

  const auto out = smooth_heightmap(h, 1.0f, 2.5f, 1.0f);
  for (float v : out.data) {
    REQUIRE(v >= lo - 1e-4f);
    REQUIRE(v <= hi + 1e-4f);
  }
}
