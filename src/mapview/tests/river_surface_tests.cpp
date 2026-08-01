// Pure-CPU tests for the river channel ribbon.
//
// Three things are load-bearing and each is easy to get silently wrong:
//   - the ribbon SITS ON the rendered terrain (same node lattice, same bilinear
//     rule), rather than on a plane through it;
//   - its width is the hydraulic width the graph solved, not a constant;
//   - it is wound so it faces UP, or half the network renders backfacing.

#include <catch_amalgamated.hpp>

#include <algorithm>
#include <cmath>

#include "mapgen/generator.hpp"
#include "mapgen/river_arcs.hpp"
#include "mapview/river_surface.hpp"

using namespace badlands;

namespace {

constexpr int kN = 16;             // texels per side
constexpr float kWorldM = 160.0f;  // => 10 m texels
constexpr float kTexelM = kWorldM / static_cast<float>(kN);

mapgen::MapArtifacts FlatMap(float ground) {
  mapgen::MapArtifacts a;
  a.heightmap = mapgen::Field2D<float>(kN, kN, ground);
  a.water_depth = mapgen::Field2D<float>(kN, kN, 0.0f);
  a.lake_id = mapgen::Field2D<int32_t>(kN, kN, -1);
  return a;
}

// A straight west-to-east reach down the middle of the map, `width` wide
// throughout.
mapgen::RiverGraph StraightReach(float width, float x0, float x1, float z) {
  mapgen::RiverGraph g;
  g.nodes.resize(2);
  mapgen::RiverEdge e;
  for (int i = 0; i <= 8; ++i) {
    const float t = static_cast<float>(i) / 8.0f;
    e.points_m.push_back(glm::vec2(x0 + (x1 - x0) * t, z));
    e.width_m.push_back(width);
    e.depth_m.push_back(0.2f);
    e.speed_m_s.push_back(1.0f);
    e.discharge_m3_s.push_back(0.1f);
  }
  g.edges.push_back(std::move(e));
  return g;
}

// Bounds of the ribbon on one axis, so "how wide is it" is measurable.
struct Span {
  float lo = 1e30f, hi = -1e30f;
  void add(float v) {
    lo = std::min(lo, v);
    hi = std::max(hi, v);
  }
  float extent() const { return hi - lo; }
};

std::vector<glm::vec3> Build(const mapgen::MapArtifacts& art,
                             const mapgen::RiverGraph& g) {
  return BuildRiverRibbonTriangles(art, kWorldM, g,
                                   mapgen::build_river_arcs(g, 0.5f));
}

}  // namespace

TEST_CASE("an empty network produces no geometry", "[river_surface]") {
  const mapgen::MapArtifacts art = FlatMap(10.0f);
  CHECK(Build(art, mapgen::RiverGraph{}).empty());
}

TEST_CASE("the ribbon faces up", "[river_surface]") {
  // Winding, checked as the thing it actually controls: the geometric normal.
  const mapgen::MapArtifacts art = FlatMap(10.0f);
  const std::vector<glm::vec3> tris = Build(art, StraightReach(4.0f, 20.0f, 140.0f, 80.0f));
  REQUIRE(tris.size() >= 3);
  REQUIRE(tris.size() % 3 == 0);
  for (size_t i = 0; i < tris.size(); i += 3) {
    const glm::vec3 n = glm::cross(tris[i + 1] - tris[i], tris[i + 2] - tris[i]);
    INFO("triangle " << i / 3);
    CHECK(n.y > 0.0f);
  }
}

TEST_CASE("the drawn width is the channel's hydraulic width", "[river_surface]") {
  const mapgen::MapArtifacts art = FlatMap(10.0f);
  // Well clear of kMinRibbonWidthM so the floor is not what is being measured.
  for (const float w : {4.0f, 9.0f}) {
    const std::vector<glm::vec3> tris = Build(art, StraightReach(w, 20.0f, 140.0f, 80.0f));
    REQUIRE(!tris.empty());
    Span across;
    for (const glm::vec3& p : tris) across.add(p.z);
    INFO("width " << w);
    CHECK(across.extent() == Catch::Approx(w).margin(1e-3));
  }
}

TEST_CASE("a hairline is floored, not drawn true to width", "[river_surface]") {
  // The one deliberate departure from physical truth in this file; if the floor
  // ever stops applying, the thin half of the network silently disappears.
  const mapgen::MapArtifacts art = FlatMap(10.0f);
  const std::vector<glm::vec3> tris = Build(art, StraightReach(0.2f, 20.0f, 140.0f, 80.0f));
  REQUIRE(!tris.empty());
  Span across;
  for (const glm::vec3& p : tris) across.add(p.z);
  CHECK(across.extent() == Catch::Approx(kMinRibbonWidthM).margin(1e-3));
}

TEST_CASE("width follows the reach downstream", "[river_surface]") {
  // A reach that swells 2 m -> 8 m must produce a ribbon that swells with it.
  // A constant-width ribbon would pass every other test in this file.
  mapgen::MapArtifacts art = FlatMap(10.0f);
  mapgen::RiverGraph g;
  g.nodes.resize(2);
  mapgen::RiverEdge e;
  for (int i = 0; i <= 8; ++i) {
    const float t = static_cast<float>(i) / 8.0f;
    e.points_m.push_back(glm::vec2(20.0f + 120.0f * t, 80.0f));
    e.width_m.push_back(2.0f + 6.0f * t);
  }
  g.edges.push_back(std::move(e));

  const std::vector<glm::vec3> tris = Build(art, g);
  REQUIRE(!tris.empty());
  Span up, down;
  for (const glm::vec3& p : tris) {
    if (p.x < 40.0f) up.add(p.z);
    if (p.x > 120.0f) down.add(p.z);
  }
  CHECK(up.extent() < 4.0f);
  CHECK(down.extent() > 6.0f);
  CHECK(down.extent() > up.extent() + 2.0f);
}

TEST_CASE("the ribbon sits on the terrain, not on a plane", "[river_surface]") {
  // A ramp in X. Every vertex must land on the bilinear surface + the lift; a
  // ribbon built off node heights alone would sag between texels.
  mapgen::MapArtifacts art = FlatMap(0.0f);
  for (int z = 0; z < kN; ++z)
    for (int x = 0; x < kN; ++x) art.heightmap.at(x, z) = 3.0f * static_cast<float>(x);

  const std::vector<glm::vec3> tris = Build(art, StraightReach(4.0f, 20.0f, 140.0f, 80.0f));
  REQUIRE(!tris.empty());
  for (const glm::vec3& p : tris) {
    // Terrain node i carries texel min(i, kN-1)'s height, so the ramp is
    // h = 3*x/texel until the last node pair, which is flat.
    const float fx = std::clamp(p.x / kTexelM, 0.0f, static_cast<float>(kN));
    const int i0 = std::clamp(static_cast<int>(std::floor(fx)), 0, kN);
    const float t = fx - static_cast<float>(i0);
    const float h0 = 3.0f * static_cast<float>(std::min(i0, kN - 1));
    const float h1 = 3.0f * static_cast<float>(std::min(i0 + 1, kN - 1));
    INFO("at x=" << p.x);
    CHECK(p.y == Catch::Approx(h0 + (h1 - h0) * t + kRiverLiftM).margin(1e-2));
  }
}

TEST_CASE("the ribbon rides standing water instead of sinking under it",
          "[river_surface]") {
  mapgen::MapArtifacts art = FlatMap(10.0f);
  // Flood the eastern half to a surface of 14 m.
  for (int z = 0; z < kN; ++z)
    for (int x = kN / 2; x < kN; ++x) art.water_depth.at(x, z) = 4.0f;

  const std::vector<glm::vec3> tris = Build(art, StraightReach(4.0f, 20.0f, 140.0f, 80.0f));
  REQUIRE(!tris.empty());
  Span dry, wet;
  for (const glm::vec3& p : tris) {
    if (p.x < 60.0f) dry.add(p.y);
    if (p.x > 100.0f) wet.add(p.y);
  }
  CHECK(dry.lo == Catch::Approx(10.0f + kRiverLiftM).margin(1e-3));
  CHECK(wet.lo == Catch::Approx(14.0f + kRiverLiftM).margin(1e-3));
}

TEST_CASE("a curved reach bends rather than facets", "[river_surface]") {
  // A quarter circle of radius 40 m. The ribbon's centreline should stay on
  // that circle to well under a metre -- the polyline it was fitted from has
  // 9 vertices and would cut ~1.2 m of corner.
  mapgen::MapArtifacts art = FlatMap(5.0f);
  mapgen::RiverGraph g;
  g.nodes.resize(2);
  mapgen::RiverEdge e;
  const glm::vec2 c(80.0f, 80.0f);
  const float r = 40.0f;
  for (int i = 0; i <= 8; ++i) {
    const float a = 1.5f * static_cast<float>(i) / 8.0f;
    e.points_m.push_back(c + r * glm::vec2(std::cos(a), std::sin(a)));
    e.width_m.push_back(3.0f);
  }
  g.edges.push_back(std::move(e));

  const std::vector<glm::vec3> tris = Build(art, g);
  REQUIRE(!tris.empty());
  float worst = 0.0f;
  for (const glm::vec3& p : tris) {
    // Every vertex is half a width off the centreline, so its radius should be
    // r +/- 1.5 m.
    const float rad = glm::length(glm::vec2(p.x, p.z) - c);
    worst = std::max(worst, std::abs(std::abs(rad - r) - 1.5f));
  }
  CHECK(worst < 0.2f);
}

TEST_CASE("a chain pointing outside the graph is ignored, not dereferenced",
          "[river_surface]") {
  const mapgen::MapArtifacts art = FlatMap(10.0f);
  const mapgen::RiverGraph g = StraightReach(4.0f, 20.0f, 140.0f, 80.0f);
  std::vector<mapgen::RiverArcChain> chains = mapgen::build_river_arcs(g, 0.5f);
  REQUIRE(chains.size() == 1);
  chains[0].edge = 7;
  CHECK(BuildRiverRibbonTriangles(art, kWorldM, g, chains).empty());
  chains[0].edge = -1;
  CHECK(BuildRiverRibbonTriangles(art, kWorldM, g, chains).empty());
}

TEST_CASE("degenerate inputs produce nothing rather than NaNs",
          "[river_surface]") {
  const mapgen::RiverGraph g = StraightReach(4.0f, 20.0f, 140.0f, 80.0f);
  const std::vector<mapgen::RiverArcChain> chains = mapgen::build_river_arcs(g, 0.5f);
  CHECK(BuildRiverRibbonTriangles(mapgen::MapArtifacts{}, kWorldM, g, chains).empty());
  CHECK(BuildRiverRibbonTriangles(FlatMap(1.0f), 0.0f, g, chains).empty());

  const std::vector<glm::vec3> tris = Build(FlatMap(10.0f), g);
  for (const glm::vec3& p : tris) {
    CHECK(std::isfinite(p.x));
    CHECK(std::isfinite(p.y));
    CHECK(std::isfinite(p.z));
  }
}
