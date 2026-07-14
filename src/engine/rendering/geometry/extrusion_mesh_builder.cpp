#include "engine/rendering/geometry/extrusion_mesh_builder.hpp"

#include "engine/rendering/geometry/mesh_builder_utils.hpp"

namespace badlands {

namespace {

constexpr float kUvMeters = 4.0f;

void PushExtrusionVertex(std::vector<float>& out, const glm::vec3& pos, const glm::vec3& normal,
                         const glm::vec3& tangent) {
  PushVertex(out, pos, glm::vec2(pos.x / kUvMeters, pos.z / kUvMeters), normal, tangent);
}

}  // namespace

TexturedMeshResult BuildExtrusionMesh(const std::vector<glm::vec2>& ring, float base_y,
                                      float delta_y, float shrink) {
  const size_t n = ring.size();
  if (n < 3) {
    StaticTexturedMeshComponent empty;
    return {.mesh = std::move(empty), .local_bounds = Aabb::Empty()};
  }

  glm::vec2 centroid(0.0f);
  for (const glm::vec2& p : ring) {
    centroid += p;
  }
  centroid /= static_cast<float>(n);

  const float top_y = base_y + delta_y;
  std::vector<glm::vec2> top(n);
  for (size_t i = 0; i < n; ++i) {
    top[i] = centroid + (ring[i] - centroid) * shrink;
  }

  std::vector<float> verts;
  verts.reserve((n * 3 + n * 6) * kTexturedMeshFloatsPerVertex);

  // Top / floor face (fan), facing up.
  const glm::vec3 c_top(centroid.x, top_y, centroid.y);
  for (size_t i = 0; i < n; ++i) {
    glm::vec3 a(top[i].x, top_y, top[i].y);
    glm::vec3 b(top[(i + 1) % n].x, top_y, top[(i + 1) % n].y);
    PushExtrusionVertex(verts, a, glm::vec3(0, 1, 0), glm::vec3(1, 0, 0));
    PushExtrusionVertex(verts, b, glm::vec3(0, 1, 0), glm::vec3(1, 0, 0));
    PushExtrusionVertex(verts, c_top, glm::vec3(0, 1, 0), glm::vec3(1, 0, 0));
  }

  // Angled walls (one quad per base edge). Normals point outward for a mesa
  // and inward for a basin, so the visible face lights correctly. This
  // geometry is drawn with the deferred normalmapped material's default
  // back-face culling (not cull_mode None); the winding below is authored
  // to be front-facing in world space accordingly.
  const bool want_outward = delta_y >= 0.0f;
  for (size_t i = 0; i < n; ++i) {
    size_t j = (i + 1) % n;
    glm::vec3 base_i(ring[i].x, base_y, ring[i].y);
    glm::vec3 base_j(ring[j].x, base_y, ring[j].y);
    glm::vec3 top_i(top[i].x, top_y, top[i].y);
    glm::vec3 top_j(top[j].x, top_y, top[j].y);

    glm::vec3 cross_v = glm::cross(base_j - base_i, top_i - base_i);
    glm::vec3 normal =
        glm::length(cross_v) > 1e-8f ? glm::normalize(cross_v) : glm::vec3(0, 1, 0);

    glm::vec2 mid = (ring[i] + ring[j]) * 0.5f;
    glm::vec2 outward = mid - centroid;
    glm::vec2 horiz(normal.x, normal.z);
    if ((glm::dot(horiz, outward) < 0.0f) == want_outward) {
      normal = -normal;
    }

    glm::vec3 edge = base_j - base_i;
    glm::vec3 tangent = glm::length(edge) > 1e-8f ? glm::normalize(edge) : glm::vec3(1, 0, 0);

    for (const glm::vec3& v : {base_i, base_j, top_j, base_i, top_j, top_i}) {
      PushExtrusionVertex(verts, v, normal, tangent);
    }
  }

  StaticTexturedMeshComponent mesh;
  mesh.vertices = std::move(verts);
  mesh.vertex_count = static_cast<uint32_t>(mesh.vertices.size() / kTexturedMeshFloatsPerVertex);
  mesh.dirty = true;

  Aabb bounds = ComputeLocalAabbFromVertices(mesh.vertices, kTexturedMeshFloatsPerVertex);
  return {.mesh = std::move(mesh), .local_bounds = bounds};
}

}  // namespace badlands
