#pragma once

// Triangulates the river network into a channel RIBBON that lies on the
// terrain -- the debug geometry for the river layer, one step up from the
// screen-space debug lines it replaces.
//
// A line has no width, so it can show WHERE a river is but nothing about what
// it is. The ribbon is built from the arc chains (mapgen/river_arcs.hpp) and
// carries the hydraulics the graph already solved:
//
//   - WIDTH is the channel's own w = k_w*sqrt(Q), sampled per vertex, so a
//     trunk visibly swells downstream and a confluence steps up. The one place
//     this stops being literal is kMinRibbonWidthM (below).
//   - The CURVE is the fitted arc, not the polyline, so a meander bends instead
//     of turning in ~6 m facets.
//
// Pure CPU (mapgen + glm), so it is unit-testable without the engine -- same
// shape as lake_surface.hpp next door.

#include <vector>

#include <glm/glm.hpp>

#include "mapgen/generator.hpp"
#include "mapgen/river_arcs.hpp"
#include "mapgen/river_graph.hpp"

namespace badlands {

// How far above the ground the ribbon floats.
//
// The channel is NOT carved into the heightmap -- no carve pass exists yet --
// so there is no bed to sit in and the ribbon is a decal on smooth terrain.
// Enough to beat depth precision at map scale, small enough that it does not
// read as a raised causeway on a slope.
inline constexpr float kRiverLiftM = 0.25f;

// Legibility floor on the drawn width.
//
// The ONE place this geometry stops being literal, so it is a named constant
// rather than a fudge inside the loop. After pruning, the surviving network
// still runs from ~0.3 m headwaters to a ~4 m trunk, and 0.3 m is a fraction of
// a texel: drawn true to width it aliases to nothing and the network looks like
// it ends partway down. The floor costs the bottom of the range its accuracy
// and keeps the topology visible; everything above it is true width.
inline constexpr float kMinRibbonWidthM = 1.5f;

// Flattening tolerance: the most an emitted chord may sag away from its arc.
// 10% of the minimum ribbon width, so a bend can never look faceted relative to
// the thing being bent.
inline constexpr float kRibbonSagittaM = 0.15f;

// Ribbon triangles for every reach with geometry, in map-local world
// coordinates, wound CCW seen from +Y. Three vertices per triangle.
//
// `chains` must have been fitted from `graph` (chain.edge indexes graph.edges);
// passing chains from a different graph is a programming error, not a case this
// handles. Reaches whose chain is missing are simply not drawn.
//
// Y comes from `art.heightmap` PLUS `art.water_depth`, sampled bilinearly on
// the same node lattice the terrain mesh is built from (node i at i*texel_m,
// edge nodes clamping to the last texel), so the ribbon sits on the rendered
// surface rather than near it -- and rides a lake surface where it meets one
// instead of vanishing under it.
std::vector<glm::vec3> BuildRiverRibbonTriangles(
    const mapgen::MapArtifacts& art, float world_size_m,
    const mapgen::RiverGraph& graph,
    const std::vector<mapgen::RiverArcChain>& chains);

}  // namespace badlands
