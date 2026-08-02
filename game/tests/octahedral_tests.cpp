// The impostor's view parameterization (src/game/visual/octahedral.hpp).
//
// Worth pinning hard despite being small: the baker and the material both read
// this mapping, so a sign slip or an off-by-half here does not fail loudly --
// it bakes one view and samples another, and the result is a tree that looks
// subtly wrong from every angle at once. None of that needs a GPU to catch.

#include <catch_amalgamated.hpp>

#include <array>
#include <cmath>
#include <vector>

#include <glm/glm.hpp>
#include <glm/gtc/constants.hpp>

#include "game/visual/octahedral.hpp"

using namespace badlands;

namespace {

// A dense sweep of the upper hemisphere, biased to include the awkward places:
// the zenith, the horizon, and the diagonals where the encode's |x|+|z| terms
// change branch.
std::vector<glm::vec3> HemisphereSweep(int azimuth_steps, int elevation_steps) {
  std::vector<glm::vec3> out;
  out.reserve(static_cast<size_t>(azimuth_steps) * elevation_steps + 1);
  for (int e = 0; e <= elevation_steps; ++e) {
    const float el = (static_cast<float>(e) / elevation_steps) *
                     (glm::pi<float>() * 0.5f);
    for (int a = 0; a < azimuth_steps; ++a) {
      const float az =
          (static_cast<float>(a) / azimuth_steps) * glm::two_pi<float>();
      out.push_back(glm::vec3(std::cos(el) * std::cos(az), std::sin(el),
                              std::cos(el) * std::sin(az)));
    }
  }
  return out;
}

// The blend expanded to a full per-view weight vector, so two blends can be
// compared even when they name different views.
std::array<float, kImpostorViewCount> WeightVector(const ImpostorBlend& b) {
  std::array<float, kImpostorViewCount> w{};
  for (int k = 0; k < 3; ++k) w[static_cast<size_t>(b.view[k])] += b.weight[k];
  return w;
}

}  // namespace

TEST_CASE("Hemi-octahedral encode and decode round-trip", "[impostor]") {
  for (const glm::vec3& d : HemisphereSweep(64, 32)) {
    const glm::vec3 back = HemiOctDecode(HemiOctEncode(d));
    INFO("dir " << d.x << "," << d.y << "," << d.z);
    CHECK(glm::length(back - d) < 1e-4f);
  }
}

TEST_CASE("The map's anchors are where the layout assumes", "[impostor]") {
  // Everything downstream -- the tile grid, the elevation argument for putting
  // views at centres -- assumes centre = zenith and boundary = horizon. If that
  // ever changed, the tile layout would be silently wrong rather than broken.
  const glm::vec2 zenith = HemiOctEncode(glm::vec3(0.0f, 1.0f, 0.0f));
  CHECK(zenith.x == Catch::Approx(0.5f));
  CHECK(zenith.y == Catch::Approx(0.5f));

  const auto corner = [](glm::vec3 d) { return HemiOctEncode(glm::normalize(d)); };
  CHECK(corner({0, 0, -1}).x == Catch::Approx(0.0f));
  CHECK(corner({0, 0, -1}).y == Catch::Approx(0.0f));
  CHECK(corner({1, 0, 0}).x == Catch::Approx(1.0f));
  CHECK(corner({1, 0, 0}).y == Catch::Approx(0.0f));
  CHECK(corner({0, 0, 1}).x == Catch::Approx(1.0f));
  CHECK(corner({0, 0, 1}).y == Catch::Approx(1.0f));
  CHECK(corner({-1, 0, 0}).x == Catch::Approx(0.0f));
  CHECK(corner({-1, 0, 0}).y == Catch::Approx(1.0f));
}

TEST_CASE("A direction below the horizon is projected onto it", "[impostor]") {
  // A free camera can dip below a tree on a rise. The nearest in-gamut view is
  // a far better answer than a NaN or a flipped one.
  const glm::vec3 below = glm::normalize(glm::vec3(1.0f, -0.5f, 0.0f));
  const glm::vec3 got = HemiOctDecode(HemiOctEncode(below));
  CHECK(got.y >= 0.0f);
  CHECK(got.x == Catch::Approx(1.0f).margin(1e-4));

  // And it still produces a usable blend rather than a degenerate one.
  const ImpostorBlend b = ImpostorBlendFor(below);
  CHECK(b.weight[0] + b.weight[1] + b.weight[2] == Catch::Approx(1.0f));
}

TEST_CASE("Every baked view is a distinct unit direction above the horizon",
          "[impostor]") {
  std::vector<glm::vec3> dirs;
  for (int j = 0; j < kImpostorViewsPerAxis; ++j) {
    for (int i = 0; i < kImpostorViewsPerAxis; ++i) {
      const glm::vec3 d = ImpostorViewDirection(i, j);
      INFO("view " << i << "," << j);
      CHECK(glm::length(d) == Catch::Approx(1.0f).margin(1e-5));
      // Strictly above, not merely on: this is the whole reason views sit at
      // tile centres rather than grid vertices. A view ON the horizon is one a
      // 50-58 degree camera never looks from.
      CHECK(d.y > 0.05f);
      dirs.push_back(d);
    }
  }
  REQUIRE(dirs.size() == static_cast<size_t>(kImpostorViewCount));

  for (size_t a = 0; a < dirs.size(); ++a) {
    for (size_t b = a + 1; b < dirs.size(); ++b) {
      INFO("views " << a << " and " << b);
      CHECK(glm::length(dirs[a] - dirs[b]) > 1e-3f);
    }
  }
}

TEST_CASE("Blend weights are a partition of unity everywhere", "[impostor]") {
  // Swept finely enough to land on cell edges and on the diagonal seam, which
  // is where a barycentric split goes negative if the triangle test is wrong.
  for (const glm::vec3& d : HemisphereSweep(128, 64)) {
    const ImpostorBlend b = ImpostorBlendFor(d);
    INFO("dir " << d.x << "," << d.y << "," << d.z);
    float sum = 0.0f;
    for (int k = 0; k < 3; ++k) {
      CHECK(b.weight[k] >= -1e-6f);
      CHECK(b.view[k] >= 0);
      CHECK(b.view[k] < kImpostorViewCount);
      sum += b.weight[k];
    }
    CHECK(sum == Catch::Approx(1.0f).margin(1e-5));
  }
}

TEST_CASE("Querying a baked view's own direction returns that view alone",
          "[impostor]") {
  // The interpolation must be exact at its sample points, or every tree is
  // permanently a blur of neighbours even when looked at from a baked angle.
  for (int j = 0; j < kImpostorViewsPerAxis; ++j) {
    for (int i = 0; i < kImpostorViewsPerAxis; ++i) {
      const ImpostorBlend b = ImpostorBlendFor(ImpostorViewDirection(i, j));
      const auto w = WeightVector(b);
      INFO("view " << i << "," << j);
      CHECK(w[static_cast<size_t>(ImpostorViewIndex(i, j))] ==
            Catch::Approx(1.0f).margin(1e-4));
    }
  }
}

TEST_CASE("The blend is continuous, so views cannot pop", "[impostor]") {
  // THE property the whole scheme exists for, and the one a nearest-3 rule
  // would fail: as the view direction sweeps, the weight vector must move
  // smoothly rather than jumping when the triangle changes. A jump here is a
  // visible flicker on a distant tree, which is worse than the coarse LOD it
  // replaced.
  //
  // Bounded against the STEP: neighbouring samples one small angular step apart
  // may not shift more than a small fraction of the total weight.
  constexpr int kAz = 720;
  constexpr int kEl = 90;
  for (int e = 0; e <= kEl; ++e) {
    const float el = (static_cast<float>(e) / kEl) * (glm::pi<float>() * 0.49f);
    std::array<float, kImpostorViewCount> prev{};
    for (int a = 0; a <= kAz; ++a) {
      const float az =
          (static_cast<float>(a) / kAz) * glm::two_pi<float>();
      const glm::vec3 d(std::cos(el) * std::cos(az), std::sin(el),
                        std::cos(el) * std::sin(az));
      const auto w = WeightVector(ImpostorBlendFor(d));
      if (a > 0) {
        float l1 = 0.0f;
        for (size_t k = 0; k < w.size(); ++k) l1 += std::abs(w[k] - prev[k]);
        INFO("elevation step " << e << " azimuth step " << a);
        CHECK(l1 < 0.1f);
      }
      prev = w;
    }
  }
}

TEST_CASE("The blended views reconstruct the queried direction", "[impostor]") {
  // A weight set can be continuous and still point somewhere else entirely.
  // This is the accuracy statement: the weighted mean of the chosen views'
  // directions must lie near the direction asked for, which bounds how wrong
  // the parallax can be between baked angles.
  //
  // The bound is DERIVED, not picked. 16 views over a hemisphere is
  // 2*pi/16 = 0.393 sr each; a cone of that solid angle has half-angle
  // acos(1 - 0.393/(2*pi)) = 20.4 deg, so ~20 deg IS this grid's angular
  // resolution and no interpolation can do better than roughly that. Measured
  // worst case: 18.4 deg -- i.e. the blend is already at the grid's limit. The
  // 22 deg guard is headroom over that measurement, there to catch a real
  // regression (a wrong triangle, a transposed axis) rather than to be a tight
  // fit; a bound BELOW ~20 deg would be asserting something arithmetically
  // impossible at this view count.
  //
  // Raising kImpostorViewsPerAxis is what moves this number: 6x6 would put the
  // resolution near 13 deg.
  float worst_deg = 0.0f;
  for (const glm::vec3& d : HemisphereSweep(64, 24)) {
    // Below the outermost baked views' own elevation (~18 deg) the blend
    // clamps by design, so there is nothing to reconstruct toward.
    if (d.y < 0.34f) continue;

    const ImpostorBlend b = ImpostorBlendFor(d);
    glm::vec3 acc(0.0f);
    for (int k = 0; k < 3; ++k) {
      const int v = b.view[k];
      acc += b.weight[k] * ImpostorViewDirection(v % kImpostorViewsPerAxis,
                                                 v / kImpostorViewsPerAxis);
    }
    REQUIRE(glm::length(acc) > 1e-3f);
    const float deg = glm::degrees(
        std::acos(std::clamp(glm::dot(glm::normalize(acc), d), -1.0f, 1.0f)));
    worst_deg = std::max(worst_deg, deg);
  }
  INFO("worst reconstruction error " << worst_deg << " deg");
  CHECK(worst_deg < 22.0f);
  // And it must not be trivially small either -- a blend that always returned
  // the query direction would mean the sweep never left a baked view, i.e. the
  // test is not exercising interpolation at all.
  CHECK(worst_deg > 1.0f);
}
