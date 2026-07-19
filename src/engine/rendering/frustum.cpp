#include "engine/rendering/frustum.hpp"

namespace badlands {

namespace {
// Row i of a glm (column-major) matrix.
glm::vec4 Row(const glm::mat4& m, int i) {
  return glm::vec4(m[0][i], m[1][i], m[2][i], m[3][i]);
}
glm::vec4 Normalize(glm::vec4 p) {
  const float len = glm::length(glm::vec3(p));
  return len > 0.0f ? p / len : p;
}
}  // namespace

Frustum Frustum::FromViewProj(const glm::mat4& m) {
  const glm::vec4 r0 = Row(m, 0);
  const glm::vec4 r1 = Row(m, 1);
  const glm::vec4 r2 = Row(m, 2);
  const glm::vec4 r3 = Row(m, 3);

  Frustum f;
  // Canonical clip volume is -w <= x,y <= w and 0 <= z <= w (D3D/[0,1] z, which
  // GLM_FORCE_DEPTH_ZERO_TO_ONE is set for). These six combinations bound it.
  f.planes[0] = Normalize(r3 + r0);  // left
  f.planes[1] = Normalize(r3 - r0);  // right
  f.planes[2] = Normalize(r3 + r1);  // bottom
  f.planes[3] = Normalize(r3 - r1);  // top
  f.planes[4] = Normalize(r2);       // near (z >= 0)
  f.planes[5] = Normalize(r3 - r2);  // far  (z <= w)
  return f;
}

bool Frustum::Intersects(const Aabb& box) const {
  for (const glm::vec4& plane : planes) {
    const glm::vec3 n(plane);
    // Positive vertex: the AABB corner furthest along the plane normal.
    const glm::vec3 p(n.x >= 0.0f ? box.max.x : box.min.x,
                      n.y >= 0.0f ? box.max.y : box.min.y,
                      n.z >= 0.0f ? box.max.z : box.min.z);
    if (glm::dot(n, p) + plane.w < 0.0f) return false;  // fully outside
  }
  return true;
}

}  // namespace badlands
