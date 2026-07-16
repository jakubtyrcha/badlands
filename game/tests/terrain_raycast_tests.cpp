// Pure-CPU tests for SampleHeight + RaycastTerrain — the mouse-picking query
// behind the hover grid.
//
// The distinguishing property is that this follows the HEIGHTMAP, not a flat
// y=0 plane: a plane test would answer confidently and wrongly over any relief,
// so the ramp cases below are the ones that actually pin the behaviour.

#include <catch_amalgamated.hpp>

#include <cmath>

#include <glm/glm.hpp>

#include "engine/core/ray.hpp"
#include "game/geometry/terrain_mesh.hpp"
#include "mapgen/field2d.hpp"
#include "mapgen/mapgen_constants.hpp"

using namespace badlands;
using badlands::mapgen::Field2D;
using badlands::mapgen::kMetersPerSample;

namespace {

Field2D<float> Flat(int w, int h, float y) {
  Field2D<float> f(w, h, y);
  return f;
}

// Height ramps with world x: h(x) = x * slope.
Field2D<float> RampX(int w, int h, float slope) {
  Field2D<float> f(w, h, 0.0f);
  for (int z = 0; z < h; ++z)
    for (int x = 0; x < w; ++x)
      f.at(x, z) = static_cast<float>(x) * kMetersPerSample * slope;
  return f;
}

Ray Down(glm::vec3 from) { return Ray{from, glm::vec3(0.0f, -1.0f, 0.0f)}; }

}  // namespace

TEST_CASE("SampleHeight: bilinear, clamped at the edges") {
  constexpr int kSize = 10;
  constexpr float kSlope = 1.0f;
  // RampX stores x * kMetersPerSample * slope, and SampleHeight converts world
  // -> sample space, so h(world_x) == world_x * slope at ANY sample density.
  Field2D<float> f = RampX(kSize, kSize, kSlope);
  CHECK(SampleHeight(f, 0.0f, 0.0f) == Catch::Approx(0.0f));
  CHECK(SampleHeight(f, 5.0f, 5.0f) == Catch::Approx(5.0f));
  CHECK(SampleHeight(f, 2.5f, 0.0f) == Catch::Approx(2.5f));  // between samples

  // Off-map clamps to the edge rather than reading out of bounds. The last
  // sample sits at world x = (kSize-1) * kMetersPerSample, so DERIVE the edge
  // height: a literal here would assert a 1 m density and break if it changed.
  const float edge_h =
      static_cast<float>(kSize - 1) * kMetersPerSample * kSlope;
  CHECK(SampleHeight(f, -100.0f, 0.0f) == Catch::Approx(0.0f));
  CHECK(SampleHeight(f, 1e6f, 0.0f) == Catch::Approx(edge_h));
}

TEST_CASE("RaycastTerrain: straight down onto flat ground") {
  Field2D<float> f = Flat(64, 64, 5.0f);
  glm::vec3 hit;
  REQUIRE(RaycastTerrain(f, Down(glm::vec3(20.0f, 50.0f, 30.0f)), hit));
  CHECK(hit.x == Catch::Approx(20.0f).margin(1e-2));
  CHECK(hit.z == Catch::Approx(30.0f).margin(1e-2));
  CHECK(hit.y == Catch::Approx(5.0f).margin(1e-2));
}

TEST_CASE("RaycastTerrain: a ray aimed at the sky misses") {
  Field2D<float> f = Flat(64, 64, 5.0f);
  glm::vec3 hit;
  CHECK_FALSE(RaycastTerrain(
      f, Ray{glm::vec3(20.0f, 50.0f, 30.0f),
             glm::normalize(glm::vec3(0.0f, 1.0f, -1.0f))},
      hit));
}

TEST_CASE("RaycastTerrain: an angled ray lands ON the slope, not the plane") {
  // The discriminating case. Ground ramps h = x. A ray fired down-and-along +x
  // must stop where it MEETS the rising terrain — a y=0 plane test (or a
  // marcher that ignores height) would run far past that point.
  Field2D<float> f = RampX(64, 64, 1.0f);
  const glm::vec3 origin(0.0f, 10.0f, 10.0f);
  const glm::vec3 dir = glm::normalize(glm::vec3(1.0f, -1.0f, 0.0f));
  glm::vec3 hit;
  REQUIRE(RaycastTerrain(f, Ray{origin, dir}, hit));

  // Analytic: y(t) = 10 - t/sqrt2, x(t) = t/sqrt2, surface h = x -> hit at x = 5.
  CHECK(hit.x == Catch::Approx(5.0f).margin(0.05));
  CHECK(hit.y == Catch::Approx(5.0f).margin(0.05));
  CHECK(hit.z == Catch::Approx(10.0f).margin(0.05));
  // And the hit must sit ON the surface.
  CHECK(hit.y == Catch::Approx(SampleHeight(f, hit.x, hit.z)).margin(1e-2));
}

TEST_CASE("RaycastTerrain: hit always lies on the sampled surface") {
  Field2D<float> f = RampX(64, 64, 0.25f);
  for (float dz : {-0.6f, 0.0f, 0.6f}) {
    const glm::vec3 dir = glm::normalize(glm::vec3(0.7f, -1.0f, dz));
    glm::vec3 hit;
    REQUIRE(RaycastTerrain(f, Ray{glm::vec3(1.0f, 20.0f, 30.0f), dir}, hit));
    INFO("dz = " << dz << " hit = " << hit.x << "," << hit.y << "," << hit.z);
    CHECK(hit.y == Catch::Approx(SampleHeight(f, hit.x, hit.z)).margin(1e-2));
  }
}

TEST_CASE("RaycastTerrain: a camera below the surface picks nothing") {
  Field2D<float> f = Flat(64, 64, 5.0f);
  glm::vec3 hit;
  CHECK_FALSE(RaycastTerrain(f, Down(glm::vec3(20.0f, 1.0f, 30.0f)), hit));
}

TEST_CASE("RaycastTerrain: an empty heightmap is not a crash") {
  Field2D<float> empty;
  glm::vec3 hit;
  CHECK_FALSE(RaycastTerrain(empty, Down(glm::vec3(0.0f, 10.0f, 0.0f)), hit));
}
