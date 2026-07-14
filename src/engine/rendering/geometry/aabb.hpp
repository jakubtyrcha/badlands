#pragma once

// Ported from sampo's src/core/math/aabb.{hpp,cpp}, namespace sampo ->
// badlands, verbatim logic.
//
// Deviation: relocated from sampo's `core/math/aabb.hpp` to
// `engine/rendering/geometry/aabb.hpp`. Task D2's global constraints only
// allow adding files under `src/engine/scene/**`,
// `src/engine/rendering/components/**`, and `src/engine/rendering/geometry/**`
// (+ scene_context.hpp) — there's no `src/core/math/` (or similar
// general-purpose core-math) tree in badlands yet to port sampo's directory
// structure into verbatim. `Aabb` is a hard transitive dependency of
// `ResolvedMesh`/`StaticMeshAabbComponent`/the mesh builders below, all of
// which live under `rendering/geometry` or `rendering/components`, so this is
// the closest allowed home. A future task that ports more of sampo's
// `core/math/` can relocate this without changing its API.
#include <glm/glm.hpp>

namespace badlands {

struct Aabb {
  glm::vec3 min{0.0f};
  glm::vec3 max{0.0f};

  static Aabb FromMinMax(const glm::vec3& min, const glm::vec3& max);
  static Aabb Empty();

  glm::vec3 Center() const;
  glm::vec3 Extents() const;

  Aabb Union(const Aabb& other) const;

  // Transform all 8 corners by the matrix, return axis-aligned bounds of the
  // result.
  Aabb TransformedBy(const glm::mat4& m) const;
};

}  // namespace badlands
