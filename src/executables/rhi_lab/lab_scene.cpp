#include "src/executables/rhi_lab/lab_scene.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <random>

#include <spdlog/spdlog.h>

#include "game/map/map_data.hpp"

namespace badlands::lab {
namespace {

// Cheap layered value noise. The lab needs terrain with real relief so LOD
// selection has something to choose between; it does not need the mapgen
// pipeline, and depending on it would drag the whole game layer in.
float Hash(int x, int z, uint32_t seed) {
  uint32_t h = uint32_t(x) * 374761393u + uint32_t(z) * 668265263u + seed;
  h = (h ^ (h >> 13)) * 1274126177u;
  return float(h ^ (h >> 16)) / float(0xFFFFFFFFu);
}

float ValueNoise(float x, float z, uint32_t seed) {
  const int xi = int(std::floor(x)), zi = int(std::floor(z));
  const float fx = x - float(xi), fz = z - float(zi);
  const float ux = fx * fx * (3.0f - 2.0f * fx);
  const float uz = fz * fz * (3.0f - 2.0f * fz);
  const float a = Hash(xi, zi, seed), b = Hash(xi + 1, zi, seed);
  const float c = Hash(xi, zi + 1, seed), d = Hash(xi + 1, zi + 1, seed);
  return (a + (b - a) * ux) + ((c + (d - c) * ux) - (a + (b - a) * ux)) * uz;
}

float Terrain(float x, float z, uint32_t seed) {
  float h = 0.0f, amp = 1.0f, freq = 1.0f / 64.0f;
  for (int o = 0; o < 5; ++o) {
    h += amp * ValueNoise(x * freq, z * freq, seed + uint32_t(o) * 7919u);
    amp *= 0.5f;
    freq *= 2.0f;
  }
  return h * 18.0f;
}

// A four-sided pyramid on a trunk-less base -- enough silhouette to read as a
// tree in a visibility buffer without pulling in the foliage generator.
void BuildTreeProxy(std::vector<LabVertex>& verts, std::vector<uint32_t>& idx) {
  auto push = [&verts](glm::vec3 p, glm::vec3 n) {
    verts.push_back({p.x, p.y, p.z, n.x, n.y, n.z, 0.0f, 0.0f});
  };
  const float r = 1.2f, h = 4.5f;
  const glm::vec3 apex{0.0f, h, 0.0f};
  const glm::vec3 base[4] = {{-r, 0, -r}, {r, 0, -r}, {r, 0, r}, {-r, 0, r}};
  for (int i = 0; i < 4; ++i) {
    const glm::vec3 a = base[i], b = base[(i + 1) % 4];
    const glm::vec3 n = glm::normalize(glm::cross(b - a, apex - a));
    const uint32_t first = uint32_t(verts.size());
    push(a, n);
    push(b, n);
    push(apex, n);
    idx.push_back(first);
    idx.push_back(first + 1);
    idx.push_back(first + 2);
  }
}

}  // namespace

Scene BuildScene(int nodes, float spacing_m, int tree_count, uint32_t seed) {
  Scene s;

  MapData map(nodes, nodes, spacing_m);
  for (int j = 0; j < nodes; ++j) {
    for (int i = 0; i < nodes; ++i) {
      const float h = Terrain(float(i) * spacing_m, float(j) * spacing_m, seed);
      map.mutable_height(i, j) = h;
      s.max_height_m = std::max(s.max_height_m, h);
    }
  }
  s.size_x_m = map.size_x_m();
  s.size_z_m = map.size_z_m();

  s.dag = BuildTerrainClusterDag(map);
  uint32_t max_indices = 0;
  size_t over_budget = 0;
  for (const auto& c : s.dag.clusters) {
    max_indices = std::max(max_indices, c.index_count);
    if (c.index_count > 384) ++over_budget;
  }
  spdlog::info("rhi_lab: cluster DAG — {} clusters, {} levels, {} tris, "
               "max index_count {} ({} clusters over the 384 budget)",
               s.dag.clusters.size(), s.dag.level_count,
               s.dag.indices.size() / 3, max_indices, over_budget);

  // Flatten the DAG into what the GPU selector reads. own/parent error and the
  // bounding sphere are exactly the inputs SelectClusters uses on the CPU --
  // the rule is unchanged, only where it runs.
  s.clusters.reserve(s.dag.clusters.size());
  for (const auto& c : s.dag.clusters) {
    ClusterGpu g;
    g.sphere = s.dag.ClusterOwnSphere(c);
    g.first_index = c.first_index;
    g.index_count = c.index_count;
    g.own_error = s.dag.ClusterOwnError(c);
    // A root is never coarsened, which selection expresses as infinite parent
    // error. Using a large finite value keeps the shader free of inf handling.
    if (c.parent_group == kNoGroup) {
      g.parent_error = 1e30f;  // a root is never coarsened
      g.parent_sphere = g.sphere;
    } else {
      // The parent's error is measured against the PARENT's sphere, not this
      // cluster's -- see ClusterGpu in lab_common.slang.
      const auto& pg = s.dag.groups[c.parent_group];
      g.parent_error = pg.error_m;
      g.parent_sphere = pg.sphere;
    }
    s.clusters.push_back(g);
  }

  BuildTreeProxy(s.tree_vertices, s.tree_indices);

  std::mt19937 rng(seed ^ 0x5eed1234u);
  std::uniform_real_distribution<float> ux(0.0f, s.size_x_m);
  std::uniform_real_distribution<float> uz(0.0f, s.size_z_m);
  std::uniform_real_distribution<float> uscale(0.7f, 1.6f);
  s.trees.reserve(size_t(tree_count));
  for (int i = 0; i < tree_count; ++i) {
    const float x = ux(rng), z = uz(rng);
    const float y = Terrain(x, z, seed);
    s.trees.push_back({{x, y, z, uscale(rng)}, {1.0f, 1.0f, 1.0f, 1.0f}});
  }
  spdlog::info("rhi_lab: {} trees, terrain {:.0f}x{:.0f} m", s.trees.size(),
               s.size_x_m, s.size_z_m);
  return s;
}

}  // namespace badlands::lab
