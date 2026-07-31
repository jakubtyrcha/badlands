#pragma once

// General-purpose mesh-simplification helper built on meshoptimizer. Pure
// geometry math -- no engine/tree/game vocabulary here; callers own the
// vertex layout and pass floats_per_vertex explicitly. See mesh_lod.cpp for
// the meshopt call sequence (weld via the canonical indexer, then
// meshopt_simplify on positions only).

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace badlands {

// Default per-level simplification ratios, passed to SimplifyMesh as
// target_ratio; level 0 = identity (no simplification -- SimplifyMesh's
// ratio >= 1.0 short-circuit below). Shared by every caller that wants a
// plain 3-level LOD chain rather than a bespoke ratio schedule.
inline constexpr std::array<float, 3> kDefaultLodRatios{1.0f, 0.5f, 0.2f};

// Leaves lose visual mass quadratically when whole cards are deleted; decimate
// them more gently than bark. (screenshot-tuned)
inline constexpr std::array<float, 3> kLeafLodRatios = {1.0f, 0.65f, 0.35f};

struct SimplifiedMesh {
  // Same layout as the input (floats_per_vertex each), but compacted: only the
  // vertices `indices` still references survive, so a heavily decimated result
  // carries a proportionally small vertex buffer rather than the full welded
  // set. (The identity path -- target_ratio >= 1.0 -- returns the input as-is.)
  std::vector<float> vertices;
  std::vector<uint32_t> indices;  // reduced index buffer, into `vertices`
  uint32_t vertex_count = 0;
};

// Simplify an indexed triangle mesh to ~target_ratio of its triangles using
// meshoptimizer. Position is the first 3 floats of each vertex. target_ratio
// in (0,1]; >=1.0 returns the mesh unchanged (identity). Welds by the
// meshopt indexer first so decimation isn't blocked by duplicated
// coincident vertices, then meshopt_simplify (positions only -- surviving
// vertices keep their exact attributes, so no UV/normal distortion).
// Deterministic.
SimplifiedMesh SimplifyMesh(const std::vector<float>& vertices,
                             size_t floats_per_vertex,
                             const std::vector<uint32_t>& indices,
                             float target_ratio);

// Same contract as SimplifyMesh, but decimates by vertex clustering
// (meshopt_simplifySloppy) instead of error-bounded edge collapse. Use it
// only where the target ratio is far below what SimplifyMesh can reach:
// edge collapse cannot merge disconnected components, so a mesh of many
// separate shells (a tree's per-branch bark tubes -- ~200 of them on the
// larger presets) bottoms out at a few triangles per shell no matter how low
// the ratio goes (measured: Oak (large) floors at ~774 tris, Bush 3 at
// ~4280, both unchanged from ratio 0.05 down to 0.005). Clustering ignores
// topology, so it merges those shells and does hit the target -- at the cost
// of a blobby silhouette and no attribute fidelity, which is the right
// trade only at the extreme end of an LOD chain.
SimplifiedMesh SimplifyMeshSloppy(const std::vector<float>& vertices,
                                   size_t floats_per_vertex,
                                   const std::vector<uint32_t>& indices,
                                   float target_ratio);

}  // namespace badlands
