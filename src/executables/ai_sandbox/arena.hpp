#pragma once

// Arena shapes, as lists of buildings to plop.
//
// There is nothing special about an arena. The output is ordinary
// PlacementDescs that go into WorldConfig::plops and become ordinary Walls; the
// navmesh routes around them exactly as it routes around a house, and no game
// system ever learns that these particular walls form a ring. "Arena" is a
// pattern the sandbox knows how to lay out, and only the sandbox.
//
// SHAPES ARE BUILT FROM WALL RUNS ON BOTH LATTICES the placement grid provides.
// The grid is a TRIANGLE grid (four triangles per tile, placement.h's
// tri_index), and a footprint may be axis-aligned OR diagonal -- rotation 1
// snaps a building to the (u, v) = (x+z, x-z) lattice instead of (x, z). So:
//
//   * an AXIS run is 4x4 blocks stepping 4 m along x or z; consecutive blocks
//     share a face.
//   * a DIAGONAL run is rotation-1 blocks stepping 6 in u (or v); each covers
//     a 6-wide band in the other coordinate, and consecutive blocks share a
//     face there too. The result is a true 45-degree wall, not a staircase.
//
// Runs may OVERLAP where they meet, and must: a slanted boundary and a vertical
// one cannot coincide, so the octagon's flat side meeting its cut corner is
// either a small overlap or a hole. plop_building allows the overlap precisely
// so the hole is not the only alternative (see placement.h).

#include <vector>

#include <glm/glm.hpp>

#include "badlands_sim.hpp"  // PlacementDesc

namespace badlands {

enum class ArenaShape : int32_t {
    // A long corridor: room to run, nowhere to hide. The kiting reference --
    // its long axis is several times a bow's reach. Axis runs only.
    Tube = 0,
    // Eight sides: a square with its corners cut, four axis-aligned faces and
    // four 45-degree ones. Wide, corner-less, and nowhere to be pinned: the
    // tube's opposite.
    Octagon,
    // A rhombus -- four 45-degree sides and nothing else -- with four
    // block-sized columns ringing an open centre. The only shape where a runner
    // can break line of pursuit.
    Rhomboid,
    Count
};

const char* arena_shape_name(ArenaShape s);

struct ArenaLayout {
    std::vector<PlacementDesc> plops;  // walls first, then columns
    glm::vec2 spawn_a{};               // opposed interior points, both reachable
    glm::vec2 spawn_b{};
    // No extent field: the host measures the world it was given rather than
    // being told, so it stays ignorant of what shape a mode thinks it built.
};

ArenaLayout build_arena(ArenaShape shape);

}  // namespace badlands
