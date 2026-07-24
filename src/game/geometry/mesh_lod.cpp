#include "game/geometry/mesh_lod.hpp"

#include <algorithm>

#include <meshoptimizer.h>

namespace badlands {

SimplifiedMesh SimplifyMesh(const std::vector<float>& vertices,
                             size_t floats_per_vertex,
                             const std::vector<uint32_t>& indices,
                             float target_ratio) {
  const size_t stride = floats_per_vertex * sizeof(float);
  const size_t vertex_count = vertices.size() / floats_per_vertex;

  if (target_ratio >= 1.0f || indices.size() < 3) {
    return SimplifiedMesh{vertices, indices,
                           static_cast<uint32_t>(vertex_count)};
  }

  // Weld coincident vertices via the canonical meshopt indexer so decimation
  // isn't blocked by duplicated positions/attributes at shared edges.
  std::vector<uint32_t> remap(vertex_count);
  const size_t welded_vertex_count = meshopt_generateVertexRemap(
      remap.data(), indices.data(), indices.size(), vertices.data(),
      vertex_count, stride);

  std::vector<float> welded_vertices(welded_vertex_count * floats_per_vertex);
  meshopt_remapVertexBuffer(welded_vertices.data(), vertices.data(),
                             vertex_count, stride, remap.data());

  std::vector<uint32_t> welded_indices(indices.size());
  meshopt_remapIndexBuffer(welded_indices.data(), indices.data(),
                            indices.size(), remap.data());

  const size_t welded_index_count = welded_indices.size();
  size_t target = static_cast<size_t>(welded_index_count * target_ratio);
  target -= target % 3;
  if (target < 3) target = std::min<size_t>(welded_index_count, 3);

  std::vector<uint32_t> simplified(welded_index_count);
  float result_error = 0.0f;
  const size_t got = meshopt_simplify(
      simplified.data(), welded_indices.data(), welded_index_count,
      welded_vertices.data(), welded_vertex_count, stride, target,
      /*target_error=*/1.0f, /*options=*/0, &result_error);
  simplified.resize(got);

  return SimplifiedMesh{std::move(welded_vertices), std::move(simplified),
                         static_cast<uint32_t>(welded_vertex_count)};
}

}  // namespace badlands
