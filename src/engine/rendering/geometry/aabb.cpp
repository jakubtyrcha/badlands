// Ported from sampo's src/core/math/aabb.cpp, namespace sampo -> badlands.
// Verbatim otherwise — see the deviation note in aabb.hpp for the relocated
// path.
#include "engine/rendering/geometry/aabb.hpp"

#include <limits>

#include <glm/glm.hpp>

namespace badlands {

Aabb Aabb::FromMinMax(const glm::vec3& min, const glm::vec3& max) {
  return {.min = min, .max = max};
}

Aabb Aabb::Empty() {
  return {.min = glm::vec3(std::numeric_limits<float>::max()),
          .max = glm::vec3(std::numeric_limits<float>::lowest())};
}

glm::vec3 Aabb::Center() const { return (min + max) * 0.5f; }

glm::vec3 Aabb::Extents() const { return max - min; }

Aabb Aabb::Union(const Aabb& other) const {
  return {.min = glm::min(min, other.min), .max = glm::max(max, other.max)};
}

Aabb Aabb::TransformedBy(const glm::mat4& m) const {
  auto result = Aabb::Empty();
  for (int i = 0; i < 8; i++) {
    glm::vec3 corner((i & 1) ? max.x : min.x, (i & 2) ? max.y : min.y,
                      (i & 4) ? max.z : min.z);
    glm::vec3 world = glm::vec3(m * glm::vec4(corner, 1.0f));
    result.min = glm::min(result.min, world);
    result.max = glm::max(result.max, world);
  }
  return result;
}

}  // namespace badlands
