#pragma once

// Assembles a building's shape (walls + roof/towers) from the primitive
// generators (Task S2.C). Game-agnostic: takes plain dimensions + a RoofShape
// (src/core/roof_shape.hpp) and returns parts tagged by structural role only
// — no MaterialId here, the caller (game layer) assigns materials per part
// kind (see game/building_catalog.h).
//
// Reproduces the reference per-building assembly in src/scene/renderer.rs
// (@ 8ee93cc, around `RoofStyle::Gable`/`CornerTowers`), which scaled shared
// unit meshes (build_unit_cube/build_gable_roof/build_cylinder/build_cone)
// via a per-draw model matrix. Here the equivalent scale factors are baked
// directly into each generator call (see building_parts_builder.cpp) since
// this task's generators take real dimensions, not "unit shape + external
// scale" — see primitive_mesh_builders.hpp.

#include <vector>

#include "core/roof_shape.hpp"
#include "engine/rendering/geometry/textured_mesh_builders.hpp"

namespace badlands {

enum class BuildingPartKind { Wall, Roof, Tower };

struct BuildingPart {
  TexturedMeshResult mesh;
  BuildingPartKind kind;
};

// Walls: a width x height x depth box centered in XZ, base at y=0, top at
// y=height. Roof: `Gable` adds one Roof part atop the walls; `CornerTowers`
// adds four Tower parts (one merged cylinder+cone-cap mesh per corner, no
// separate roof part over the walls — matching the reference, which draws
// only the four towers for RoofStyle::CornerTowers); `None` returns walls
// only.
std::vector<BuildingPart> BuildBuildingParts(float width, float depth, float height,
                                             RoofShape roof);

}  // namespace badlands
