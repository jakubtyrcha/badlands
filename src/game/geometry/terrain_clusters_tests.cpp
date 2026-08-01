// Catch2 invariant suite for the terrain cluster-LOD DAG build
// (terrain_clusters.cpp). Pure CPU: synthetic MapData heightfields + a two-biome
// split, no GPU. Pins the load-bearing seamlessness invariants from the spec
// (docs/superpowers/specs/2026-07-19-terrain-cluster-lod-design.md,
// Verification section):
//   1. error monotonicity along the DAG,
//   2. sibling error/sphere sharing + parent-sphere nesting,
//   3. crack-freeness (bitwise-equal shared boundary vertices),
//   4/5. grid arithmetic incl. non-square + a rebuild with non-default constants.
//
// DEPTH-AGNOSTIC BY CONSTRUCTION. Nothing here may read `level` as a resolution
// tier or as "is this a leaf" — locally refined tiles will give the DAG mixed
// depth, and any such check would then silently skip or miscount at exactly the
// new seams. Leafness is `own_group == kNoGroup` (the header's sentinel), seam
// checks span the whole DAG rather than one level, and the only level-dependent
// datum in a vertex record (the lod_level debug byte) is masked out.

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
#include "game/map/map_data.hpp"
#include "mapgen/biomes.hpp"

using badlands::BuildTerrainClusterDag;
using badlands::kNoGroup;
using badlands::SelectClusters;
using badlands::TerrainCluster;
using badlands::TerrainClusterDag;
using badlands::TerrainClusterParams;

namespace {

using badlands::MapData;

// The DAG's leaf grid is the MapData lattice: (nodes-1) quads over `nodes`
// vertices per axis. The suite's `w`/`h` are QUAD counts (matching the old
// heightmap width/height), so a w x h map is nodes = (w+1) x (h+1) at 1 m
// spacing -- extent [0, w] x [0, h], preserving every leaf-count / coverage
// assertion. One-hot slices, so WeightsAtNode(i,j).Dominant() is a single biome.

// A ridged sine heightfield: enough curvature that simplification actually
// collapses interior verts and reports non-trivial error, so the monotonicity
// and sphere checks exercise real numbers rather than a flat plane. Diagonal
// two-biome split so vertex color varies and the attribute metric is engaged.
MapData MakeMapData(int w, int h) {
  const int nodes_x = w + 1, nodes_z = h + 1;
  MapData m(nodes_x, nodes_z, 1.0f);
  for (int j = 0; j < nodes_z; ++j) {
    for (int i = 0; i < nodes_x; ++i) {
      const float fx = static_cast<float>(i);
      const float fz = static_cast<float>(j);
      const float base = 6.0f * std::sin(fx * 0.06f) * std::cos(fz * 0.05f);
      const float ridge = 3.0f * std::abs(std::sin(fx * 0.11f + fz * 0.03f));
      m.mutable_height(i, j) = base + ridge;
      const badlands::mapgen::Biome b =
          (i + j < (nodes_x + nodes_z) / 2) ? badlands::mapgen::Biome::Forest
                                            : badlands::mapgen::Biome::Hills;
      m.mutable_slice(static_cast<int>(b), i, j) = 255;
    }
  }
  return m;
}

// Flat single-biome map for the cheap grid-arithmetic checks.
MapData MakeFlatMapData(int w, int h, badlands::mapgen::Biome biome) {
  const int nodes_x = w + 1, nodes_z = h + 1;
  MapData m(nodes_x, nodes_z, 1.0f);
  for (int j = 0; j < nodes_z; ++j)
    for (int i = 0; i < nodes_x; ++i)
      m.mutable_slice(static_cast<int>(biome), i, j) = 255;
  return m;
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

// The crack-relevant payload of a vertex record. Word 7 is the meta word,
// PackU8x4{biome_id, cluster_hash_byte, lod_level, 0}: EmitCluster stamps its
// bits 16..23 with the EMITTING LEVEL as a debug tint, so that byte is
// level-varying BY CONSTRUCTION and can never participate in a cross-level
// comparison. Mask it out; everything else — position, normal, packed color,
// biome id, and the position-hash byte (a pure function of position) — is the
// payload a crack would perturb, and it must agree everywhere in the DAG.
// Masking loses nothing WITHIN a level (the byte is constant across a level), so
// every check below is strictly stronger than the per-level form it replaces.
constexpr uint32_t kLodLevelByteMask = 0x00FF0000u;

std::array<uint32_t, 8> CrackRecord(const TerrainClusterDag& dag, uint32_t vidx) {
  auto r = Record(dag, vidx);
  r[7] &= ~kLodLevelByteMask;
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

// (3a) Agreement: ACROSS THE WHOLE DAG, any two vertices sharing a bitwise
// position share the whole crack-relevant record. A mismatch here is exactly a
// crack. Deliberately NOT bucketed per level: simplification never CREATES
// vertices, it only removes them, so every vertex at every level is a bitwise
// copy of some leaf vertex, and the leaf lattice is position-injective. Same
// position therefore implies the same payload globally, not merely within one
// level — which also keeps this check meaningful once the DAG has mixed depth
// and `level` no longer names a resolution tier.
void CheckSeamAgreement(const TerrainClusterDag& dag) {
  std::map<std::array<uint32_t, 3>, std::array<uint32_t, 8>> seen;
  for (const auto& c : dag.clusters) {
    for (uint32_t v = 0; v < c.vertex_count; ++v) {
      const auto rec = CrackRecord(dag, c.first_vertex + v);
      const auto pos = PosBits(rec);
      auto it = seen.find(pos);
      if (it == seen.end())
        seen.emplace(pos, rec);
      else
        REQUIRE(it->second == rec);
    }
  }
}

// (3b) Completeness: for ANY two adjacent groups (footprints sharing an edge
// segment), WHATEVER THEIR LEVELS, the SETS of boundary-vertex records on that
// line are identical — so a coarse neighbor can't drop a vertex the finer side
// keeps (a T-junction). The invariant is level-free: LockVertex locks every
// vertex lying on an interior footprint line, and locked vertices survive
// simplification bitwise, so any two groups whose footprints share a segment
// carry identical vertex-record sets on the overlap regardless of the levels
// they sit at. That is exactly the case a mixed-depth DAG creates, and the case
// a level filter here would silently skip.
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
        const auto rec = CrackRecord(dag, c.first_vertex + v);
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

// Coverage: the union of all leaf clusters spans the full map extent.
void CheckLeafCoverage(const TerrainClusterDag& dag, int w, int h) {
  glm::vec3 lo(1e9f), hi(-1e9f);
  int leaves = 0;
  for (const auto& c : dag.clusters) {
    // A leaf is a cluster no group produced (header's kNoGroup sentinel); level
    // 0 will no longer mean leaf once the DAG has locally refined tiles.
    if (c.own_group != kNoGroup) continue;
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
    // Leaf = produced by no group; level 0 will no longer mean leaf once the DAG
    // has locally refined tiles.
    if (c.own_group == kNoGroup) ++n;
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
    // Leaf = produced by no group; level 0 will no longer mean leaf once the DAG
    // has locally refined tiles.
    if (c.own_group != kNoGroup) continue;
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
  const auto dag = BuildTerrainClusterDag(MakeMapData(w, h));
  REQUIRE(dag.clusters.size() > 0);
  REQUIRE(dag.groups.size() > 0);
  CheckMonotonicErrors(dag);
  CheckSpheres(dag);
}

TEST_CASE("terrain cluster DAG: crack-free shared boundaries", "[terrain_clusters]") {
  const int w = 64, h = 64;
  const auto dag = BuildTerrainClusterDag(MakeMapData(w, h));
  CheckSeamAgreement(dag);
  CheckSeamCompleteness(dag);
}

TEST_CASE("terrain cluster DAG: converges to a single root", "[terrain_clusters]") {
  const int w = 64, h = 64;
  const auto dag = BuildTerrainClusterDag(MakeMapData(w, h));
  int roots = 0;
  for (const auto& c : dag.clusters)
    if (c.parent_group == kNoGroup) ++roots;
  REQUIRE(roots == 1);
}

TEST_CASE("terrain cluster DAG: grid arithmetic", "[terrain_clusters]") {
  SECTION("512x512 -> 64x64 = 4096 leaves (arithmetic only)") {
    // Flat map keeps this cheap: it's an arithmetic check, not a shape check.
    const auto dag =
        BuildTerrainClusterDag(MakeFlatMapData(512, 512, badlands::mapgen::Biome::Plains));
    REQUIRE(LeafCount(dag) == 64 * 64);
    CheckLeafCoverage(dag, 512, 512);
  }

  SECTION("non-square 100x60 covers the full extent + stays crack-free") {
    const int w = 100, h = 60;
    const auto dag = BuildTerrainClusterDag(MakeMapData(w, h));
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
        BuildTerrainClusterDag(MakeMapData(mc.w, mc.h));
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
  const auto dag = BuildTerrainClusterDag(MakeMapData(w, h));
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
    return BuildTerrainClusterDag(MakeMapData(w, h), params);
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
      BuildTerrainClusterDag(MakeMapData(w, h), params);
  // ceil(64/4)=16, ceil(60/4)=15 leaf tiles.
  REQUIRE(LeafCount(dag) == 16 * 15);
  CheckLeafCoverage(dag, w, h);
  CheckAllInvariants(dag);
}

// --- local subdivision (TerrainDetailField) ----------------------------------
//
// The detail feature's contract, pinned from four directions:
//   1. absence is EXACT: null / all-zero fields reproduce today's DAG bitwise,
//      and cold tiles of a detailed build stay byte-identical per tile;
//   2. presence is WATERTIGHT: the leaf set (and every selected cut) is
//      edge-manifold -- a T-junction, a dropped fan triangle, or a 1-ulp weld
//      miss all surface as an interior boundary edge;
//   3. the seam rule is factor-generic: k in {1,2,3}, mixed exponents, islands
//      and single quads all hold the same invariants (a bug that shows at one
//      factor only is a genericity bug by definition);
//   4. budgets hold: no leaf cluster exceeds the triangle budget.

namespace {

using badlands::TerrainDetailField;

// A compliant producer: the base bilinear surface minus a compact-support
// cosine dent. `map` must outlive the fixture (the sampler captures it).
struct DetailFixture {
  std::vector<uint8_t> exp;
  TerrainDetailField field;
};

DetailFixture MakeDetail(const MapData& map, int w, int h,
                         const std::function<int(int, int)>& exp_of,
                         float dent_cx, float dent_cz, float dent_r,
                         float dent_depth) {
  DetailFixture fx;
  fx.exp.resize(static_cast<size_t>(w) * h);
  for (int qz = 0; qz < h; ++qz)
    for (int qx = 0; qx < w; ++qx)
      fx.exp[static_cast<size_t>(qz) * w + qx] =
          static_cast<uint8_t>(exp_of(qx, qz));
  fx.field.level = fx.exp.data();
  fx.field.width = w;
  fx.field.height = h;
  fx.field.height_at = [&map, dent_cx, dent_cz, dent_r,
                        dent_depth](float wx, float wz) {
    const float base = map.HeightAt(wx, wz);
    const float dx = wx - dent_cx, dz = wz - dent_cz;
    const float d = std::sqrt(dx * dx + dz * dz);
    if (d >= dent_r) return base;
    return base -
           dent_depth * (0.5f + 0.5f * std::cos(3.14159265f * d / dent_r));
  };
  return fx;
}

std::vector<uint32_t> LeafClusters(const TerrainClusterDag& dag) {
  std::vector<uint32_t> out;
  for (uint32_t i = 0; i < dag.clusters.size(); ++i)
    if (dag.clusters[i].own_group == kNoGroup) out.push_back(i);
  return out;
}

// Edge-manifold check over a set of clusters: every triangle edge is shared by
// EXACTLY two triangles, or lies on the map perimeter. Stated geometrically
// (bit-exact endpoint positions), so it is depth- and level-agnostic by
// construction, and strictly stronger than comparing seam vertex sets.
void CheckEdgeManifold(const TerrainClusterDag& dag,
                       const std::vector<uint32_t>& clusters, float map_w,
                       float map_h) {
  using EdgeKey = std::array<uint32_t, 6>;  // two position bit-triples, sorted
  std::map<EdgeKey, int> count;
  for (uint32_t cidx : clusters) {
    const auto& c = dag.clusters[cidx];
    for (uint32_t t = 0; t + 2 < c.index_count; t += 3) {
      const uint32_t idx[3] = {dag.indices[c.first_index + t],
                               dag.indices[c.first_index + t + 1],
                               dag.indices[c.first_index + t + 2]};
      for (int e = 0; e < 3; ++e) {
        const auto pa = PosBits(Record(dag, idx[e]));
        const auto pb = PosBits(Record(dag, idx[(e + 1) % 3]));
        REQUIRE(pa != pb);  // no degenerate edges
        EdgeKey k;
        const bool swap = pb < pa;
        const auto& lo = swap ? pb : pa;
        const auto& hi = swap ? pa : pb;
        std::copy(lo.begin(), lo.end(), k.begin());
        std::copy(hi.begin(), hi.end(), k.begin() + 3);
        ++count[k];
      }
    }
  }
  for (const auto& [k, n] : count) {
    std::array<uint32_t, 8> ra{}, rb{};
    std::copy(k.begin(), k.begin() + 3, ra.begin());
    std::copy(k.begin() + 3, k.end(), rb.begin());
    const glm::vec3 a = PosOf(ra), b = PosOf(rb);
    CAPTURE(a.x, a.z, b.x, b.z, n);
    REQUIRE(n <= 2);
    if (n == 1) {
      // A boundary edge must lie ON one perimeter line of the map -- both
      // endpoints on the same border. Anything else is a crack.
      const bool on_border =
          (a.x == 0.0f && b.x == 0.0f) || (a.x == map_w && b.x == map_w) ||
          (a.z == 0.0f && b.z == 0.0f) || (a.z == map_h && b.z == map_h);
      REQUIRE(on_border);
    }
  }
}

// Every triangle faces up (+Y geometric normal), STRICTLY -- for LEAF
// geometry, where a zero-area triangle could only come from a broken ring
// walk. Simplified clusters are held to the weaker CheckNoInvertedFaces below.
void CheckFacesUp(const TerrainClusterDag& dag,
                  const std::vector<uint32_t>& clusters) {
  for (uint32_t cidx : clusters) {
    const auto& c = dag.clusters[cidx];
    for (uint32_t t = 0; t + 2 < c.index_count; t += 3) {
      const glm::vec3 p0 = PosOf(Record(dag, dag.indices[c.first_index + t]));
      const glm::vec3 p1 =
          PosOf(Record(dag, dag.indices[c.first_index + t + 1]));
      const glm::vec3 p2 =
          PosOf(Record(dag, dag.indices[c.first_index + t + 2]));
      CAPTURE(p0, p1, p2);
      REQUIRE(glm::cross(p1 - p0, p2 - p0).y > 0.0f);
    }
  }
}

// The SIMPLIFIED-geometry face check: no inversion anywhere (cross.y < 0 would
// be an overhang folded through the heightfield), and XZ-degenerate "fins"
// (cross.y == 0) rare.
//
// Fins are not garbage and cannot be forbidden outright: when a locked seam
// vertex's surface neighbours collapse onto the chord, meshopt keeps the seam
// SEALED by a vertical sliver between the chord and the neighbour's border
// polyline -- zero projected area, 3D area bounded by the very result_error
// the LOD cut already budgets, and the reason the edge-count check reads 2
// everywhere across LOD transitions. The uniform build has always contained a
// handful (found the moment cuts were checked at all). What fins must NOT do
// is plateau: a fin population explosion is the over-reduction wall pathology
// (locked verts kept alive ONLY by degenerates), which the locked-vertex
// simplify floor exists to prevent. The 1% ceiling is the tripwire for that.
void CheckNoInvertedFaces(const TerrainClusterDag& dag,
                          const std::vector<uint32_t>& clusters) {
  size_t tris = 0, fins = 0;
  for (uint32_t cidx : clusters) {
    const auto& c = dag.clusters[cidx];
    for (uint32_t t = 0; t + 2 < c.index_count; t += 3) {
      const glm::vec3 p0 = PosOf(Record(dag, dag.indices[c.first_index + t]));
      const glm::vec3 p1 =
          PosOf(Record(dag, dag.indices[c.first_index + t + 1]));
      const glm::vec3 p2 =
          PosOf(Record(dag, dag.indices[c.first_index + t + 2]));
      const float cy = glm::cross(p1 - p0, p2 - p0).y;
      CAPTURE(p0, p1, p2, cy);
      REQUIRE(cy >= 0.0f);
      ++tris;
      if (cy == 0.0f) ++fins;
    }
  }
  CAPTURE(fins, tris);
  // Measured: 2.8% at the coarsest cut of the dense diagonal-band fixture
  // (whose hot-tile fraction is ~5x the real river corridor's), every one a
  // verified seal. The ceiling is a REGIME tripwire, not a per-seal police --
  // the wall pathology it guards against floods the count, it does not creep.
  REQUIRE(fins * 100 <= 5 * tris);
}

// Watertightness of a SELECTED CUT, stated to tolerate exactly one artifact
// and nothing else. Counting is over REAL (cy > 0) triangles:
//   n >= 3           -> broken, full stop (overlapping surface);
//   n == 2           -> a proper interior edge;
//   n == 1           -> must lie on the map perimeter OR be covered by a FIN,
//                       the vertical sliver meshopt uses to seal a chord
//                       against a locked border polyline (see
//                       CheckNoInvertedFaces for why fins are legitimate);
//   n == 0, fin-only -> internal to a seal, fine (a double-fin seam leaves its
//                       short edges to the two fins alone).
// Counting fins as ordinary surface would let two seals on one chord read as a
// 4-triangle edge; ignoring them entirely would re-open every transition they
// seal. Both wrong answers were measured before this shape of the check.
void CheckCutWatertight(const TerrainClusterDag& dag,
                        const std::vector<uint32_t>& clusters, float map_w,
                        float map_h) {
  using EdgeKey = std::array<uint32_t, 6>;
  std::map<EdgeKey, int> real_count;
  std::set<EdgeKey> fin_edges;
  for (uint32_t cidx : clusters) {
    const auto& c = dag.clusters[cidx];
    for (uint32_t t = 0; t + 2 < c.index_count; t += 3) {
      const uint32_t idx[3] = {dag.indices[c.first_index + t],
                               dag.indices[c.first_index + t + 1],
                               dag.indices[c.first_index + t + 2]};
      const glm::vec3 p0 = PosOf(Record(dag, idx[0]));
      const glm::vec3 p1 = PosOf(Record(dag, idx[1]));
      const glm::vec3 p2 = PosOf(Record(dag, idx[2]));
      const bool fin = glm::cross(p1 - p0, p2 - p0).y == 0.0f;
      for (int e = 0; e < 3; ++e) {
        const auto pa = PosBits(Record(dag, idx[e]));
        const auto pb = PosBits(Record(dag, idx[(e + 1) % 3]));
        REQUIRE(pa != pb);
        EdgeKey k;
        const bool swap = pb < pa;
        const auto& lo = swap ? pb : pa;
        const auto& hi = swap ? pa : pb;
        std::copy(lo.begin(), lo.end(), k.begin());
        std::copy(hi.begin(), hi.end(), k.begin() + 3);
        if (fin)
          fin_edges.insert(k);
        else
          ++real_count[k];
      }
    }
  }
  for (const auto& [k, n] : real_count) {
    std::array<uint32_t, 8> ra{}, rb{};
    std::copy(k.begin(), k.begin() + 3, ra.begin());
    std::copy(k.begin() + 3, k.end(), rb.begin());
    const glm::vec3 a = PosOf(ra), b = PosOf(rb);
    CAPTURE(a.x, a.z, b.x, b.z, n);
    REQUIRE(n <= 2);
    if (n == 1) {
      const bool on_border =
          (a.x == 0.0f && b.x == 0.0f) || (a.x == map_w && b.x == map_w) ||
          (a.z == 0.0f && b.z == 0.0f) || (a.z == map_h && b.z == map_h);
      const bool sealed = fin_edges.count(k) > 0;
      REQUIRE((on_border || sealed));
    }
  }
}

// For every tile whose quads are all plain, the detailed build's single leaf
// cluster must match the null build's byte for byte (records AND order). This
// is "the refinement is provably local", stated as a test. Leaves are assigned
// to tiles by their AABB minimum, which always lies inside the owning tile.
void CheckColdTilesIdentical(const TerrainClusterDag& da,
                             const TerrainClusterDag& db,
                             const std::vector<uint8_t>& exp, int w, int h,
                             int tile_quads) {
  const int tiles_x = (w + tile_quads - 1) / tile_quads;
  const int tiles_z = (h + tile_quads - 1) / tile_quads;
  auto leaves_by_tile = [&](const TerrainClusterDag& dag) {
    std::map<std::pair<int, int>, std::vector<uint32_t>> out;
    for (uint32_t i : LeafClusters(dag)) {
      const auto& c = dag.clusters[i];
      const int tx = std::min(tiles_x - 1,
                              static_cast<int>(c.bounds.min.x) / tile_quads);
      const int tz = std::min(tiles_z - 1,
                              static_cast<int>(c.bounds.min.z) / tile_quads);
      out[{tx, tz}].push_back(i);
    }
    return out;
  };
  const auto la = leaves_by_tile(da);
  const auto lb = leaves_by_tile(db);
  for (int tz = 0; tz < tiles_z; ++tz) {
    for (int tx = 0; tx < tiles_x; ++tx) {
      bool hot = false;
      for (int qz = tz * tile_quads; qz < std::min((tz + 1) * tile_quads, h);
           ++qz)
        for (int qx = tx * tile_quads; qx < std::min((tx + 1) * tile_quads, w);
             ++qx)
          hot |= exp[static_cast<size_t>(qz) * w + qx] != 0;
      if (hot) continue;
      CAPTURE(tx, tz);
      const auto& ca_list = la.at({tx, tz});
      const auto& cb_list = lb.at({tx, tz});
      REQUIRE(ca_list.size() == 1);
      REQUIRE(cb_list.size() == 1);
      const auto& ca = da.clusters[ca_list[0]];
      const auto& cb = db.clusters[cb_list[0]];
      REQUIRE(ca.vertex_count == cb.vertex_count);
      REQUIRE(ca.index_count == cb.index_count);
      REQUIRE(std::memcmp(
                  da.vertices.data() + ca.first_vertex * 8,
                  db.vertices.data() + cb.first_vertex * 8,
                  static_cast<size_t>(ca.vertex_count) * 8 * sizeof(float)) ==
              0);
      for (uint32_t j = 0; j < ca.index_count; ++j)
        REQUIRE(da.indices[ca.first_index + j] - ca.first_vertex ==
                db.indices[cb.first_index + j] - cb.first_vertex);
    }
  }
}

}  // namespace

TEST_CASE("terrain detail: absent detail reproduces today's build bitwise",
          "[terrain_clusters]") {
  const int w = 32, h = 32;
  const MapData map = MakeMapData(w, h);
  const auto base = BuildTerrainClusterDag(map);
  SECTION("null field") {
    RequireDagsBitIdentical(base, BuildTerrainClusterDag(map, {}, nullptr));
  }
  SECTION("all-zero exponents") {
    const DetailFixture fx = MakeDetail(
        map, w, h, [](int, int) { return 0; }, 16.0f, 16.0f, 6.0f, 0.3f);
    RequireDagsBitIdentical(base, BuildTerrainClusterDag(map, {}, &fx.field));
  }
  SECTION("mismatched grid is ignored, not half-applied") {
    DetailFixture fx = MakeDetail(
        map, w, h, [](int, int) { return 3; }, 16.0f, 16.0f, 6.0f, 0.3f);
    fx.field.width = w - 1;  // wrong on purpose
    RequireDagsBitIdentical(base, BuildTerrainClusterDag(map, {}, &fx.field));
  }
}

TEST_CASE("terrain detail: refined leaves are watertight at every factor",
          "[terrain_clusters]") {
  const int w = 32, h = 32;
  const MapData map = MakeMapData(w, h);
  const auto base = BuildTerrainClusterDag(map);
  for (int k : {1, 2, 3}) {
    DYNAMIC_SECTION("k=" << k) {
      // A diagonal band of refined quads: crosses tile boundaries, touches
      // plain quads on both flanks, and is wholly interior to the map.
      const DetailFixture fx = MakeDetail(
          map, w, h,
          [&](int qx, int qz) {
            return (std::abs(qx - qz) <= 1 && qx > 2 && qx < 29) ? k : 0;
          },
          16.0f, 16.0f, 8.0f, 0.3f);
      const auto dag = BuildTerrainClusterDag(map, {}, &fx.field);

      const std::vector<uint32_t> leaves = LeafClusters(dag);
      CheckEdgeManifold(dag, leaves, static_cast<float>(w),
                        static_cast<float>(h));
      CheckFacesUp(dag, leaves);
      for (uint32_t i : leaves)
        REQUIRE(dag.clusters[i].index_count / 3 <=
                static_cast<uint32_t>(badlands::kClusterTriBudget));
      CheckLeafCoverage(dag, w, h);
      CheckColdTilesIdentical(dag, base, fx.exp, w, h,
                              badlands::kTileQuads);
      CheckAllInvariants(dag);
    }
  }
}

TEST_CASE("terrain detail: unequal exponents meet at the coarser resolution",
          "[terrain_clusters]") {
  const int w = 32, h = 32;
  const MapData map = MakeMapData(w, h);
  // k=3 against k=2 side by side within one tile: the shared edge must carry
  // exactly the coarser side's vertices (fe = 4 -> 3 interior positions), with
  // the finer side fanning down to them.
  const DetailFixture fx = MakeDetail(
      map, w, h,
      [](int qx, int qz) {
        if (qz != 10) return 0;
        if (qx == 10) return 3;
        if (qx == 11) return 2;
        return 0;
      },
      10.5f, 10.5f, 2.0f, 0.2f);
  const auto dag = BuildTerrainClusterDag(map, {}, &fx.field);
  const std::vector<uint32_t> leaves = LeafClusters(dag);
  CheckEdgeManifold(dag, leaves, static_cast<float>(w), static_cast<float>(h));
  CheckFacesUp(dag, leaves);

  // Count distinct vertex positions on the open shared edge x == 11, z in
  // (10, 11): exactly the coarser factor's 3 interior vertices.
  std::set<uint32_t> z_bits;
  for (uint32_t cidx : leaves) {
    const auto& c = dag.clusters[cidx];
    for (uint32_t v = 0; v < c.vertex_count; ++v) {
      const glm::vec3 p = PosOf(Record(dag, c.first_vertex + v));
      if (p.x == 11.0f && p.z > 10.0f && p.z < 11.0f) {
        uint32_t zb;
        std::memcpy(&zb, &p.z, 4);
        z_bits.insert(zb);
      }
    }
  }
  REQUIRE(z_bits.size() == 3);
}

TEST_CASE("terrain detail: islands and single quads hold the invariants",
          "[terrain_clusters]") {
  const int w = 32, h = 32;
  const MapData map = MakeMapData(w, h);
  const auto base = BuildTerrainClusterDag(map);
  const DetailFixture fx = MakeDetail(
      map, w, h,
      [](int qx, int qz) {
        if (qx >= 3 && qx <= 4 && qz >= 3 && qz <= 4) return 2;    // island A
        if (qx >= 20 && qx <= 21 && qz >= 20 && qz <= 21) return 3;  // island B
        if (qx == 28 && qz == 5) return 1;  // a single isolated quad
        return 0;
      },
      3.5f, 3.5f, 1.5f, 0.2f);
  const auto dag = BuildTerrainClusterDag(map, {}, &fx.field);
  const std::vector<uint32_t> leaves = LeafClusters(dag);
  CheckEdgeManifold(dag, leaves, static_cast<float>(w), static_cast<float>(h));
  CheckFacesUp(dag, leaves);
  CheckLeafCoverage(dag, w, h);
  CheckColdTilesIdentical(dag, base, fx.exp, w, h, badlands::kTileQuads);
  CheckAllInvariants(dag);
}

TEST_CASE("terrain detail: selected cuts stay exact watertight covers",
          "[terrain_clusters]") {
  const int w = 32, h = 32;
  const MapData map = MakeMapData(w, h);
  const DetailFixture fx = MakeDetail(
      map, w, h,
      [](int qx, int qz) { return (std::abs(qx - qz) <= 1) ? 3 : 0; }, 16.0f,
      16.0f, 8.0f, 0.3f);
  const glm::vec3 cams[] = {{16.0f, 40.0f, 16.0f}, {2.0f, 6.0f, 2.0f}};
  const float fov = 45.0f, screen_h = 900.0f;
  for (const bool with_detail : {false, true}) {
    DYNAMIC_SECTION("detail=" << with_detail) {
      const auto dag = BuildTerrainClusterDag(
          map, {}, with_detail ? &fx.field : nullptr);
      for (const glm::vec3& cam : cams) {
        for (float tau : {0.25f, 1.5f, 16.0f}) {
          CAPTURE(cam, tau);
          CheckCutValidity(dag, cam, fov, screen_h, tau);
          std::vector<uint32_t> sel;
          SelectClusters(dag, cam, fov, screen_h, tau, sel);
          // Watertight up to fin seals, plus no inversions and a bounded fin
          // population. The pair caught a live finding: before the simplify
          // target was floored by the locked-vertex count, tiny pre-pass
          // groups kept locked verts alive ONLY through fins.
          CheckCutWatertight(dag, sel, static_cast<float>(w),
                             static_cast<float>(h));
          CheckNoInvertedFaces(dag, sel);
        }
      }
    }
  }
}

TEST_CASE("terrain detail: the pre-pass leaves a sound mixed-depth tree",
          "[terrain_clusters]") {
  const int w = 32, h = 32;
  const MapData map = MakeMapData(w, h);
  const DetailFixture fx = MakeDetail(
      map, w, h,
      [](int qx, int qz) { return (std::abs(qx - qz) <= 1) ? 3 : 0; }, 16.0f,
      16.0f, 8.0f, 0.3f);
  const auto dag = BuildTerrainClusterDag(map, {}, &fx.field);

  // The tree is genuinely mixed-depth (this fixture is not vacuous): leaves
  // coexist with clusters many levels up.
  int max_level = 0;
  for (const auto& c : dag.clusters) max_level = std::max(max_level, c.level);
  REQUIRE(max_level > 3);
  REQUIRE(dag.level_count == max_level + 1);

  // No cluster is empty. An empty cluster is the signature of scratch geometry
  // freed while a region still held it (the use-after-clear the liveness-based
  // free exists to prevent) -- it would render as a hole, not a crash.
  for (const auto& c : dag.clusters) {
    REQUIRE(c.vertex_count > 0);
    REQUIRE(c.index_count >= 3);
  }

  // Locked fine seams must DRAIN as the hierarchy coarsens, not persist: the
  // root is where every seam has long since fallen inside some group, so its
  // size is the plateau detector. (Mid levels legitimately exceed the budget
  // while a seam is still locked -- that is the documented one-extra-level
  // persistence -- so the assertion is at the root, not per level.)
  for (const auto& c : dag.clusters) {
    if (c.parent_group != kNoGroup) continue;
    REQUIRE(c.index_count / 3 <= 2u * badlands::kClusterTriBudget);
  }
}

TEST_CASE("terrain detail: parallel == serial, bitwise, on a mixed-depth build",
          "[terrain_clusters]") {
  const int w = 32, h = 32;
  const MapData map = MakeMapData(w, h);
  const DetailFixture fx = MakeDetail(
      map, w, h,
      [](int qx, int qz) { return (std::abs(qx - qz) <= 1) ? 3 : 0; }, 16.0f,
      16.0f, 8.0f, 0.3f);
  auto build = [&](bool parallel) {
    TerrainClusterParams p;
    p.parallel_build = parallel;
    return BuildTerrainClusterDag(map, p, &fx.field);
  };
  RequireDagsBitIdentical(build(false), build(true));
  RequireDagsBitIdentical(build(true), build(true));
}
