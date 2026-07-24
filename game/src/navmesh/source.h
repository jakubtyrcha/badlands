// The nav core's only view of the world (pure CPU, world XZ).
//
// Everything the quadtree / HPA* layers read comes through NavSource, and
// NOTHING else -- no MapData, no EnTT, no BadlandsGame. That boundary is what
// lets the whole nav core be unit-tested against a hand-built synthetic grid
// (see game/tests/navmesh_tests.cpp), and what lets the real sim back it with a
// MapData + placement.blocked adapter (Stage 2) without the core knowing.
//
// The grid is a SQUARE with a power-of-two side, so the quadtree subdivides
// cleanly with no padding. The shipping map is 256x256 @ 1 m = 2^8 exactly.

#pragma once

#include <glm/glm.hpp>

#include <limits>

namespace badlands::nav {

// Terrain a unit cannot enter (open water, cliffs). A cost, not a separate flag,
// so the merge criterion and the pathfinder treat "too costly to cross" and
// "cannot cross" uniformly.
inline constexpr float kImpassable = std::numeric_limits<float>::infinity();

// Per-cell terrain + obstacle field. Indices are cell coordinates in
// [0, side()); world XZ of a cell's min corner is origin_m() + cell*cell_size_m.
struct NavSource {
    virtual ~NavSource() = default;

    virtual int side() const = 0;            // cells per axis (a power of two)
    virtual float cell_size_m() const = 0;   // world metres per cell
    virtual glm::vec2 origin_m() const = 0;  // world XZ of cell (0,0)'s min corner

    // Terrain movement-cost multiplier for the cell: >= 1 for passable ground
    // (1 = open plains, higher = slower), or kImpassable for terrain no unit
    // may cross. Independent of blocked() (which is buildings).
    virtual float cost(int cx, int cz) const = 0;

    // Representative terrain height at the cell (world metres). Feeds the
    // co-planarity merge so a slope is not collapsed into one flat leaf.
    virtual float height(int cx, int cz) const = 0;

    // Obstacle (building) occupancy. A blocked cell is impassable regardless of
    // its terrain cost.
    virtual bool blocked(int cx, int cz) const = 0;
};

// A cell is traversable iff it is neither a building nor impassable terrain.
inline bool cell_passable(const NavSource& s, int cx, int cz) {
    return !s.blocked(cx, cz) && s.cost(cx, cz) < kImpassable;
}

// Bounded-error merge tolerances + agent clearance + HPA* cluster size. The two
// epsilons ARE the accuracy guarantee: no merged leaf spans a cost range wider
// than cost_epsilon or a height range taller than height_epsilon.
struct NavParams {
    float cost_epsilon = 0.05f;    // max cost spread inside one merged leaf
    float height_epsilon = 0.25f;  // max height spread (world m) inside one leaf
    int clearance_cells = 1;       // dilate obstacles+impassable by this many cells
    int cluster_cells = 32;        // HPA* cluster side (must divide side())
};

}  // namespace badlands::nav
