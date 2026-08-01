#pragma once

// Triangulates the river network into the CHANNEL WATER surface: the free
// surface that sits inside the carved cavity (mapgen/river_carve.hpp), not a
// decal laid over smooth ground.
//
// The one number that decides everything here is where the surface sits:
//
//   water level(s) = carved_bed(s) + d_flow(s)
//
// so the banks stand proud of the water by whatever the carve cut beyond the
// flow depth (1.390*d + k_bank*w^0.6 against d, see river_carve.hpp). That
// freeboard is what makes a channel read as a channel; a surface at ground
// level would fill the trench and undo the carve.
//
// Two properties come from the arc chains (mapgen/river_arcs.hpp) rather than
// from any look knob:
//
//   - WIDTH is the channel's own w = k_w*sqrt(Q), sampled per cross-section, so
//     a trunk visibly swells downstream and a confluence steps up. TRUE width,
//     with no legibility floor: the cavity is what makes a sub-metre brook
//     visible, so the surface no longer has to lie about how wide it is.
//   - The CURVE is the fitted arc, not the polyline, so a meander bends instead
//     of turning in ~6 m facets.
//
// Pure CPU (mapgen + glm), so it is unit-testable without the engine -- same
// shape as lake_surface.hpp next door.

#include <functional>
#include <vector>

#include <glm/glm.hpp>

#include "mapgen/generator.hpp"
#include "mapgen/river_arcs.hpp"
#include "mapgen/river_graph.hpp"

namespace badlands {

// How far above the computed water level the surface is emitted.
//
// The bed under it is the SAME analytic field (RiverCarve::HeightAt) the
// terrain was tessellated from, so at a shallow station the two surfaces are a
// few centimetres apart and coplanar-looking to the depth buffer. One
// centimetre-scale epsilon beats that z-fighting; anything larger would start
// eating the freeboard the carve exists to show.
inline constexpr float kWaterEpsilonM = 0.02f;

// Flattening tolerance: the most an emitted chord may sag away from its arc.
// A channel's own half-width is the scale a bend is judged against and the
// median is sub-metre, so 0.15 m keeps a meander smooth relative to the thing
// being bent rather than relative to the map.
inline constexpr float kRiverSagittaM = 0.15f;

// Water-surface triangles for every reach that carries flow, in map-local world
// coordinates, wound CCW seen from +Y. Three vertices per triangle.
//
// `chains` must have been fitted from `graph` (chain.edge indexes graph.edges);
// passing chains from a different graph is a programming error, not a case this
// handles. Reaches whose chain is missing are simply not drawn.
//
// `height_at` is the CARVED terrain surface (RiverCarve::HeightAt): sampled on
// the centreline it returns the channel bed, which is why the water level needs
// nothing else. Each cross-section is FLAT at its own level across the full
// width -- water is level across a channel, and draping it on the banks is
// exactly the debug-decal look the carve replaced.
//
// `art` supplies only the raster LATTICE (its texel size sets how finely the
// centreline is sampled); the surface itself comes from `height_at`.
//
// Stations with no flow (d_flow <= 0) carry no water and are not drawn: the
// strip is emitted in runs, so a dry reach in the middle of a chain leaves a
// gap rather than a sheet of water bridging it.
std::vector<glm::vec3> BuildRiverWaterTriangles(
    const mapgen::MapArtifacts& art, float world_size_m,
    const mapgen::RiverGraph& graph,
    const std::vector<mapgen::RiverArcChain>& chains,
    const std::function<float(float wx, float wz)>& height_at);

}  // namespace badlands
