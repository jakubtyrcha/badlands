#pragma once

// Turning a model's bounding box into the circle the foliage sampler spaces by.
//
// Header-only and free of GPU types on purpose: this is the one piece of the
// "crowns must not intersect" rule that can actually be wrong, so it is
// testable without a device (see game/tests/crown_bounds_tests.cpp).

#include <algorithm>
#include <cmath>

#include "engine/rendering/geometry/aabb.hpp"

namespace badlands {

// Radius, in world metres, of the smallest vertical cylinder about the model's
// TRUNK AXIS (its local x = z = 0, where the generator puts the stem) that
// contains `local_bounds`. `native_to_world_scale` converts the generator's
// native units to metres for the height the model is being planted at.
//
// The largest half-extent, NOT the distance to a box corner. A crown is roughly
// a disc INSCRIBED in its own bounding box, not one circumscribing it; taking
// the corner would inflate every radius by up to sqrt(2) and, under a
// sum-of-radii spacing rule, space the whole forest out by ~40% for nothing.
// Half-extents are measured from the axis rather than from the box centre so a
// leaning tree -- whose box is off-centre -- still reports a circle that
// actually covers it when the instance is randomly yawed about that axis.
inline float CrownRadiusM(const Aabb& local_bounds,
                          float native_to_world_scale) {
  const float reach = std::max({std::abs(local_bounds.min.x),
                                std::abs(local_bounds.max.x),
                                std::abs(local_bounds.min.z),
                                std::abs(local_bounds.max.z)});
  // Aabb::Empty() is inverted (min > max), so a model that somehow produced no
  // geometry reports 0 rather than a garbage radius that would empty the map.
  if (!std::isfinite(reach) || local_bounds.min.x > local_bounds.max.x) {
    return 0.0f;
  }
  return reach * native_to_world_scale;
}

}  // namespace badlands
