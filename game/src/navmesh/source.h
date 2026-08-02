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

#include "tri.h"

#include <glm/glm.hpp>

#include <cstdint>
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

    // Obstacle (building) occupancy, as a per-corner-triangle bitmask (navmesh/
    // tri.h): bit `corner` set = that quarter of the cell is inside a footprint.
    // kMaskFree / kMaskSolid for a cell that is wholly one or the other.
    //
    // A MASK and not a bool, deliberately. Footprints are authored per triangle
    // and a diagonal building genuinely covers half a cell; the boolean this
    // replaced ORed the four bits, which made every 45-degree wall a whole cell
    // fatter than it is drawn. There is no boolean overload to fall back to --
    // a caller that wants one has to say which way it is rounding.
    virtual uint8_t blocked_mask(int cx, int cz) const = 0;
};

// Terrain alone, ignoring buildings. Terrain cost is sampled per cell (biomes
// are a tile-resolution field), so it has no sub-cell structure to lose.
inline bool cell_terrain_passable(const NavSource& s, int cx, int cz) {
    return s.cost(cx, cz) < kImpassable;
}

// A triangle is traversable iff it is neither a footprint nor impassable
// terrain. This is the atom the whole nav core is built on.
inline bool tri_passable(const NavSource& s, int cx, int cz, int corner) {
    return !mask_has(s.blocked_mask(cx, cz), corner) && cell_terrain_passable(s, cx, cz);
}

// NB there is deliberately no cell-level "is this whole cell free/solid"
// helper over the SOURCE. The quadtree's merge test has to run on the
// POST-CLEARANCE masks, not on raw source data, and a helper that looks like it
// answers the same question would disagree with the quadtree for every cell in
// an obstacle's standoff ring.

// Bounded-error merge tolerances + agent clearance + HPA* cluster size. The two
// epsilons ARE the accuracy guarantee: no merged leaf spans a cost range wider
// than cost_epsilon or a height range taller than height_epsilon.
struct NavParams {
    float cost_epsilon = 0.05f;    // max cost spread inside one merged leaf
    float height_epsilon = 0.25f;  // max height spread (world m) inside one leaf
    // Agent standoff: dilate obstacles + impassable terrain by this many world
    // METRES. Metres and not cells because the useful values are sub-cell -- on
    // the 1 m grid this was an int, so the only radii expressible were 0 and a
    // full metre, and the agent radius is neither.
    //
    // The dilation runs over triangles (a triangle is blocked when its centroid
    // comes within this of a solid one), so it is QUANTISED to the triangle
    // lattice rather than honoured continuously. Three distances decide
    // everything, all measured from a centroid:
    //
    //   1/6      ~ 0.167   to its own cell's edge  -> the triangle ACROSS the border
    //   sqrt2/6  ~ 0.236   to its own cell's diagonals -> the two IN-TILE corners
    //   1/3      ~ 0.333   to the opposite corner of its own cell
    //
    // which carves out three regimes:
    //
    //   <= 0.167          no standoff at all; paths graze the exact footprint.
    //   0.167 .. 0.236    one triangle across the border. THE USEFUL BAND.
    //   >= 0.236          a partial cell's free half is inside its own solid
    //                     half's radius, so EVERY half-covered cell fills in and
    //                     collapses to solid -- which silently throws away the
    //                     entire triangle mask and puts the fat diagonal walls
    //                     straight back. game/tests/navmesh_tests.cpp pins this.
    //
    // Metres and not cells because that band is sub-cell throughout: on the old
    // 1 m integer grid the only radii expressible were 0 and a full metre, and
    // a metre is ~2x a hero.
    float clearance_m = 0.2f;
    int cluster_cells = 32;  // HPA* cluster side (must divide side())
};

}  // namespace badlands::nav
