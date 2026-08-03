#pragma once

// CPU-side scene for the rhi_lab MVP: a real terrain cluster DAG plus a field
// of tree instances. No GPU types here -- this is the data the renderer
// uploads, kept separate so it can be built and inspected without a device.

#include <cstdint>
#include <vector>

#include <glm/glm.hpp>

#include "game/geometry/terrain_clusters.hpp"

namespace badlands::lab {

// Mirrors ClusterGpu in shaders/slang/rhi_lab/lab_common.slang.
struct ClusterGpu {
  glm::vec4 sphere{0.0f};  // xyz centre, w radius
  uint32_t first_index = 0;
  uint32_t index_count = 0;
  float own_error = 0.0f;
  float parent_error = 0.0f;
};
static_assert(sizeof(ClusterGpu) == 32, "must match the shader struct");

// Mirrors TreeInstance.
struct TreeInstance {
  glm::vec4 position_scale{0.0f};
  glm::vec4 tint{1.0f};
};
static_assert(sizeof(TreeInstance) == 32, "must match the shader struct");

// One vertex, matching kFloatsPerClusterVertex so the DAG's array uploads
// verbatim.
struct LabVertex {
  float px = 0, py = 0, pz = 0;
  float nx = 0, ny = 1, nz = 0;
  float color_packed = 0;
  float meta_packed = 0;
};
static_assert(sizeof(LabVertex) == 32, "must match kFloatsPerClusterVertex");

struct Scene {
  // Terrain, straight from the cluster DAG.
  TerrainClusterDag dag;
  std::vector<ClusterGpu> clusters;

  // A low-poly tree proxy, shared by every instance.
  std::vector<LabVertex> tree_vertices;
  std::vector<uint32_t> tree_indices;
  std::vector<TreeInstance> trees;

  // World extent, for framing the camera and building the splat transform.
  float size_x_m = 0.0f;
  float size_z_m = 0.0f;
  float max_height_m = 0.0f;
};

// Builds a synthetic heightmap of `nodes` x `nodes` at `spacing_m`, runs the
// real cluster DAG over it, and scatters `tree_count` trees across it.
// Deterministic for a given seed, so a screenshot is reproducible.
Scene BuildScene(int nodes, float spacing_m, int tree_count, uint32_t seed);

}  // namespace badlands::lab
