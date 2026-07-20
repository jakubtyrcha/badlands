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
#include <limits>
#include <map>
#include <set>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <catch_amalgamated.hpp>
#include <glm/glm.hpp>

#include "game/geometry/terrain_clusters.hpp"
#include "mapgen/biomes.hpp"
#include "mapgen/field2d.hpp"

using badlands::BuildTerrainClusterDag;
using badlands::kNoGroup;
using badlands::SelectClusters;
using badlands::TerrainCluster;
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

// --- cut-validity helpers (test 4) ------------------------------------------
// The build produces a clean group tree: every cluster a group emits is consumed
// by the SAME parent group, so a leaf has a single chain of ancestor GROUPS
// (g0 = leaf.parent_group, g1 = the group consuming g0's outputs, ...). All of a
// group's emitted clusters share its LOD error+sphere, hence one selection
// decision per group — so coverage is counted per selected GROUP, not per raw
// cluster (a group emits kGroupSplitCount siblings that are selected in lockstep;
// counting them individually would double-count the same cut level).

// group index -> the parent group consuming its outputs (kNoGroup at the root).
std::unordered_map<int, int> GroupParentGroup(const TerrainClusterDag& dag) {
  std::unordered_map<int, int> out;
  for (const auto& c : dag.clusters) {
    if (c.own_group == kNoGroup) continue;
    auto it = out.find(c.own_group);
    if (it == out.end())
      out.emplace(c.own_group, c.parent_group);
    else
      REQUIRE(it->second == c.parent_group);  // clean tree: single parent group
  }
  return out;
}

// group index -> whether its emitted clusters are selected. Asserts all-or-none
// (group-consistent refinement: siblings share the identical own/parent test).
std::unordered_map<int, bool> GroupSelected(
    const TerrainClusterDag& dag, const std::unordered_set<uint32_t>& selected) {
  std::unordered_map<int, int> count;  // group -> #selected emitted clusters
  std::unordered_map<int, int> total;  // group -> #emitted clusters
  for (uint32_t i = 0; i < dag.clusters.size(); ++i) {
    const int g = dag.clusters[i].own_group;
    if (g == kNoGroup) continue;
    total[g]++;
    if (selected.count(i)) count[g]++;
  }
  std::unordered_map<int, bool> out;
  for (const auto& [g, n] : total) {
    const int sel = count.count(g) ? count[g] : 0;
    REQUIRE((sel == 0 || sel == n));  // all emitted siblings agree
    out[g] = sel > 0;
  }
  return out;
}

// The two load-bearing cut properties, for one camera+tau:
//  (a) antichain — no selected cluster is an ancestor of another selected one
//      (walk parent_group chains up: no ancestor group may be selected).
//  (b) exact cover — every leaf is covered by exactly one cut level: the leaf
//      itself, or exactly one selected group along its ancestor-group chain.
void CheckCutValidity(const TerrainClusterDag& dag, glm::vec3 cam, float fov_deg,
                      float screen_h, float tau) {
  std::vector<uint32_t> sel_vec;
  SelectClusters(dag, cam, fov_deg, screen_h, tau, sel_vec);
  const std::unordered_set<uint32_t> selected(sel_vec.begin(), sel_vec.end());
  REQUIRE(sel_vec.size() == selected.size());  // no duplicates

  const auto parent_group = GroupParentGroup(dag);
  const auto group_selected = GroupSelected(dag, selected);
  auto is_group_selected = [&](int g) {
    auto it = group_selected.find(g);
    return it != group_selected.end() && it->second;
  };

  // (a) antichain: from each selected cluster, no ancestor group is selected.
  for (uint32_t cidx : sel_vec) {
    int g = dag.clusters[cidx].parent_group;
    while (g != kNoGroup) {
      REQUIRE_FALSE(is_group_selected(g));
      auto it = parent_group.find(g);
      g = (it == parent_group.end()) ? kNoGroup : it->second;
    }
  }

  // (b) exact cover: each leaf has exactly one selected level (self or one
  // ancestor group).
  for (uint32_t i = 0; i < dag.clusters.size(); ++i) {
    const TerrainCluster& c = dag.clusters[i];
    if (c.level != 0) continue;
    int covers = selected.count(i) ? 1 : 0;
    int g = c.parent_group;
    while (g != kNoGroup) {
      if (is_group_selected(g)) ++covers;
      auto it = parent_group.find(g);
      g = (it == parent_group.end()) ? kNoGroup : it->second;
    }
    REQUIRE(covers == 1);
  }
}

uint64_t SelectedTriCount(const TerrainClusterDag& dag,
                          const std::vector<uint32_t>& sel) {
  uint64_t tris = 0;
  for (uint32_t i : sel) tris += dag.clusters[i].index_count / 3;
  return tris;
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

TEST_CASE("terrain cluster DAG: SelectClusters cut validity", "[terrain_clusters]") {
  const float fov = 45.0f;
  const float screen_h = 1080.0f;
  const float taus[] = {0.5f, 1.5f, 4.0f, 16.0f};

  struct MapCase {
    int w, h;
    std::vector<glm::vec3> cameras;
  };
  const std::vector<MapCase> cases = {
      // 64x64: near ground, mid, high overhead, outside the map.
      {64, 64,
       {{32, 4, 32}, {32, 40, 90}, {32, 400, 32}, {-120, 60, -120}}},
      // non-square 100x60.
      {100, 60,
       {{50, 4, 30}, {50, 45, 110}, {50, 500, 30}, {220, 80, 140}}},
  };

  for (const MapCase& mc : cases) {
    const auto dag =
        BuildTerrainClusterDag(MakeHeightmap(mc.w, mc.h), MakeBiomes(mc.w, mc.h));
    for (const glm::vec3& cam : mc.cameras) {
      // Cut validity holds at every tau.
      for (float tau : taus) {
        CheckCutValidity(dag, cam, fov, screen_h, tau);
      }
      // Monotone sanity: raising tau (coarser threshold) never increases the
      // selected triangle count for a fixed camera. Proven: each leaf's cut
      // level moves same-or-coarser as tau rises, and a coarser cluster carries
      // fewer triangles over the same footprint. (Cluster COUNT is not asserted
      // monotone — it is not guaranteed level-by-level; triangles are the
      // meaningful, theory-backed quantity.)
      uint64_t prev = UINT64_MAX;
      for (float tau : taus) {
        std::vector<uint32_t> sel;
        SelectClusters(dag, cam, fov, screen_h, tau, sel);
        const uint64_t tris = SelectedTriCount(dag, sel);
        REQUIRE(tris <= prev);
        prev = tris;
      }
    }
  }
}

// Any tau — including degenerate ones (negative, 0, NaN, +inf, absurdly large)
// — must still yield a valid, NON-EMPTY exact cover, because SelectClusters
// sanitizes tau into [kMinTauPx, kMaxTauPx]. Without the clamp, each of these
// produces an EMPTY cut (blank terrain): the finding this guards. RED before the
// clamp (cut.empty()), GREEN after.
TEST_CASE("terrain cluster DAG: SelectClusters valid non-empty cover for any tau",
          "[terrain_clusters]") {
  const int w = 64, h = 64;
  const auto dag = BuildTerrainClusterDag(MakeHeightmap(w, h), MakeBiomes(w, h));
  const glm::vec3 cam(32.0f, 80.0f, 32.0f);
  const float fov = 45.0f, screen_h = 900.0f;
  const float nan = std::numeric_limits<float>::quiet_NaN();
  const float inf = std::numeric_limits<float>::infinity();
  std::vector<uint32_t> cut;
  for (float tau : {-1.0f, 0.0f, nan, inf, 1e30f, 1.5f}) {
    CAPTURE(tau);
    SelectClusters(dag, cam, fov, screen_h, tau, cut);
    REQUIRE_FALSE(cut.empty());
    // Same antichain + exact-leaf-cover checks the cut-validity test uses.
    CheckCutValidity(dag, cam, fov, screen_h, tau);
  }
}

// --- determinism gate (parallel build == serial build, bitwise) -------------
// Compares two DAGs field-by-field on raw bit patterns (not float ==, so a
// -0.0/0.0 or any last-bit drift is caught). This is the non-negotiable gate for
// the ParallelFor build: the parallel phase only runs the pure per-group
// weld+simplify+split; emission stays serial, so the output MUST be identical.

template <typename T>
bool BitsEqual(const T& a, const T& b) {
  return std::memcmp(&a, &b, sizeof(T)) == 0;
}

void RequireDagsBitIdentical(const TerrainClusterDag& a,
                             const TerrainClusterDag& b) {
  REQUIRE(a.level_count == b.level_count);
  REQUIRE(a.map_quads_x == b.map_quads_x);
  REQUIRE(a.map_quads_z == b.map_quads_z);

  REQUIRE(a.vertices.size() == b.vertices.size());
  REQUIRE(std::memcmp(a.vertices.data(), b.vertices.data(),
                      a.vertices.size() * sizeof(float)) == 0);

  REQUIRE(a.indices == b.indices);
  REQUIRE(a.group_children == b.group_children);

  REQUIRE(a.clusters.size() == b.clusters.size());
  for (size_t i = 0; i < a.clusters.size(); ++i) {
    const TerrainCluster& ca = a.clusters[i];
    const TerrainCluster& cb = b.clusters[i];
    REQUIRE(ca.first_index == cb.first_index);
    REQUIRE(ca.index_count == cb.index_count);
    REQUIRE(ca.first_vertex == cb.first_vertex);
    REQUIRE(ca.vertex_count == cb.vertex_count);
    REQUIRE(ca.own_group == cb.own_group);
    REQUIRE(ca.parent_group == cb.parent_group);
    REQUIRE(ca.level == cb.level);
    REQUIRE(BitsEqual(ca.bounds.min, cb.bounds.min));
    REQUIRE(BitsEqual(ca.bounds.max, cb.bounds.max));
  }

  REQUIRE(a.groups.size() == b.groups.size());
  for (size_t i = 0; i < a.groups.size(); ++i) {
    const auto& ga = a.groups[i];
    const auto& gb = b.groups[i];
    REQUIRE(BitsEqual(ga.error_m, gb.error_m));
    REQUIRE(BitsEqual(ga.sphere, gb.sphere));
    REQUIRE(BitsEqual(ga.footprint, gb.footprint));
    REQUIRE(ga.level == gb.level);
    REQUIRE(ga.first_child == gb.first_child);
    REQUIRE(ga.child_count == gb.child_count);
  }
}

TEST_CASE("terrain cluster DAG: parallel build == serial build (bitwise)",
          "[terrain_clusters]") {
  auto build = [](int w, int h, bool parallel) {
    TerrainClusterParams params;
    params.parallel_build = parallel;
    return BuildTerrainClusterDag(MakeHeightmap(w, h), MakeBiomes(w, h), params);
  };

  SECTION("64x64") {
    RequireDagsBitIdentical(build(64, 64, false), build(64, 64, true));
  }
  SECTION("non-square 100x60") {
    RequireDagsBitIdentical(build(100, 60, false), build(100, 60, true));
  }
  SECTION("parallel is reproducible run-to-run") {
    // A second parallel build must match the first bitwise too — guards against
    // any scheduling-dependent nondeterminism the serial reference can't expose.
    RequireDagsBitIdentical(build(64, 64, true), build(64, 64, true));
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
