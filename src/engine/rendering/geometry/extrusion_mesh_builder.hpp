#pragma once

// Ring-extrusion mesh for rock mesas / lake basins (Task S2.C). Ported from
// the reference build_extrusion (src/scene/ploppable.rs @ 8ee93cc), dropping
// its per-vertex terrain material `layer` float (kTexturedMesh's 11-float
// format has no such slot; the reference's terrain_pbr triplanar material
// system isn't ported yet — surface-material assignment is a game/material
// concern, out of scope for this engine-side, game-agnostic generator).

#include <glm/glm.hpp>
#include <vector>

#include "engine/rendering/geometry/textured_mesh_builders.hpp"

namespace badlands {

// Extrudes a convex world-XZ ring into a mesa (delta_y > 0) or basin
// (delta_y < 0): a top/floor face at base_y + delta_y whose ring is the base
// ring scaled toward the centroid by `shrink` (so the visible face is
// smaller than the base), plus angled wall quads connecting it back down to
// the base ring at `base_y`. Planar UVs (world XZ / 4 meters). Returns an
// empty mesh (0 vertices) if `ring` has fewer than 3 points.
TexturedMeshResult BuildExtrusionMesh(const std::vector<glm::vec2>& ring, float base_y,
                                      float delta_y, float shrink);

}  // namespace badlands
