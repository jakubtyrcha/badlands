// Catch2 invariant suite for the terrain cluster-LOD DAG build
// (terrain_clusters.cpp). Pure CPU: synthetic Field2D heightfields + a two-biome
// split, no noiser scripts, no GPU. Pins the load-bearing seamlessness
// invariants from the spec (docs/superpowers/specs/
// 2026-07-19-terrain-cluster-lod-design.md, Verification section):
//   1. error monotonicity along the DAG,
//   2. sibling error/sphere sharing + parent-sphere nesting,
//   3. crack-freeness (bitwise-equal shared boundary vertices),
//   4/5. grid arithmetic incl. non-square + a rebuild with non-default constants.

#include <array>
#include <cmath>
#include <cstring>
#include <map>
#include <set>
#include <vector>

#include <catch_amalgamated.hpp>
#include <glm/glm.hpp>

#include "game/geometry/terrain_clusters.hpp"
#include "mapgen/biomes.hpp"
#include "mapgen/field2d.hpp"

using badlands::BuildTerrainClusterDag;
using badlands::kNoGroup;
using badlands::TerrainClusterDag;
using badlands::TerrainClusterParams;

namespace {

using badlands::mapgen::Field2D;

// A ridged sine heightfield: enough curvature that simplification actually
// collapses interior verts and reports non-trivial error, so the monotonicity
// and sphere checks exercise real numbers rather than a flat plane.
Field2D<float> MakeHeightmap(int w, int h) {
  Field2D<float> hm(w, h);
  for (int j = 0; j < h; ++j) {
    for (int i = 0; i < w; ++i) {
      const float fx = static_cast<float>(i);
      const float fz = static_cast<float>(j);
      const float base = 6.0f * std::sin(fx * 0.06f) * std::cos(fz * 0.05f);
      const float ridge = 3.0f * std::abs(std::sin(fx * 0.11f + fz * 0.03f));
      hm.at(i, j) = base + ridge;
    }
  }
  return hm;
}

// Two-biome split (diagonal), so vertex color varies and the attribute metric is
// actually engaged; values are Biome enum indices.
Field2D<uint8_t> MakeBiomes(int w, int h) {
  Field2D<uint8_t> b(w, h);
  for (int j = 0; j < h; ++j)
    for (int i = 0; i < w; ++i)
      b.at(i, j) = static_cast<uint8_t>(
          (i + j < (w + h) / 2) ? badlands::mapgen::Biome::Forest
                                : badlands::mapgen::Biome::Hills);
  return b;
}

// --- bitwise vertex helpers -------------------------------------------------
// The crack-free invariant is BITWISE equality, so compare the raw float bits.

std::array<uint32_t, 8> Record(const TerrainClusterDag& dag, uint32_t vidx) {
  std::array<uint32_t, 8> r{};
  std::memcpy(r.data(),
              &dag.vertices[static_cast<size_t>(vidx) *
                            badlands::kFloatsPerClusterVertex],
              sizeof(r));
  return r;
}

std::array<uint32_t, 3> PosBits(const std::array<uint32_t, 8>& rec) {
  return {rec[0], rec[1], rec[2]};
}

glm::vec3 PosOf(const std::array<uint32_t, 8>& rec) {
  glm::vec3 p;
  uint32_t b[3] = {rec[0], rec[1], rec[2]};
  std::memcpy(&p, b, sizeof(p));
  return p;
}

// Sphere containment with a small absolute epsilon (float accumulation in the
// centroid/radius reduction).
bool SphereContains(const glm::vec4& outer, const glm::vec4& inner) {
  const float d = glm::length(glm::vec3(outer) - glm::vec3(inner));
  return d + inner.w <= outer.w + 1e-3f;
}

// --- invariant checks (reused across the map variants) ----------------------

// (1) Every group's error is >= each consumed child's own-group error.
void CheckMonotonicErrors(const TerrainClusterDag& dag) {
  for (const auto& g : dag.groups) {
    for (uint32_t k = 0; k < g.child_count; ++k) {
      const uint32_t cidx = dag.group_children[g.first_child + k];
      REQUIRE(g.error_m + 1e-4f >= dag.ClusterOwnError(dag.clusters[cidx]));
    }
  }
}

// (2) Siblings (same own_group) report identical error+sphere, and each group's
// sphere contains every consumed child's own sphere.
void CheckSpheres(const TerrainClusterDag& dag) {
  // sibling sharing: a cluster's own error/sphere IS its own_group's, so any two
  // clusters with the same own_group trivially agree — assert the mechanism by
  // checking the group sphere nests each child.
  for (const auto& g : dag.groups) {
    for (uint32_t k = 0; k < g.child_count; ++k) {
      const uint32_t cidx = dag.group_children[g.first_child + k];
      REQUIRE(SphereContains(g.sphere, dag.ClusterOwnSphere(dag.clusters[cidx])));
    }
  }
}

// (3a) Agreement: within a level, any two vertices sharing a bitwise position
// share the whole 8-float record. A mismatch here is exactly a crack.
void CheckSeamAgreement(const TerrainClusterDag& dag) {
  std::map<int, std::map<std::array<uint32_t, 3>, std::array<uint32_t, 8>>> seen;
  for (const auto& c : dag.clusters) {
    auto& level_map = seen[c.level];
    for (uint32_t v = 0; v < c.vertex_count; ++v) {
      const auto rec = Record(dag, c.first_vertex + v);
      const auto pos = PosBits(rec);
      auto it = level_map.find(pos);
      if (it == level_map.end())
        level_map.emplace(pos, rec);
      else
        REQUIRE(it->second == rec);
    }
  }
}

// (3b) Completeness: for adjacent groups at a level (footprints sharing an edge
// line), the SETS of boundary-vertex records on that line are identical — so a
// coarse neighbor can't drop a vertex the finer side keeps (a T-junction).
void CheckSeamCompleteness(const TerrainClusterDag& dag) {
  // Bucket output clusters by producing group.
  std::map<int, std::vector<uint32_t>> outputs_of;  // group -> cluster indices
  for (uint32_t i = 0; i < dag.clusters.size(); ++i)
    if (dag.clusters[i].own_group != kNoGroup)
      outputs_of[dag.clusters[i].own_group].push_back(i);

  auto edge_records = [&](int gidx, bool vertical, float line, float lo,
                          float hi) {
    std::set<std::array<uint32_t, 8>> out;
    for (uint32_t cidx : outputs_of[gidx]) {
      const auto& c = dag.clusters[cidx];
      for (uint32_t v = 0; v < c.vertex_count; ++v) {
        const auto rec = Record(dag, c.first_vertex + v);
        const glm::vec3 p = PosOf(rec);
        const float on = vertical ? p.x : p.z;
        const float along = vertical ? p.z : p.x;
        if (on == line && along >= lo && along <= hi) out.insert(rec);
      }
    }
    return out;
  };

  const int ng = static_cast<int>(dag.groups.size());
  for (int a = 0; a < ng; ++a) {
    for (int b = a + 1; b < ng; ++b) {
      const auto& ga = dag.groups[a];
      const auto& gb = dag.groups[b];
      if (ga.level != gb.level) continue;
      const glm::vec4 fa = ga.footprint, fb = gb.footprint;
      // vertical shared edge: a's right == b's left (or vice versa), z overlap.
      auto try_edge = [&](bool vertical) {
        const float a0 = vertical ? fa.x : fa.y;  // near coord on `on` axis
        const float a1 = vertical ? fa.z : fa.w;
        const float b0 = vertical ? fb.x : fb.y;
        const float b1 = vertical ? fb.z : fb.w;
        float line;
        if (a1 == b0)
          line = a1;
        else if (b1 == a0)
          line = b1;
        else
          return;
        // overlap on the perpendicular axis
        const float pa0 = vertical ? fa.y : fa.x;
        const float pa1 = vertical ? fa.w : fa.z;
        const float pb0 = vertical ? fb.y : fb.x;
        const float pb1 = vertical ? fb.w : fb.z;
        const float lo = std::max(pa0, pb0);
        const float hi = std::min(pa1, pb1);
        if (lo >= hi) return;  // touch at a corner only, no shared segment
        REQUIRE(edge_records(a, vertical, line, lo, hi) ==
                edge_records(b, vertical, line, lo, hi));
      };
      try_edge(true);
      try_edge(false);
    }
  }
}

// Coverage: the union of all leaf (level-0) clusters spans the full map extent.
void CheckLeafCoverage(const TerrainClusterDag& dag, int w, int h) {
  glm::vec3 lo(1e9f), hi(-1e9f);
  int leaves = 0;
  for (const auto& c : dag.clusters) {
    if (c.level != 0) continue;
    ++leaves;
    lo = glm::min(lo, c.bounds.min);
    hi = glm::max(hi, c.bounds.max);
  }
  REQUIRE(leaves > 0);
  REQUIRE(lo.x == Catch::Approx(0.0f));
  REQUIRE(lo.z == Catch::Approx(0.0f));
  REQUIRE(hi.x == Catch::Approx(static_cast<float>(w)));
  REQUIRE(hi.z == Catch::Approx(static_cast<float>(h)));
}

int LeafCount(const TerrainClusterDag& dag) {
  int n = 0;
  for (const auto& c : dag.clusters)
    if (c.level == 0) ++n;
  return n;
}

void CheckAllInvariants(const TerrainClusterDag& dag) {
  CheckMonotonicErrors(dag);
  CheckSpheres(dag);
  CheckSeamAgreement(dag);
  CheckSeamCompleteness(dag);
}

}  // namespace

TEST_CASE("terrain cluster DAG: monotonic errors + nesting spheres", "[terrain_clusters]") {
  const int w = 64, h = 64;
  const auto dag = BuildTerrainClusterDag(MakeHeightmap(w, h), MakeBiomes(w, h));
  REQUIRE(dag.clusters.size() > 0);
  REQUIRE(dag.groups.size() > 0);
  CheckMonotonicErrors(dag);
  CheckSpheres(dag);
}

TEST_CASE("terrain cluster DAG: crack-free shared boundaries", "[terrain_clusters]") {
  const int w = 64, h = 64;
  const auto dag = BuildTerrainClusterDag(MakeHeightmap(w, h), MakeBiomes(w, h));
  CheckSeamAgreement(dag);
  CheckSeamCompleteness(dag);
}

TEST_CASE("terrain cluster DAG: converges to a single root", "[terrain_clusters]") {
  const int w = 64, h = 64;
  const auto dag = BuildTerrainClusterDag(MakeHeightmap(w, h), MakeBiomes(w, h));
  int roots = 0;
  for (const auto& c : dag.clusters)
    if (c.parent_group == kNoGroup) ++roots;
  REQUIRE(roots == 1);
}

TEST_CASE("terrain cluster DAG: grid arithmetic", "[terrain_clusters]") {
  SECTION("512x512 -> 64x64 = 4096 leaves (arithmetic only)") {
    // Flat map keeps this cheap: it's an arithmetic check, not a shape check.
    Field2D<float> flat(512, 512, 0.0f);
    Field2D<uint8_t> biome(512, 512,
                           static_cast<uint8_t>(badlands::mapgen::Biome::Plains));
    const auto dag = BuildTerrainClusterDag(flat, biome);
    REQUIRE(LeafCount(dag) == 64 * 64);
    CheckLeafCoverage(dag, 512, 512);
  }

  SECTION("non-square 100x60 covers the full extent + stays crack-free") {
    const int w = 100, h = 60;
    const auto dag = BuildTerrainClusterDag(MakeHeightmap(w, h), MakeBiomes(w, h));
    // ceil(100/8)=13, ceil(60/8)=8 leaf tiles.
    REQUIRE(LeafCount(dag) == 13 * 8);
    CheckLeafCoverage(dag, w, h);
    CheckAllInvariants(dag);
  }
}

TEST_CASE("terrain cluster DAG: non-default constants hold invariants", "[terrain_clusters]") {
  const int w = 64, h = 60;
  TerrainClusterParams params;
  params.tile_quads = 4;  // half the default leaf edge
  const auto dag =
      BuildTerrainClusterDag(MakeHeightmap(w, h), MakeBiomes(w, h), params);
  // ceil(64/4)=16, ceil(60/4)=15 leaf tiles.
  REQUIRE(LeafCount(dag) == 16 * 15);
  CheckLeafCoverage(dag, w, h);
  CheckAllInvariants(dag);
}
