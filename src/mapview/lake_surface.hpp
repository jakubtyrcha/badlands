#pragma once

// Builds the standing-water surface for every lake the generator produced.
//
// Read straight from MapArtifacts rather than from the frozen MapData contract:
// MapData carries ONE global water_level_m, but the generator ponds multiple
// lakes at DIFFERENT elevations (LakeInfo::level_m per lake, indexed by
// lake_id). A single plane would be wrong for every lake but one.
//
// River channels carry lake_id == -1 and are therefore excluded -- rivers are
// out of scope for this pass.
//
// The emitted surface deliberately extends PAST each shoreline and under the
// terrain (the "skirt"). Water is depth-tested but does not write depth, so the
// terrain in front rejects the buried fragments for free; the overlap is what
// keeps a later vertical wave displacement from opening a gap at the waterline.
//
// Pure CPU (mapgen + glm), so it is unit-testable without the engine.

#include <vector>

#include <glm/glm.hpp>

#include "mapgen/generator.hpp"

namespace badlands {

// How far above a lake's level the terrain must rise before the skirt stops
// growing. The outermost ring is buried by at least this much, so a wave trough
// of up to this amplitude still has terrain in front of it.
inline constexpr float kBurialM = 0.6f;

// Hard cap on how far the skirt may run from the shoreline. A dead-flat shore
// never reaches kBurialM; the cap stops the search from flooding the map. Such
// shores are also where waves are smallest, so the shortfall is benign.
inline constexpr float kMaxSkirtM = 6.0f;

// Flat, lattice-aligned water triangles for every lake in `art`, in map-local
// world coordinates, wound CCW seen from +Y. Three vertices per triangle; each
// vertex's Y is its own lake's level_m, so lakes at different elevations can
// share one mesh. Texel (x, z) spans world [x*s, (x+1)*s] on both axes, where
// s = world_size_m / lake_id.width -- the same cell lattice the terrain mesh
// uses. Returns empty when there are no lakes.
std::vector<glm::vec3> BuildLakeSurfaceTriangles(const mapgen::MapArtifacts& art,
                                                 float world_size_m);

}  // namespace badlands
