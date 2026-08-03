// Pure-CPU tests for the lake water surface + its buried skirt.
//
// The skirt is the whole point: the water plane is extended under the terrain
// so that vertical wave displacement (added later) cannot open a gap at the
// shoreline. Two ways to get that wrong are load-bearing here -- the skirt
// stopping before it is buried, and the skirt spilling over a ridge into a
// lower basin where it would hang in the air as a visible sheet.

#include <catch_amalgamated.hpp>

#include <algorithm>
#include <cmath>
#include <set>

#include "mapgen/patch_data.hpp"
#include "mapview/lake_surface.hpp"

using namespace badlands;

namespace {

// A flat map at height `ground`, with a rectangular lake carved to `bed`.
mapgen::PatchData MakeMap(int n, float ground, int lx0, int lz0, int lx1,
                             int lz1, float bed, float level) {
  mapgen::PatchData a;
  a.height = mapgen::Field2D<float>(n, n, ground);
  a.lake_id = mapgen::Field2D<int32_t>(n, n, -1);
  for (int z = lz0; z < lz1; ++z) {
    for (int x = lx0; x < lx1; ++x) {
      a.height.at(x, z) = bed;
      a.lake_id.at(x, z) = 0;
    }
  }
  mapgen::LakeInfo l;
  l.level_m = level;
  a.lakes.push_back(l);
  return a;
}

std::set<int> DistinctY(const std::vector<glm::vec3>& tris) {
  std::set<int> ys;
  for (const glm::vec3& v : tris) {
    ys.insert(static_cast<int>(std::lround(v.y * 100.0f)));
  }
  return ys;
}

}  // namespace

TEST_CASE("a map with no lakes yields no water", "[water]") {
  mapgen::PatchData a;
  a.height = mapgen::Field2D<float>(8, 8, 10.0f);
  a.lake_id = mapgen::Field2D<int32_t>(8, 8, -1);
  CHECK(BuildLakeSurfaceTriangles(a, 8.0f).empty());
}

TEST_CASE("every water vertex sits at its lake's level", "[water]") {
  // 1 m texels. Bank at 12 m, well above the 10 m level, so the skirt reaches
  // burial depth after a single ring.
  const mapgen::PatchData a = MakeMap(16, 12.0f, 6, 6, 10, 10, 8.0f, 10.0f);
  const std::vector<glm::vec3> tris = BuildLakeSurfaceTriangles(a, 16.0f);
  REQUIRE(!tris.empty());
  for (const glm::vec3& v : tris) CHECK(v.y == Catch::Approx(10.0f));
}

TEST_CASE("the surface overlaps the shore", "[water]") {
  // The lake spans x in [6, 10). The emitted surface must reach further --
  // that overlap is what a later wave displacement hides its edge in.
  const mapgen::PatchData a = MakeMap(16, 12.0f, 6, 6, 10, 10, 8.0f, 10.0f);
  const std::vector<glm::vec3> tris = BuildLakeSurfaceTriangles(a, 16.0f);
  float min_x = 1e9f, max_x = -1e9f;
  for (const glm::vec3& v : tris) {
    min_x = std::min(min_x, v.x);
    max_x = std::max(max_x, v.x);
  }
  CHECK(min_x < 6.0f);
  CHECK(max_x > 10.0f);
}

TEST_CASE("the skirt never runs past its cap", "[water]") {
  // A dead-flat shore exactly AT the water level never reaches burial depth,
  // so the cap is the only thing stopping the search.
  const mapgen::PatchData a = MakeMap(64, 10.0f, 28, 28, 36, 36, 8.0f, 10.0f);
  const std::vector<glm::vec3> tris = BuildLakeSurfaceTriangles(a, 64.0f);
  REQUIRE(!tris.empty());
  float min_x = 1e9f, max_x = -1e9f;
  for (const glm::vec3& v : tris) {
    min_x = std::min(min_x, v.x);
    max_x = std::max(max_x, v.x);
  }
  // Lake spans [28, 36); the skirt may add at most kMaxSkirtM on each side
  // (+1 m for the quad the outermost texel contributes).
  CHECK(min_x >= 28.0f - kMaxSkirtM - 1.0f);
  CHECK(max_x <= 36.0f + kMaxSkirtM + 1.0f);
  // ...and it really did run out to the cap rather than stopping early.
  CHECK(min_x < 28.0f - kMaxSkirtM + 2.0f);
}

TEST_CASE("the skirt does not spill into a lower dry basin", "[water]") {
  // Lake at level 10 on the left, a ridge at 11 m, then dry ground at 5 m --
  // well BELOW the water level. Water reaching the low ground would hang in
  // the air as a visible sheet.
  mapgen::PatchData a = MakeMap(24, 11.0f, 4, 4, 10, 20, 8.0f, 10.0f);
  for (int z = 0; z < 24; ++z) {
    for (int x = 12; x < 24; ++x) a.height.at(x, z) = 5.0f;
  }
  const std::vector<glm::vec3> tris = BuildLakeSurfaceTriangles(a, 24.0f);
  REQUIRE(!tris.empty());
  float max_x = -1e9f;
  for (const glm::vec3& v : tris) max_x = std::max(max_x, v.x);
  CHECK(max_x <= 12.0f);
}

TEST_CASE("two lakes keep their own levels", "[water]") {
  mapgen::PatchData a = MakeMap(32, 20.0f, 4, 4, 8, 8, 8.0f, 10.0f);
  // Second lake, far from the first, at a different level.
  for (int z = 20; z < 24; ++z) {
    for (int x = 20; x < 24; ++x) {
      a.height.at(x, z) = 12.0f;
      a.lake_id.at(x, z) = 1;
    }
  }
  mapgen::LakeInfo second;
  second.level_m = 15.0f;
  a.lakes.push_back(second);

  const std::set<int> ys = DistinctY(BuildLakeSurfaceTriangles(a, 32.0f));
  CHECK(ys.size() == 2);
  CHECK(ys.count(1000) == 1);  // 10.00 m
  CHECK(ys.count(1500) == 1);  // 15.00 m
}
