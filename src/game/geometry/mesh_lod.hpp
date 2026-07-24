#pragma once

// General-purpose mesh-simplification helper built on meshoptimizer. Pure
// geometry math -- no engine/tree/game vocabulary here; callers own the
// vertex layout and pass floats_per_vertex explicitly. See mesh_lod.cpp for
// the meshopt call sequence (weld via the canonical indexer, then
// meshopt_simplify on positions only).

#include <cstddef>
#include <cstdint>
#include <vector>

namespace badlands {

struct SimplifiedMesh {
  std::vector<float> vertices;    // same layout as input (floats_per_vertex each)
  std::vector<uint32_t> indices;  // reduced index buffer
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

}  // namespace badlands
