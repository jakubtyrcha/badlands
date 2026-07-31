#pragma once

// Arena shapes, as lists of buildings to plop.
//
// There is nothing special about an arena. The output is ordinary
// PlacementDescs that go into WorldConfig::plops and become ordinary Walls; the
// navmesh routes around them exactly as it routes around a house, and no game
// system ever learns that these particular walls form a ring. "Arena" is a
// pattern the sandbox knows how to lay out, and only the sandbox.
//
// EVERYTHING IS BUILT ON THE BLOCK LATTICE. A Wall is 4x4 tiles and snaps to an
// integer centre, so blocks centred on multiples of 4 TILE THE PLANE exactly --
// block (i, j) covers world [4i-2, 4i+2] x [4j-2, 4j+2]. Two consequences, and
// both are why the shapes are defined this way rather than by tracing outlines:
//
//   * No two plops can overlap, so none is ever refused, so a wall can never
//     come out with a hole in it.
//   * A shape is a PREDICATE over block coordinates, and its wall is the
//     8-neighbour dilation of its interior minus the interior. That is sealed
//     by construction: any path leaving the interior enters a wall block, and
//     the 8- rather than 4-neighbour dilation is what closes the corner-to-
//     corner diagonal gaps a staircase would otherwise leave.
//
// The cost is that diagonals are block staircases rather than true 45-degree
// faces. At 4 m per block against a 40 m arena that reads as a chamfer, and it
// buys the two guarantees above outright.

#include <vector>

#include <glm/glm.hpp>

#include "badlands_sim.hpp"  // PlacementDesc

namespace badlands {

enum class ArenaShape : int32_t {
    // A long corridor: room to run, nowhere to hide. The kiting reference --
    // its long axis is several times a bow's reach.
    Tube = 0,
    // Wide and corner-less. No straight line longer than the width and no
    // corner to be pinned in: the tube's opposite.
    Octagon,
    // A square stood on its diagonal, with four block-sized columns ringing an
    // open centre. The only shape where a runner can break line of pursuit.
    Diamond,
    Count
};

const char* arena_shape_name(ArenaShape s);

struct ArenaLayout {
    std::vector<PlacementDesc> plops;  // walls first, then columns
    glm::vec2 spawn_a{};               // opposed interior points, both reachable
    glm::vec2 spawn_b{};
    glm::vec2 half_extent{};           // outer footprint half-size (floor + framing)
};

ArenaLayout build_arena(ArenaShape shape);

// World XZ centre of block (i, j). Exposed because the tests reason in blocks.
glm::vec2 arena_block_center(int i, int j);

}  // namespace badlands
