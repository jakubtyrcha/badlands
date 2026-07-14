// Ported from sampo's src/rendering/components/mesh_components.cpp,
// namespace sampo -> badlands (wrapped in namespace, see mesh_components.hpp
// for why), verbatim otherwise.
#include "engine/rendering/components/mesh_components.hpp"

namespace badlands {

std::vector<glm::vec3> ExtractVertexPositions(const StaticMeshComponent& mesh) {
  std::vector<glm::vec3> positions;
  constexpr size_t kVertexStride =
      10;  // pos(3) + color(3) + normal(3) + cell_index(1)
  size_t vertex_count = mesh.vertices.size() / kVertexStride;
  positions.reserve(vertex_count);
  for (size_t v = 0; v < vertex_count; ++v) {
    size_t base = v * kVertexStride;
    positions.emplace_back(mesh.vertices[base], mesh.vertices[base + 1],
                           mesh.vertices[base + 2]);
  }
  return positions;
}

}  // namespace badlands
