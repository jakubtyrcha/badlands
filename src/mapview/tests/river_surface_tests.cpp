// Pure-CPU tests for the river channel WATER surface.
//
// Four things are load-bearing and each is easy to get silently wrong:
//   - the level is the CARVED BED under the centreline plus the flow depth, so
//     the water sits inside the cavity with the banks proud of it;
//   - each cross-section is FLAT at that level across its full width -- draping
//     it is the debug-decal behaviour this replaced;
//   - its width is the hydraulic width the graph solved, TRUE to scale (the
//     legibility floor is gone with the cavity to sit in);
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
constexpr float kReachZ = 80.0f;   // every straight fixture runs along this z

// The artifacts supply only the raster LATTICE now (it sets the centreline
// sampling step); the surface itself comes from the height_at closure.
mapgen::MapArtifacts Lattice() {
  mapgen::MapArtifacts a;
  a.heightmap = mapgen::Field2D<float>(kN, kN, 0.0f);
  return a;
}

// A straight west-to-east reach down the middle of the map, `width` wide and
// `depth` deep throughout.
mapgen::RiverGraph StraightReach(float width, float depth, float x0 = 20.0f,
                                 float x1 = 140.0f) {
  mapgen::RiverGraph g;
  g.nodes.resize(2);
  mapgen::RiverEdge e;
  for (int i = 0; i <= 8; ++i) {
    const float t = static_cast<float>(i) / 8.0f;
    e.points_m.push_back(glm::vec2(x0 + (x1 - x0) * t, kReachZ));
    e.width_m.push_back(width);
    e.depth_m.push_back(depth);
    e.speed_m_s.push_back(1.0f);
    e.discharge_m3_s.push_back(0.1f);
  }
  g.edges.push_back(std::move(e));
  return g;
}

// A flat 10 m plain with a NARROW ditch cut along the reach: 1 m deep, and only
// 1 m to either side of the centreline. Narrower than the channels drawn on it,
// so a per-vertex drape and a flat section give different answers.
float DitchAt(float /*wx*/, float wz) {
  return std::abs(wz - kReachZ) <= 1.0f ? 9.0f : 10.0f;
}

// Bounds of the surface on one axis, so "how wide is it" is measurable.
struct Span {
  float lo = 1e30f, hi = -1e30f;
  void add(float v) {
    lo = std::min(lo, v);
    hi = std::max(hi, v);
  }
  float extent() const { return hi - lo; }
};

std::vector<glm::vec3> Build(const mapgen::RiverGraph& g,
                             std::function<float(float, float)> height_at) {
  return BuildRiverWaterTriangles(Lattice(), kWorldM, g,
                                  mapgen::build_river_arcs(g, 0.5f),
                                  std::move(height_at));
}

}  // namespace

TEST_CASE("an empty network produces no geometry", "[river_surface]") {
  CHECK(Build(mapgen::RiverGraph{}, DitchAt).empty());
}

TEST_CASE("the water surface faces up", "[river_surface]") {
  // Winding, checked as the thing it actually controls: the geometric normal.
  const std::vector<glm::vec3> tris = Build(StraightReach(4.0f, 0.2f), DitchAt);
  REQUIRE(tris.size() >= 3);
  REQUIRE(tris.size() % 3 == 0);
  for (size_t i = 0; i < tris.size(); i += 3) {
    const glm::vec3 n = glm::cross(tris[i + 1] - tris[i], tris[i + 2] - tris[i]);
    INFO("triangle " << i / 3);
    CHECK(n.y > 0.0f);
  }
}

TEST_CASE("the water sits at the carved bed plus the flow depth",
          "[river_surface]") {
  // The bed is 9 m under the reach; 0.35 m of flow puts the surface at 9.35 m
  // (plus the z-fight epsilon) -- NOT at the 10 m plain the cavity was cut into.
  const std::vector<glm::vec3> tris = Build(StraightReach(4.0f, 0.35f), DitchAt);
  REQUIRE(!tris.empty());
  for (const glm::vec3& p : tris) {
    INFO("at x=" << p.x << " z=" << p.z);
    CHECK(p.y == Catch::Approx(9.0f + 0.35f + kWaterEpsilonM).margin(1e-4));
  }
}

TEST_CASE("a cross-section is flat across its width, not draped",
          "[river_surface]") {
  // The channel is 6 m wide over a ditch only 2 m wide, so a draped surface
  // would put its outer vertices on the 10 m plain. Water is level across a
  // channel: every vertex takes the CENTRELINE's bed.
  const std::vector<glm::vec3> tris = Build(StraightReach(6.0f, 0.2f), DitchAt);
  REQUIRE(!tris.empty());
  Span across, level;
  for (const glm::vec3& p : tris) {
    across.add(p.z);
    level.add(p.y);
  }
  CHECK(across.extent() == Catch::Approx(6.0f).margin(1e-3));
  CHECK(level.extent() == Catch::Approx(0.0f).margin(1e-4));
  CHECK(level.lo == Catch::Approx(9.0f + 0.2f + kWaterEpsilonM).margin(1e-4));
}

TEST_CASE("the surface follows the bed downstream", "[river_surface]") {
  // A bed that drops 1 m per 10 m of easting. A surface built off one level, or
  // off the reach's endpoints, would pass every other test in this file.
  auto ramp = [](float wx, float /*wz*/) { return 40.0f - 0.1f * wx; };
  const std::vector<glm::vec3> tris = Build(StraightReach(4.0f, 0.2f), ramp);
  REQUIRE(!tris.empty());
  for (const glm::vec3& p : tris) {
    INFO("at x=" << p.x);
    CHECK(p.y == Catch::Approx(ramp(p.x, p.z) + 0.2f + kWaterEpsilonM)
                     .margin(1e-3));
  }
}

TEST_CASE("the drawn width is the channel's true width, with no floor",
          "[river_surface]") {
  // A 0.2 m brook is drawn 0.2 m wide. The retired kMinRibbonWidthM used to
  // inflate this to 1.5 m because a hairline on smooth ground aliased to
  // nothing; the cavity is what makes it visible now, so the surface no longer
  // lies about its width.
  for (const float w : {0.2f, 4.0f, 9.0f}) {
    const std::vector<glm::vec3> tris = Build(StraightReach(w, 0.2f), DitchAt);
    REQUIRE(!tris.empty());
    Span across;
    for (const glm::vec3& p : tris) across.add(p.z);
    INFO("width " << w);
    CHECK(across.extent() == Catch::Approx(w).margin(1e-3));
  }
}

TEST_CASE("width follows the reach downstream", "[river_surface]") {
  // A reach that swells 2 m -> 8 m must produce a surface that swells with it.
  mapgen::RiverGraph g;
  g.nodes.resize(2);
  mapgen::RiverEdge e;
  for (int i = 0; i <= 8; ++i) {
    const float t = static_cast<float>(i) / 8.0f;
    e.points_m.push_back(glm::vec2(20.0f + 120.0f * t, kReachZ));
    e.width_m.push_back(2.0f + 6.0f * t);
    e.depth_m.push_back(0.2f);
  }
  g.edges.push_back(std::move(e));

  const std::vector<glm::vec3> tris = Build(g, DitchAt);
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

TEST_CASE("a reach with no flow carries no water", "[river_surface]") {
  // Depth is the whole reason there is a surface to draw; a dry channel must
  // leave the cavity empty rather than paint a sheet across it.
  CHECK(Build(StraightReach(4.0f, 0.0f), DitchAt).empty());
  // And a reach carrying no per-point depth at all (sample_at_param returns 0
  // for an empty attribute) is the same case.
  mapgen::RiverGraph g = StraightReach(4.0f, 0.2f);
  g.edges[0].depth_m.clear();
  CHECK(Build(g, DitchAt).empty());
}

TEST_CASE("a curved reach bends rather than facets", "[river_surface]") {
  // A quarter circle of radius 40 m. The centreline should stay on that circle
  // to well under a metre -- the polyline it was fitted from has 9 vertices and
  // would cut ~1.2 m of corner.
  mapgen::RiverGraph g;
  g.nodes.resize(2);
  mapgen::RiverEdge e;
  const glm::vec2 c(80.0f, 80.0f);
  const float r = 40.0f;
  for (int i = 0; i <= 8; ++i) {
    const float a = 1.5f * static_cast<float>(i) / 8.0f;
    e.points_m.push_back(c + r * glm::vec2(std::cos(a), std::sin(a)));
    e.width_m.push_back(3.0f);
    e.depth_m.push_back(0.2f);
  }
  g.edges.push_back(std::move(e));

  const std::vector<glm::vec3> tris = Build(g, [](float, float) { return 5.0f; });
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
  const mapgen::RiverGraph g = StraightReach(4.0f, 0.2f);
  std::vector<mapgen::RiverArcChain> chains = mapgen::build_river_arcs(g, 0.5f);
  REQUIRE(chains.size() == 1);
  chains[0].edge = 7;
  CHECK(BuildRiverWaterTriangles(Lattice(), kWorldM, g, chains, DitchAt).empty());
  chains[0].edge = -1;
  CHECK(BuildRiverWaterTriangles(Lattice(), kWorldM, g, chains, DitchAt).empty());
}

TEST_CASE("degenerate inputs produce nothing rather than NaNs",
          "[river_surface]") {
  const mapgen::RiverGraph g = StraightReach(4.0f, 0.2f);
  const std::vector<mapgen::RiverArcChain> chains = mapgen::build_river_arcs(g, 0.5f);
  CHECK(BuildRiverWaterTriangles(mapgen::MapArtifacts{}, kWorldM, g, chains,
                                 DitchAt)
            .empty());
  CHECK(BuildRiverWaterTriangles(Lattice(), 0.0f, g, chains, DitchAt).empty());
  // No carve, no cavity, no water -- an absent height_at is not a reason to
  // guess a level.
  CHECK(BuildRiverWaterTriangles(Lattice(), kWorldM, g, chains, {}).empty());

  const std::vector<glm::vec3> tris = Build(g, DitchAt);
  for (const glm::vec3& p : tris) {
    CHECK(std::isfinite(p.x));
    CHECK(std::isfinite(p.y));
    CHECK(std::isfinite(p.z));
  }
}
