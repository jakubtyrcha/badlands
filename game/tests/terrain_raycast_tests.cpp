// Pure-CPU tests for MapData::HeightAt + RaycastTerrain — the mouse-picking
// query behind the hover grid.
//
// The distinguishing property is that this follows the HEIGHTMAP, not a flat
// y=0 plane: a plane test would answer confidently and wrongly over any relief,
// so the ramp cases below are the ones that actually pin the behaviour.

#include <catch_amalgamated.hpp>

#include <cmath>

#include <glm/glm.hpp>

#include "engine/core/ray.hpp"
#include "game/geometry/terrain_mesh.hpp"
#include "game/map/map_data.hpp"

using namespace badlands;

namespace {

// 1 m lattice spacing keeps world coordinates equal to node indices, so the
// analytic expectations below read directly.
constexpr float kSpacing = 1.0f;

MapData Flat(int w, int h, float y) {
  MapData m(w, h, kSpacing);
  for (int j = 0; j < h; ++j)
    for (int i = 0; i < w; ++i) m.mutable_height(i, j) = y;
  return m;
}

// Height ramps with world x: h(x) = x * slope.
MapData RampX(int w, int h, float slope) {
  MapData m(w, h, kSpacing);
  for (int j = 0; j < h; ++j)
    for (int i = 0; i < w; ++i)
      m.mutable_height(i, j) = static_cast<float>(i) * kSpacing * slope;
  return m;
}

Ray Down(glm::vec3 from) { return Ray{from, glm::vec3(0.0f, -1.0f, 0.0f)}; }

}  // namespace

TEST_CASE("MapData::HeightAt: bilinear, clamped at the edges") {
  constexpr int kSize = 10;
  constexpr float kSlope = 1.0f;
  const MapData m = RampX(kSize, kSize, kSlope);
  CHECK(m.HeightAt(0.0f, 0.0f) == Catch::Approx(0.0f));
  CHECK(m.HeightAt(5.0f, 5.0f) == Catch::Approx(5.0f));
  CHECK(m.HeightAt(2.5f, 0.0f) == Catch::Approx(2.5f));  // between nodes

  // Off-map clamps to the edge rather than reading out of bounds. The last node
  // sits at world x = (kSize-1) * spacing, so DERIVE the edge height.
  const float edge_h = static_cast<float>(kSize - 1) * kSpacing * kSlope;
  CHECK(m.HeightAt(-100.0f, 0.0f) == Catch::Approx(0.0f));
  CHECK(m.HeightAt(1e6f, 0.0f) == Catch::Approx(edge_h));
}

TEST_CASE("RaycastTerrain: straight down onto flat ground") {
  const MapData m = Flat(64, 64, 5.0f);
  glm::vec3 hit;
  REQUIRE(RaycastTerrain(m, Down(glm::vec3(20.0f, 50.0f, 30.0f)), hit));
  CHECK(hit.x == Catch::Approx(20.0f).margin(1e-2));
  CHECK(hit.z == Catch::Approx(30.0f).margin(1e-2));
  CHECK(hit.y == Catch::Approx(5.0f).margin(1e-2));
}

TEST_CASE("RaycastTerrain: a ray aimed at the sky misses") {
  const MapData m = Flat(64, 64, 5.0f);
  glm::vec3 hit;
  CHECK_FALSE(RaycastTerrain(
      m, Ray{glm::vec3(20.0f, 50.0f, 30.0f),
             glm::normalize(glm::vec3(0.0f, 1.0f, -1.0f))},
      hit));
}

TEST_CASE("RaycastTerrain: an angled ray lands ON the slope, not the plane") {
  // The discriminating case. Ground ramps h = x. A ray fired down-and-along +x
  // must stop where it MEETS the rising terrain — a y=0 plane test (or a
  // marcher that ignores height) would run far past that point.
  const MapData m = RampX(64, 64, 1.0f);
  const glm::vec3 origin(0.0f, 10.0f, 10.0f);
  const glm::vec3 dir = glm::normalize(glm::vec3(1.0f, -1.0f, 0.0f));
  glm::vec3 hit;
  REQUIRE(RaycastTerrain(m, Ray{origin, dir}, hit));

  // Analytic: y(t) = 10 - t/sqrt2, x(t) = t/sqrt2, surface h = x -> hit at x = 5.
  CHECK(hit.x == Catch::Approx(5.0f).margin(0.05));
  CHECK(hit.y == Catch::Approx(5.0f).margin(0.05));
  CHECK(hit.z == Catch::Approx(10.0f).margin(0.05));
  // And the hit must sit ON the surface.
  CHECK(hit.y == Catch::Approx(m.HeightAt(hit.x, hit.z)).margin(1e-2));
}

TEST_CASE("RaycastTerrain: hit always lies on the sampled surface") {
  const MapData m = RampX(64, 64, 0.25f);
  for (float dz : {-0.6f, 0.0f, 0.6f}) {
    const glm::vec3 dir = glm::normalize(glm::vec3(0.7f, -1.0f, dz));
    glm::vec3 hit;
    REQUIRE(RaycastTerrain(m, Ray{glm::vec3(1.0f, 20.0f, 30.0f), dir}, hit));
    INFO("dz = " << dz << " hit = " << hit.x << "," << hit.y << "," << hit.z);
    CHECK(hit.y == Catch::Approx(m.HeightAt(hit.x, hit.z)).margin(1e-2));
  }
}

TEST_CASE("RaycastTerrain: a camera below the surface picks nothing") {
  const MapData m = Flat(64, 64, 5.0f);
  glm::vec3 hit;
  CHECK_FALSE(RaycastTerrain(m, Down(glm::vec3(20.0f, 1.0f, 30.0f)), hit));
}

TEST_CASE("RaycastTerrain: an empty map is not a crash") {
  const MapData empty;
  glm::vec3 hit;
  CHECK_FALSE(RaycastTerrain(empty, Down(glm::vec3(0.0f, 10.0f, 0.0f)), hit));
}
