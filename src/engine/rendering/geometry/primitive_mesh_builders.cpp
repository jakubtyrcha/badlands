#include "engine/rendering/geometry/primitive_mesh_builders.hpp"

#include <glm/gtc/constants.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <tuple>

#include "engine/rendering/geometry/mesh_builder_utils.hpp"

namespace badlands {

namespace {

TexturedMeshResult Finish(std::vector<float>&& vertices, const Aabb& bounds) {
  StaticTexturedMeshComponent mesh;
  mesh.vertices = std::move(vertices);
  mesh.vertex_count = static_cast<uint32_t>(mesh.vertices.size() / kTexturedMeshFloatsPerVertex);
  mesh.dirty = true;
  return {.mesh = std::move(mesh), .local_bounds = bounds};
}

}  // namespace

TexturedMeshResult GenerateCube(glm::vec3 half_extents) {
  struct Face {
    glm::vec3 normal;
    glm::vec3 tangent;
    glm::vec3 corners[4];  // CCW viewed from outside
  };

  const glm::vec3& he = half_extents;
  const Face faces[6] = {
      // +Y (top)
      {glm::vec3(0, 1, 0),
       glm::vec3(1, 0, 0),
       {glm::vec3(-he.x, he.y, -he.z), glm::vec3(-he.x, he.y, he.z),
        glm::vec3(he.x, he.y, he.z), glm::vec3(he.x, he.y, -he.z)}},
      // +X
      {glm::vec3(1, 0, 0),
       glm::vec3(0, 0, -1),
       {glm::vec3(he.x, he.y, he.z), glm::vec3(he.x, -he.y, he.z),
        glm::vec3(he.x, -he.y, -he.z), glm::vec3(he.x, he.y, -he.z)}},
      // -X
      {glm::vec3(-1, 0, 0),
       glm::vec3(0, 0, 1),
       {glm::vec3(-he.x, he.y, -he.z), glm::vec3(-he.x, -he.y, -he.z),
        glm::vec3(-he.x, -he.y, he.z), glm::vec3(-he.x, he.y, he.z)}},
      // +Z
      {glm::vec3(0, 0, 1),
       glm::vec3(1, 0, 0),
       {glm::vec3(-he.x, he.y, he.z), glm::vec3(-he.x, -he.y, he.z),
        glm::vec3(he.x, -he.y, he.z), glm::vec3(he.x, he.y, he.z)}},
      // -Z
      {glm::vec3(0, 0, -1),
       glm::vec3(-1, 0, 0),
       {glm::vec3(he.x, he.y, -he.z), glm::vec3(he.x, -he.y, -he.z),
        glm::vec3(-he.x, -he.y, -he.z), glm::vec3(-he.x, he.y, -he.z)}},
      // -Y (bottom)
      {glm::vec3(0, -1, 0),
       glm::vec3(1, 0, 0),
       {glm::vec3(-he.x, -he.y, -he.z), glm::vec3(he.x, -he.y, -he.z),
        glm::vec3(he.x, -he.y, he.z), glm::vec3(-he.x, -he.y, he.z)}},
  };

  const glm::vec2 uvs[4] = {{0.0f, 0.0f}, {0.0f, 1.0f}, {1.0f, 1.0f}, {1.0f, 0.0f}};
  const int tri[6] = {0, 1, 2, 0, 2, 3};

  std::vector<float> verts;
  verts.reserve(6 * 6 * kTexturedMeshFloatsPerVertex);
  for (const Face& f : faces) {
    for (int i : tri) {
      PushVertex(verts, f.corners[i], uvs[i], f.normal, f.tangent);
    }
  }

  return Finish(std::move(verts), Aabb::FromMinMax(-he, he));
}

TexturedMeshResult GenerateCylinder(float radius, float height, int segments) {
  std::vector<float> verts;
  verts.reserve(static_cast<size_t>(segments) * 12 * kTexturedMeshFloatsPerVertex);

  const glm::vec3 top_center(0.0f, height, 0.0f);
  const glm::vec3 bottom_center(0.0f, 0.0f, 0.0f);
  auto cap_uv = [radius](const glm::vec3& p) {
    return glm::vec2(p.x / (2.0f * radius) + 0.5f, p.z / (2.0f * radius) + 0.5f);
  };

  for (int j = 0; j < segments; ++j) {
    float theta0 = glm::two_pi<float>() * static_cast<float>(j) / static_cast<float>(segments);
    float theta1 =
        glm::two_pi<float>() * static_cast<float>(j + 1) / static_cast<float>(segments);
    float c0 = std::cos(theta0), s0 = std::sin(theta0);
    float c1 = std::cos(theta1), s1 = std::sin(theta1);
    float u0 = static_cast<float>(j) / static_cast<float>(segments);
    float u1 = static_cast<float>(j + 1) / static_cast<float>(segments);

    glm::vec3 b0(radius * c0, 0.0f, radius * s0);
    glm::vec3 b1(radius * c1, 0.0f, radius * s1);
    glm::vec3 t0(radius * c0, height, radius * s0);
    glm::vec3 t1(radius * c1, height, radius * s1);
    glm::vec3 n0(c0, 0.0f, s0), n1(c1, 0.0f, s1);
    glm::vec3 tan0(-s0, 0.0f, c0), tan1(-s1, 0.0f, c1);

    // Side (two triangles), smooth radial normals, tangent along circumference.
    // Wound so the geometric (front-face) normal matches the outward radial
    // shading normal (CullMode::Back / FrontFace::CCW keeps the outer surface).
    PushVertex(verts, b0, {u0, 0.0f}, n0, tan0);
    PushVertex(verts, t1, {u1, 1.0f}, n1, tan1);
    PushVertex(verts, b1, {u1, 0.0f}, n1, tan1);
    PushVertex(verts, b0, {u0, 0.0f}, n0, tan0);
    PushVertex(verts, t0, {u0, 1.0f}, n0, tan0);
    PushVertex(verts, t1, {u1, 1.0f}, n1, tan1);

    // Top cap fan, normal +Y (wound CCW seen from above / outside).
    PushVertex(verts, t0, cap_uv(t0), glm::vec3(0, 1, 0), glm::vec3(1, 0, 0));
    PushVertex(verts, top_center, glm::vec2(0.5f, 0.5f), glm::vec3(0, 1, 0), glm::vec3(1, 0, 0));
    PushVertex(verts, t1, cap_uv(t1), glm::vec3(0, 1, 0), glm::vec3(1, 0, 0));

    // Bottom cap fan, normal -Y (wound CCW seen from below / outside).
    PushVertex(verts, b1, cap_uv(b1), glm::vec3(0, -1, 0), glm::vec3(1, 0, 0));
    PushVertex(verts, bottom_center, glm::vec2(0.5f, 0.5f), glm::vec3(0, -1, 0),
              glm::vec3(1, 0, 0));
    PushVertex(verts, b0, cap_uv(b0), glm::vec3(0, -1, 0), glm::vec3(1, 0, 0));
  }

  return Finish(std::move(verts), Aabb::FromMinMax(glm::vec3(-radius, 0.0f, -radius),
                                                    glm::vec3(radius, height, radius)));
}

TexturedMeshResult GenerateCone(float radius, float height, int segments) {
  std::vector<float> verts;
  verts.reserve(static_cast<size_t>(segments) * 6 * kTexturedMeshFloatsPerVertex);

  const glm::vec3 apex(0.0f, height, 0.0f);
  const glm::vec3 base_center(0.0f, 0.0f, 0.0f);
  auto cap_uv = [radius](const glm::vec3& p) {
    return glm::vec2(p.x / (2.0f * radius) + 0.5f, p.z / (2.0f * radius) + 0.5f);
  };

  for (int j = 0; j < segments; ++j) {
    float theta0 = glm::two_pi<float>() * static_cast<float>(j) / static_cast<float>(segments);
    float theta1 =
        glm::two_pi<float>() * static_cast<float>(j + 1) / static_cast<float>(segments);
    glm::vec3 b0(radius * std::cos(theta0), 0.0f, radius * std::sin(theta0));
    glm::vec3 b1(radius * std::cos(theta1), 0.0f, radius * std::sin(theta1));
    float u0 = static_cast<float>(j) / static_cast<float>(segments);
    float u1 = static_cast<float>(j + 1) / static_cast<float>(segments);

    glm::vec3 edge = b1 - b0;
    glm::vec3 n = glm::length(edge) > 1e-8f ? glm::normalize(glm::cross(apex - b0, edge))
                                            : glm::vec3(0, 1, 0);
    glm::vec3 tan = glm::length(edge) > 1e-8f ? glm::normalize(edge) : glm::vec3(1, 0, 0);

    // Side: base0 -> apex -> base1. Wound so the geometric (front-face) normal
    // matches the outward shading normal `n`, so the outer surface survives the
    // deferred material's default CullMode::Back / FrontFace::CCW.
    PushVertex(verts, b0, {u0, 0.0f}, n, tan);
    PushVertex(verts, apex, {(u0 + u1) * 0.5f, 1.0f}, n, tan);
    PushVertex(verts, b1, {u1, 0.0f}, n, tan);

    // Base cap (downward), matching GenerateCylinder's bottom cap.
    PushVertex(verts, b1, cap_uv(b1), glm::vec3(0, -1, 0), glm::vec3(1, 0, 0));
    PushVertex(verts, base_center, glm::vec2(0.5f, 0.5f), glm::vec3(0, -1, 0), glm::vec3(1, 0, 0));
    PushVertex(verts, b0, cap_uv(b0), glm::vec3(0, -1, 0), glm::vec3(1, 0, 0));
  }

  return Finish(std::move(verts), Aabb::FromMinMax(glm::vec3(-radius, 0.0f, -radius),
                                                    glm::vec3(radius, height, radius)));
}

TexturedMeshResult GenerateGableRoof(glm::vec3 size) {
  std::vector<float> verts;

  float lo_x = -size.x * 0.5f, hi_x = size.x * 0.5f;
  float lo_z = -size.z * 0.5f, hi_z = size.z * 0.5f;
  float ridge_y = size.y;

  glm::vec3 ridge_a(lo_x, ridge_y, 0.0f);
  glm::vec3 ridge_b(hi_x, ridge_y, 0.0f);
  // +Z slope.
  glm::vec3 p0(lo_x, 0.0f, hi_z);
  glm::vec3 p1(hi_x, 0.0f, hi_z);
  // -Z slope.
  glm::vec3 q0(hi_x, 0.0f, lo_z);
  glm::vec3 q1(lo_x, 0.0f, lo_z);

  auto quad = [&verts](const glm::vec3& a, const glm::vec3& b, const glm::vec3& c,
                       const glm::vec3& d) {
    glm::vec3 n = glm::normalize(glm::cross(b - a, c - a));
    glm::vec3 tan = glm::normalize(b - a);
    const glm::vec2 uv[4] = {{0.0f, 1.0f}, {1.0f, 1.0f}, {1.0f, 0.0f}, {0.0f, 0.0f}};
    const glm::vec3 v[4] = {a, b, c, d};
    const int tri[6] = {0, 1, 2, 0, 2, 3};
    for (int i : tri) {
      PushVertex(verts, v[i], uv[i], n, tan);
    }
  };
  quad(p0, p1, ridge_b, ridge_a);  // +Z face
  quad(q0, q1, ridge_a, ridge_b);  // -Z face

  // Gable end triangles at x = lo_x and x = hi_x. The reference used a
  // degenerate constant UV here ([0,0] for every vertex); this port gives
  // each end a proper (non-degenerate) triangle UV instead.
  auto tri_face = [&verts](const glm::vec3& a, const glm::vec3& b, const glm::vec3& c) {
    glm::vec3 n = glm::normalize(glm::cross(b - a, c - a));
    glm::vec3 tan = glm::normalize(b - a);
    const glm::vec2 uv[3] = {{0.0f, 0.0f}, {1.0f, 0.0f}, {0.5f, 1.0f}};
    const glm::vec3 v[3] = {a, b, c};
    for (int i = 0; i < 3; ++i) {
      PushVertex(verts, v[i], uv[i], n, tan);
    }
  };
  tri_face(q1, p0, ridge_a);  // x = lo_x: (lo,0,lo)->(lo,0,hi)->ridge
  tri_face(p1, q0, ridge_b);  // x = hi_x: (hi,0,hi)->(hi,0,lo)->ridge

  return Finish(std::move(verts),
               Aabb::FromMinMax(glm::vec3(lo_x, 0.0f, lo_z), glm::vec3(hi_x, ridge_y, hi_z)));
}

TexturedMeshResult GenerateCapsule(float radius, float cylinder_height, int segments) {
  constexpr int kCapRings = 5;  // latitude rings per hemisphere

  const float bottom_center_y = radius;
  const float top_center_y = radius + cylinder_height;
  const float total_height = 2.0f * radius + cylinder_height;

  // Ring definitions: (y, xz_radius, normal_y). The bottom-hemisphere equator
  // and the top-hemisphere equator (both normal_y = 0) bound the cylinder.
  std::vector<std::array<float, 3>> rings;
  rings.reserve(2 * (kCapRings + 1));
  for (int i = 0; i <= kCapRings; ++i) {
    float phi = -glm::half_pi<float>() +
               glm::half_pi<float>() * (static_cast<float>(i) / static_cast<float>(kCapRings));
    rings.push_back({bottom_center_y + radius * std::sin(phi), radius * std::cos(phi),
                     std::sin(phi)});
  }
  for (int i = 0; i <= kCapRings; ++i) {
    float phi = glm::half_pi<float>() * (static_cast<float>(i) / static_cast<float>(kCapRings));
    rings.push_back(
        {top_center_y + radius * std::sin(phi), radius * std::cos(phi), std::sin(phi)});
  }

  auto vert = [segments, total_height](float y, float rad, float ny,
                                       int j) -> std::tuple<glm::vec3, glm::vec2, glm::vec3, glm::vec3> {
    float theta = glm::two_pi<float>() * static_cast<float>(j) / static_cast<float>(segments);
    float st = std::sin(theta), ct = std::cos(theta);
    float nxz = std::sqrt(std::max(0.0f, 1.0f - ny * ny));
    return {glm::vec3(rad * ct, y, rad * st),
           glm::vec2(static_cast<float>(j) / static_cast<float>(segments), y / total_height),
           glm::vec3(nxz * ct, ny, nxz * st), glm::vec3(-st, 0.0f, ct)};
  };

  std::vector<float> verts;
  verts.reserve((rings.size() - 1) * static_cast<size_t>(segments) * 6 *
               kTexturedMeshFloatsPerVertex);
  for (size_t i = 0; i + 1 < rings.size(); ++i) {
    auto [y0, r0, ny0] = rings[i];
    auto [y1, r1, ny1] = rings[i + 1];
    for (int j = 0; j < segments; ++j) {
      auto a = vert(y0, r0, ny0, j);
      auto b = vert(y1, r1, ny1, j);
      auto c = vert(y1, r1, ny1, j + 1);
      auto d = vert(y0, r0, ny0, j + 1);
      for (const auto& v : {a, b, c, a, c, d}) {
        PushVertex(verts, std::get<0>(v), std::get<1>(v), std::get<2>(v), std::get<3>(v));
      }
    }
  }

  return Finish(std::move(verts), Aabb::FromMinMax(glm::vec3(-radius, 0.0f, -radius),
                                                    glm::vec3(radius, total_height, radius)));
}

}  // namespace badlands
