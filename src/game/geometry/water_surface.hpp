#pragma once

// Water surface geometry, DERIVED from the map rather than stored in it -- the
// frozen MapData contract carries only biome slices + a heightmap, so the lake
// shape is recomputed here from "which cells are Lake-dominant".
//
// Pure CPU (glm + MapData): returns a flat triangle soup so it can be unit
// tested without the engine; the caller wraps it in a render mesh.

#include <vector>

#include <glm/glm.hpp>

#include "game/map/map_data.hpp"

namespace badlands {

// The water surface as lattice-aligned triangles at map.water_level_m(), in
// map-local world coordinates, wound CCW seen from +Y. Three vertices per
// triangle; every vertex is a lattice-cell corner or a cell CENTRE, so the water
// sits on exactly the same X-split lattice as the terrain mesh.
//
//   * A Lake-dominant cell contributes all 4 of its X-split triangles.
//   * A land cell with exactly two PERPENDICULAR lake neighbours contributes the
//     2 triangles facing them. Because the cell centre lies ON the cell
//     diagonal, that pair is exactly the half-cell cut by that diagonal -- a
//     clean 45-degree shore that turns the cell staircase into a smooth
//     diagonal. Any other configuration (a lone neighbour, or an opposite pair)
//     would emit a spike or a bowtie, so it is skipped.
std::vector<glm::vec3> BuildWaterSurfaceTriangles(const MapData& map);

}  // namespace badlands
