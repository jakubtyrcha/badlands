// Ported from sampo's src/rendering/geometry/textured_mesh_builders.cpp,
// namespace sampo -> badlands, verbatim otherwise (includes adapted to
// badlands paths; `core/math/cube_sphere.hpp` -> the relocated
// `engine/rendering/geometry/cube_sphere.hpp`, see the deviation note in
// aabb.hpp).
#include "engine/rendering/geometry/textured_mesh_builders.hpp"

#include "engine/rendering/geometry/cube_sphere.hpp"

namespace badlands {

Aabb ComputeLocalAabbFromVertices(const std::vector<float>& vertices,
                                  size_t floats_per_vertex) {
  auto result = Aabb::Empty();
  for (size_t i = 0; i < vertices.size(); i += floats_per_vertex) {
    glm::vec3 pos(vertices[i], vertices[i + 1], vertices[i + 2]);
    result.min = glm::min(result.min, pos);
    result.max = glm::max(result.max, pos);
  }
  return result;
}

Aabb ComputeLocalAabb(const StaticTexturedMeshComponent& mesh) {
  return ComputeLocalAabbFromVertices(mesh.vertices,
                                      kTexturedMeshFloatsPerVertex);
}

TexturedMeshResult GenerateSphereTexturedMesh(float radius, int resolution) {
  auto sphere = GenerateCubeSphereMesh(resolution);

  StaticTexturedMeshComponent result;

  result.vertices.reserve(sphere.indices.size() * kTexturedMeshFloatsPerVertex);

  for (uint32_t idx : sphere.indices) {
    glm::vec3 pos = sphere.positions[idx] * radius;
    glm::vec2 uv = sphere.uvs[idx];
    glm::vec3 normal = sphere.positions[idx];  // Radial normal
    glm::vec3 tangent = sphere.tangents[idx];

    result.vertices.push_back(pos.x);
    result.vertices.push_back(pos.y);
    result.vertices.push_back(pos.z);
    result.vertices.push_back(uv.x);
    result.vertices.push_back(uv.y);
    result.vertices.push_back(normal.x);
    result.vertices.push_back(normal.y);
    result.vertices.push_back(normal.z);
    result.vertices.push_back(tangent.x);
    result.vertices.push_back(tangent.y);
    result.vertices.push_back(tangent.z);
  }

  result.vertex_count = static_cast<uint32_t>(sphere.indices.size());
  result.dirty = true;

  // Analytical AABB for sphere
  auto bounds = Aabb::FromMinMax(glm::vec3(-radius), glm::vec3(radius));

  return {.mesh = std::move(result), .local_bounds = bounds};
}

TexturedMeshResult GenerateQuadTexturedMesh(float size, int resolution) {
  StaticTexturedMeshComponent result;

  if (resolution <= 1) {
    // Simple quad (2 triangles, 6 vertices)
    result.vertices.reserve(6 * kTexturedMeshFloatsPerVertex);

    float half = size * 0.5f;
    glm::vec3 normal(0.0f, 0.0f, 1.0f);
    glm::vec3 tangent(1.0f, 0.0f, 0.0f);

    auto emit_vertex = [&](float x, float y, float u, float v) {
      result.vertices.push_back(x);
      result.vertices.push_back(y);
      result.vertices.push_back(0.0f);
      result.vertices.push_back(u);
      result.vertices.push_back(v);
      result.vertices.push_back(normal.x);
      result.vertices.push_back(normal.y);
      result.vertices.push_back(normal.z);
      result.vertices.push_back(tangent.x);
      result.vertices.push_back(tangent.y);
      result.vertices.push_back(tangent.z);
    };

    // Triangle 1
    emit_vertex(-half, -half, 0.0f, 0.0f);  // bottom-left
    emit_vertex(half, half, 1.0f, 1.0f);    // top-right
    emit_vertex(-half, half, 0.0f, 1.0f);   // top-left

    // Triangle 2
    emit_vertex(-half, -half, 0.0f, 0.0f);  // bottom-left
    emit_vertex(half, -half, 1.0f, 0.0f);   // bottom-right
    emit_vertex(half, half, 1.0f, 1.0f);    // top-right

    result.vertex_count = 6;
  } else {
    // Tessellated quad
    auto quad = GenerateTessellatedQuad(resolution);

    result.vertices.reserve(quad.indices.size() * kTexturedMeshFloatsPerVertex);

    for (uint32_t idx : quad.indices) {
      glm::vec3 pos = quad.positions[idx] * size;
      glm::vec2 uv = quad.uvs[idx];
      glm::vec3 normal(0.0f, 0.0f, 1.0f);
      glm::vec3 tangent = quad.tangents[idx];

      result.vertices.push_back(pos.x);
      result.vertices.push_back(pos.y);
      result.vertices.push_back(pos.z);
      result.vertices.push_back(uv.x);
      result.vertices.push_back(uv.y);
      result.vertices.push_back(normal.x);
      result.vertices.push_back(normal.y);
      result.vertices.push_back(normal.z);
      result.vertices.push_back(tangent.x);
      result.vertices.push_back(tangent.y);
      result.vertices.push_back(tangent.z);
    }

    result.vertex_count = static_cast<uint32_t>(quad.indices.size());
  }

  result.dirty = true;

  // Analytical AABB for quad in XY plane
  float half = size * 0.5f;
  auto bounds =
      Aabb::FromMinMax(glm::vec3(-half, -half, 0.0f), glm::vec3(half, half, 0.0f));

  return {.mesh = std::move(result), .local_bounds = bounds};
}

StaticCubeMapTexturedMesh GenerateSphereCubeMapTexturedMesh(float radius,
                                                            int resolution) {
  auto sphere = GenerateCubeSphereMesh(resolution);

  StaticCubeMapTexturedMesh result;

  result.vertices.reserve(sphere.indices.size() * kCubeMapMeshFloatsPerVertex);

  for (uint32_t idx : sphere.indices) {
    glm::vec3 pos = sphere.positions[idx] * radius;
    glm::vec3 normal = sphere.positions[idx];  // Radial normal
    glm::vec3 tangent = sphere.tangents[idx];

    result.vertices.push_back(pos.x);
    result.vertices.push_back(pos.y);
    result.vertices.push_back(pos.z);
    result.vertices.push_back(normal.x);
    result.vertices.push_back(normal.y);
    result.vertices.push_back(normal.z);
    result.vertices.push_back(tangent.x);
    result.vertices.push_back(tangent.y);
    result.vertices.push_back(tangent.z);
  }

  result.vertex_count = static_cast<uint32_t>(sphere.indices.size());
  result.dirty = true;

  return result;
}

}  // namespace badlands
