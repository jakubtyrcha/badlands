#pragma once

// Build-side of the Nanite-style terrain cluster-LOD DAG (see
// docs/superpowers/specs/2026-07-19-terrain-cluster-lod-design.md). Pure CPU,
// game-layer: it tessellates a mapgen heightmap into grid-tile leaf clusters and
// repeatedly groups + boundary-locked-simplifies (meshoptimizer) + splits them
// into a level hierarchy, recording per-group LOD error + bounding sphere. The
// output DAG feeds runtime screen-space-error cluster selection (SelectClusters,
// declared here but implemented in a later milestone) and per-cluster indexed
// draws. No engine/GPU dependency beyond the header-only Aabb.
//
// Seamlessness is a build invariant, not a runtime rule: a vertex on the
// boundary between two groups of a level is LOCKED in both groups' simplify
// calls, so both sides keep it bitwise-identically -> adjacent clusters (even at
// different LODs) share crack-free boundaries with no runtime coordination. The
// Catch2 suite (terrain_clusters_tests.cpp) pins the load-bearing invariants:
// error monotonicity, sphere nesting/sharing, and bitwise seam equality.

#include <cstdint>
#include <vector>

#include <glm/glm.hpp>

#include "engine/rendering/geometry/aabb.hpp"
#include "mapgen/field2d.hpp"

namespace badlands {

// Compile-time partition/budget constants — no bare partition literals live in
// the build code (it reads TerrainClusterParams, which defaults to these). Same
// pattern as mapgen_constants.hpp. Changing kTileQuads / kGroupDim reshapes the
// whole build consistently because every derived value is computed from them.
inline constexpr int kTileQuads = 8;      // leaf tile edge, in 1 m quads
inline constexpr int kGroupDim = 2;       // group = kGroupDim x kGroupDim children
inline constexpr int kClusterTriBudget =  // 2 tris/quad over a kTileQuads^2 tile
    2 * kTileQuads * kTileQuads;          // = 128
inline constexpr int kGroupSplitCount = 2;  // clusters emitted per simplified group
inline constexpr float kSimplifyTargetRatio =
    float(kGroupSplitCount) / (kGroupDim * kGroupDim);  // = 0.5

static_assert(kGroupDim * kGroupDim * kClusterTriBudget % kGroupSplitCount == 0);

// Interleaved vertex: 8 floats (32 B), matching the kTerrainCluster layout added
// in a later milestone: pos 3f, normal 3f, color Unorm8x4 packed as 1 float,
// meta Uint8x4 packed as 1 float {biome_id, cluster_hash_byte, lod_level, 0}.
inline constexpr int kFloatsPerClusterVertex = 8;

// Sentinel for "no group": a leaf's own_group (its own LOD error is 0) and a
// root's parent_group (treated as +inf error by selection -> never coarsened).
inline constexpr int kNoGroup = -1;

// Build knobs. Every field defaults to the constexpr above (or a tuned weight);
// the build reads these everywhere so a test can rebuild with, e.g., tile_quads
// = 4 and assert the invariants still hold (spec Verification test 6).
struct TerrainClusterParams {
  int tile_quads = kTileQuads;
  int group_dim = kGroupDim;
  int group_split_count = kGroupSplitCount;
  float simplify_target_ratio = kSimplifyTargetRatio;
  // meshopt attribute-error weights (relative to position, in world meters);
  // normal is unit-length, color is unpacked to [0,1] before weighting.
  float attr_weight_normal = 0.5f;
  float attr_weight_color = 0.5f;
};

// One LOD cluster: a range into the DAG's shared vertex/index buffers plus its
// place in the hierarchy. Vertices are never shared between clusters (boundary
// verts are duplicated bitwise-identically), so per-vertex cluster/level tint is
// legal and each cluster is one DrawIndexed.
struct TerrainCluster {
  uint32_t first_index = 0;
  uint32_t index_count = 0;
  uint32_t first_vertex = 0;   // debug/tests
  uint32_t vertex_count = 0;   // debug/tests
  Aabb bounds;                 // world-space
  int own_group = kNoGroup;    // group that produced this cluster (leaf: none)
  int parent_group = kNoGroup; // group that consumed this cluster (root: none)
  int level = 0;               // 0 = full-res leaf
};

// One group: consumes kGroupDim^2 (fewer at the map edge) same-level clusters,
// simplifies them under a locked shared boundary, and emits kGroupSplitCount
// (fewer for a small group) clusters one level up. error/sphere are monotone by
// construction so projected error is monotone along any leaf->root chain.
struct TerrainClusterGroup {
  float error_m = 0.0f;        // world meters; max(simplify error, children's)
  glm::vec4 sphere{0.0f};      // xyz center, w radius; encloses children + AABB
  glm::vec4 footprint{0.0f};   // world XZ rect {x0, z0, x1, z1} (locking/tests)
  int level = 0;               // level of the clusters this group PRODUCED
  uint32_t first_child = 0;    // span into group_children (consumed clusters)
  uint32_t child_count = 0;
};

// The whole DAG for one map. Flat parallel arrays, indices not pointers.
struct TerrainClusterDag {
  std::vector<float> vertices;          // kFloatsPerClusterVertex each
  std::vector<uint32_t> indices;        // into `vertices` (global)
  std::vector<TerrainCluster> clusters;
  std::vector<TerrainClusterGroup> groups;
  std::vector<uint32_t> group_children; // consumed-cluster indices, per group

  int map_quads_x = 0;  // heightmap.width  (leaf vertex grid is (w+1) wide)
  int map_quads_z = 0;  // heightmap.height
  int level_count = 0;  // number of LOD levels (leaves = level 0)

  // A cluster's own LOD error (leaves: 0) and the error above which it must be
  // refined (roots: +inf). Small helpers so selection/tests never special-case
  // the kNoGroup sentinel by hand.
  float ClusterOwnError(const TerrainCluster& c) const {
    return c.own_group == kNoGroup ? 0.0f : groups[c.own_group].error_m;
  }
  glm::vec4 ClusterOwnSphere(const TerrainCluster& c) const;
};

// Build the DAG from a mapgen heightmap (world meters) + per-sample biome map.
// Both fields are WxH samples; the leaf vertex grid is (W+1)x(H+1) at 1 m
// spacing (edge verts clamp-sample), so W and H are also the quad counts and
// W/tile_quads tiles span each axis. Logs per-level stats via spdlog.
TerrainClusterDag BuildTerrainClusterDag(const mapgen::Field2D<float>& heightmap,
                                         const mapgen::Field2D<uint8_t>& biomes,
                                         const TerrainClusterParams& params = {});

// Runtime cluster selection (implemented in a later milestone — declared here so
// the DAG's consumers can link against the final interface). Selects the cut
// where projected own-error <= tau and projected parent error > tau.
void SelectClusters(const TerrainClusterDag& dag, glm::vec3 cam_pos,
                    float fov_deg, float screen_h_px, float tau_px,
                    std::vector<uint32_t>& out);

}  // namespace badlands
