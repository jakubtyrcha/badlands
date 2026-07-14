#include "engine/rendering/geometry/mesh_builder_utils.hpp"

#include <glm/gtc/matrix_inverse.hpp>

#include "engine/rendering/geometry/textured_mesh_builders.hpp"

namespace badlands {

void PushVertex(std::vector<float>& out, const glm::vec3& pos, const glm::vec2& uv,
                const glm::vec3& normal, const glm::vec3& tangent) {
  out.push_back(pos.x);
  out.push_back(pos.y);
  out.push_back(pos.z);
  out.push_back(uv.x);
  out.push_back(uv.y);
  out.push_back(normal.x);
  out.push_back(normal.y);
  out.push_back(normal.z);
  out.push_back(tangent.x);
  out.push_back(tangent.y);
  out.push_back(tangent.z);
}

void AppendTransformedMesh(StaticTexturedMeshComponent& dst,
                           const StaticTexturedMeshComponent& src,
                           const glm::mat4& transform) {
  const glm::mat3 linear(transform);
  const glm::mat3 normal_matrix = glm::inverseTranspose(linear);

  dst.vertices.reserve(dst.vertices.size() + src.vertices.size());
  for (size_t i = 0; i + kTexturedMeshFloatsPerVertex <= src.vertices.size();
       i += kTexturedMeshFloatsPerVertex) {
    glm::vec4 pos(src.vertices[i], src.vertices[i + 1], src.vertices[i + 2], 1.0f);
    glm::vec2 uv(src.vertices[i + 3], src.vertices[i + 4]);
    glm::vec3 normal(src.vertices[i + 5], src.vertices[i + 6], src.vertices[i + 7]);
    glm::vec3 tangent(src.vertices[i + 8], src.vertices[i + 9], src.vertices[i + 10]);

    glm::vec3 world_pos = glm::vec3(transform * pos);
    glm::vec3 world_normal = glm::normalize(normal_matrix * normal);
    glm::vec3 world_tangent = glm::normalize(linear * tangent);

    PushVertex(dst.vertices, world_pos, uv, world_normal, world_tangent);
    ++dst.vertex_count;
  }
}

}  // namespace badlands
