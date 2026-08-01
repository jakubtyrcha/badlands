#pragma once

// The CPU coarse-cull decision for one foliage cell, as a pure function.
//
// Split out of ForestRenderer so it can be tested without a GPU: the renderer
// needs a device to exist at all, while this is the part that can actually be
// wrong -- and was. Both subtleties below were live bugs.
//
// Header-only; depends on engine geometry (Frustum/Aabb) and glm, nothing else.

#include <algorithm>
#include <cmath>

#include <glm/glm.hpp>

#include "engine/rendering/frustum.hpp"
#include "engine/rendering/geometry/aabb.hpp"

namespace badlands {

// Cap on how far a cell's shadow is chased. Near sunrise/sunset the geometric
// reach runs to infinity as the sun approaches the horizon, and past the shadow
// map's own coverage the shadow is not rendered anyway -- so an unbounded sweep
// would keep the entire map for nothing.
inline constexpr float kMaxShadowReachM = 200.0f;

// A cell's world bounds: its XZ footprint PADDED by `crown_pad_m`, with the Y
// extent its instances actually occupy.
//
// The padding is the first subtlety. A cell holds the trees whose TRUNKS are
// inside it, but a 22 m oak planted a metre from the edge has a crown metres
// wider than the cell -- so the bare footprint culls trees that are still
// partly on screen, and they pop in at the frame edge as the camera pans.
inline Aabb FoliageCellBounds(glm::vec2 cell_origin_m, float cell_size_m,
                              float min_y, float max_y, float crown_pad_m) {
  return Aabb{
      glm::vec3(cell_origin_m.x - crown_pad_m, min_y,
                cell_origin_m.y - crown_pad_m),
      glm::vec3(cell_origin_m.x + cell_size_m + crown_pad_m, max_y,
                cell_origin_m.y + cell_size_m + crown_pad_m)};
}

// Should this cell be uploaded? True if it is visible, OR if its shadow could
// fall into view.
//
// The shadow half is the second subtlety, and the reason this is not just a
// frustum test. GpuInstanceRenderer runs a SEPARATE light-frustum cull so
// off-screen geometry still casts into view -- but both cull sets read the one
// uploaded instance buffer, so an instance dropped here can never be recovered
// by it. Culling on the camera alone silently deletes shadow casters: a stand
// just off-screen drops its shadow off ground that is still visible, and
// shadows flicker as the visible cell set flips.
//
// `sun_direction` points TOWARD the sun (SceneContext::sun_direction's
// convention, see shadow_map.cpp), so shadows run along its negation. The sweep
// runs until the cell's top would reach `min_ground_y`, the lowest ground any
// instance stands on.
inline bool FoliageCellVisibleOrCasts(const Frustum& frustum,
                                      const Aabb& cell_box,
                                      const glm::vec3& sun_direction,
                                      float min_ground_y) {
  if (frustum.Intersects(cell_box)) return true;

  const glm::vec3 sun = glm::normalize(sun_direction);
  if (!(sun.y > 0.01f)) return false;  // at or below the horizon: nothing casts

  const float fall = std::max(0.0f, cell_box.max.y - min_ground_y);
  const float reach = std::min(fall / sun.y, kMaxShadowReachM);
  const glm::vec3 offset = -sun * reach;

  const Aabb swept =
      cell_box.Union(Aabb{cell_box.min + offset, cell_box.max + offset});
  return frustum.Intersects(swept);
}

}  // namespace badlands
