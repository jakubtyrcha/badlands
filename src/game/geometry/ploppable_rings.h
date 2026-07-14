#pragma once

// Ploppable footprint rings (rocks, sinkhole) — game data consumed by the
// engine-agnostic BuildExtrusionMesh(ring, ...)
// (engine/rendering/geometry/extrusion_mesh_builder.hpp) to build the actual
// mesa/basin mesh. Ported from the reference ploppable_local_ring +
// GamePloppableKind (game/src/ploppable.{h,cpp} @ 8ee93cc); the kind
// discriminants are local to this port — badlands_game.h has no ploppable
// enum yet to mirror.

#include <glm/glm.hpp>
#include <vector>

namespace badlands {

enum class GamePloppableKind {
  RockA = 0,
  RockB,
  RockC,
  Tree,      // footprint-less: rendered as a cone (GenerateCone), not extruded
  Sinkhole,
};

// Local (origin-centered), convex, CCW footprint ring for `kind`, rotated by
// `rot * 45` degrees (matching the building rotation_index convention).
// Empty for footprint-less kinds (Tree).
std::vector<glm::vec2> ploppable_local_ring(GamePloppableKind kind, int rot);

}  // namespace badlands
