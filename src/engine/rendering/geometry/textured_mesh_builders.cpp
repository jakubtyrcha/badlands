// Ported from sampo's src/rendering/geometry/textured_mesh_builders.cpp,
// namespace sampo -> badlands, verbatim otherwise (includes adapted to
// badlands paths; `core/math/cube_sphere.hpp` -> the relocated
// `engine/rendering/geometry/cube_sphere.hpp`, see the deviation note in
// aabb.hpp).
#include "engine/rendering/geometry/textured_mesh_builders.hpp"

#include <algorithm>

#include "engine/rendering/geometry/cube_sphere.hpp"
#include "engine/rendering/geometry/mesh_builder_utils.hpp"

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

TexturedMeshResult GenerateQuadTexturedMesh(float size, int resolution,
                                            float uv_scale) {
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
      result.vertices.push_back(u * uv_scale);
      result.vertices.push_back(v * uv_scale);
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
      glm::vec2 uv = quad.uvs[idx] * uv_scale;
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

TexturedMeshResult GenerateHeightmapMesh(
    float size, int resolution,
    const std::function<float(float, float)>& height_fn, float uv_scale) {
  TexturedMeshResult result;
  result.mesh.geometry_type = GeometryType::kTexturedMesh;

  const int res = std::max(resolution, 1);
  const int samples = res + 1;
  const float half = size * 0.5f;
  const float step = size / static_cast<float>(res);
  const float eps = step * 0.5f;

  // Precompute a shared grid of position/normal/tangent/uv so adjacent cells
  // reuse smooth per-vertex normals (central-difference of height_fn).
  struct GridVertex {
    glm::vec3 pos;
    glm::vec3 normal;
    glm::vec3 tangent;
    glm::vec2 uv;
  };
  std::vector<GridVertex> grid(static_cast<size_t>(samples) * samples);
  for (int j = 0; j < samples; ++j) {
    for (int i = 0; i < samples; ++i) {
      const float x = -half + static_cast<float>(i) * step;
      const float z = -half + static_cast<float>(j) * step;
      const float y = height_fn(x, z);
      const float dhdx =
          (height_fn(x + eps, z) - height_fn(x - eps, z)) / (2.0f * eps);
      const float dhdz =
          (height_fn(x, z + eps) - height_fn(x, z - eps)) / (2.0f * eps);
      grid[static_cast<size_t>(j) * samples + i] = {
          glm::vec3(x, y, z),
          glm::normalize(glm::vec3(-dhdx, 1.0f, -dhdz)),
          glm::normalize(glm::vec3(1.0f, dhdx, 0.0f)),
          glm::vec2((x + half) / size, (z + half) / size) * uv_scale};
    }
  }

  auto& v = result.mesh.vertices;
  v.reserve(static_cast<size_t>(res) * res * 6 * kTexturedMeshFloatsPerVertex);
  auto push = [&](const GridVertex& g) {
    PushVertex(v, g.pos, g.uv, g.normal, g.tangent);
  };
  for (int j = 0; j + 1 < samples; ++j) {
    for (int i = 0; i + 1 < samples; ++i) {
      const GridVertex& a = grid[static_cast<size_t>(j) * samples + i];
      const GridVertex& b = grid[static_cast<size_t>(j) * samples + (i + 1)];
      const GridVertex& c = grid[static_cast<size_t>(j + 1) * samples + (i + 1)];
      const GridVertex& d = grid[static_cast<size_t>(j + 1) * samples + i];
      // Two triangles, wound CCW as seen from +Y.
      push(a);
      push(c);
      push(b);
      push(a);
      push(d);
      push(c);
    }
  }

  result.mesh.vertex_count =
      static_cast<uint32_t>(v.size() / kTexturedMeshFloatsPerVertex);
  result.local_bounds =
      ComputeLocalAabbFromVertices(v, kTexturedMeshFloatsPerVertex);
  return result;
}

}  // namespace badlands
