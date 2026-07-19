// Tests for the biome-driven fog emitter generator (fog_generator.cpp): only
// forest/swamp patches emit, forest is flat and swamp is noise, and the emitter's
// oriented-ellipse footprint is fitted to the covariance of the biome region.

#include <catch_amalgamated.hpp>

#include <cmath>
#include <vector>

#include "mapgen/biomes.hpp"
#include "mapgen/field2d.hpp"
#include "mapgen/fog_generator.hpp"

using namespace badlands::mapgen;
using badlands::fog::EmitterShape;
using badlands::fog::EmitterType;

namespace {

// A biome field filled with `bg`, plus a filled axis-aligned rectangle of `fg`.
Field2D<uint8_t> RectField(int w, int h, Biome bg, Biome fg, int x0, int z0,
                           int x1, int z1) {
  Field2D<uint8_t> f(w, h, static_cast<uint8_t>(bg));
  for (int z = z0; z < z1; ++z)
    for (int x = x0; x < x1; ++x) f.at(x, z) = static_cast<uint8_t>(fg);
  return f;
}

}  // namespace

TEST_CASE("GenerateBiomeFog: plains-only map emits nothing") {
  Field2D<uint8_t> biome(128, 128, static_cast<uint8_t>(Biome::Plains));
  Field2D<float> height(128, 128, 0.0f);
  const auto ems = GenerateBiomeFog(biome, height, 1);
  CHECK(ems.empty());
}

TEST_CASE("GenerateBiomeFog: a forest region emits flat elliptical emitters") {
  // Forest everywhere -> every patch qualifies; emitters are Ellipse + Disc(flat).
  Field2D<uint8_t> biome(128, 128, static_cast<uint8_t>(Biome::Forest));
  Field2D<float> height(128, 128, 3.0f);
  const auto ems = GenerateBiomeFog(biome, height, 7);
  REQUIRE(!ems.empty());
  for (const auto& e : ems) {
    CHECK(e.shape == EmitterShape::Ellipse);
    CHECK(e.type == EmitterType::Disc);  // forest = flat radial fog
    CHECK(e.magnitude > 0.0f);
    CHECK(e.base_y == Catch::Approx(3.0f));  // sampled terrain height
  }
}

TEST_CASE("GenerateBiomeFog: a swamp region emits animated noise emitters") {
  Field2D<uint8_t> biome(128, 128, static_cast<uint8_t>(Biome::Swamp));
  Field2D<float> height(128, 128, 0.0f);
  const auto ems = GenerateBiomeFog(biome, height, 3);
  REQUIRE(!ems.empty());
  bool any_scroll = false;
  for (const auto& e : ems) {
    CHECK(e.type == EmitterType::Noise);  // swamp = granular time-animated fog
    CHECK(e.noise_freq > 0.0f);
    any_scroll |= (e.scroll.y != 0.0f);
  }
  CHECK(any_scroll);
}

TEST_CASE("GenerateBiomeFog: footprint orientation follows the biome elongation") {
  // A forest band elongated along X. It must be the majority within a 16 m gather
  // disc (so ~24 tall, wider than half the 32 m diameter) yet still clipped in Z
  // by the disc, so the covariance is wider in X: rotation ~0, half_extent.x > .y.
  Field2D<uint8_t> biome =
      RectField(160, 160, Biome::Plains, Biome::Forest,
                /*x0*/ 0, /*z0*/ 68, /*x1*/ 160, /*z1*/ 92);  // full-width, 24 tall
  Field2D<float> height(160, 160, 0.0f);
  const auto ems = GenerateBiomeFog(biome, height, 5);
  REQUIRE(!ems.empty());

  // Find an emitter well inside the strip (center z ~ 80).
  int checked = 0;
  for (const auto& e : ems) {
    if (std::abs(e.center.y - 80.0f) > 6.0f) continue;  // e.center.y is world Z
    // Elongated along X: half_extent.x is the major axis.
    CHECK(e.half_extent.x > e.half_extent.y);
    // Major axis aligned with X => rotation near 0 or +-pi (both align local x/X).
    const float r = std::fmod(std::abs(e.rotation), static_cast<float>(M_PI));
    CHECK((r < 0.3f || r > static_cast<float>(M_PI) - 0.3f));
    ++checked;
  }
  CHECK(checked > 0);
}

TEST_CASE("GenerateBiomeFog: deterministic in the seed") {
  Field2D<uint8_t> biome(96, 96, static_cast<uint8_t>(Biome::Forest));
  Field2D<float> height(96, 96, 0.0f);
  const auto a = GenerateBiomeFog(biome, height, 42);
  const auto b = GenerateBiomeFog(biome, height, 42);
  const auto c = GenerateBiomeFog(biome, height, 43);
  REQUIRE(a.size() == b.size());
  for (size_t i = 0; i < a.size(); ++i) {
    CHECK(a[i].center.x == Catch::Approx(b[i].center.x));
    CHECK(a[i].center.y == Catch::Approx(b[i].center.y));
  }
  // A different seed jitters the sample points, so at least one center moves.
  bool differs = a.size() != c.size();
  for (size_t i = 0; i < a.size() && i < c.size() && !differs; ++i)
    differs = std::abs(a[i].center.x - c[i].center.x) > 1e-3f;
  CHECK(differs);
}
