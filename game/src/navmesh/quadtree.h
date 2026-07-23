// Adaptive quadtree over a NavSource: the base navmesh cell decomposition.
//
// A quad is kept whole when its cells are uniform enough (all passable with cost
// + height within NavParams tolerances, or all impassable); otherwise it splits
// into four. Flat, single-biome ground collapses into a few big leaves while
// building edges and the lake shore stay at 1 cell -- the reduction that makes
// A* / HPA* over the leaves cheap.
//
// Deterministic: leaves are emitted in a fixed NW,NE,SW,SE pre-order, so the
// same (source, params) always yields byte-identical leaves -- required by the
// sim's replay/determinism contract once brains query nav.

#pragma once

#include "source.h"

#include <cstdint>
#include <vector>

namespace badlands::nav {

// One merged navmesh cell: the square block [x0, x0+size) x [z0, z0+size) in
// CELL coordinates. Impassable leaves are retained (so the debug view can draw
// obstacles); the graph layer skips them.
struct Leaf {
    int x0 = 0, z0 = 0;      // min corner, in cells
    int size = 0;            // side, in cells (a power of two)
    float cost = 0.0f;       // representative terrain cost; kImpassable if blocked
    bool passable = false;
};

class Quadtree {
   public:
    // Build the decomposition. `src.side()` must be a power of two. Obstacles
    // and impassable terrain are first dilated by params.clearance_cells.
    void Build(const NavSource& src, const NavParams& params);

    const std::vector<Leaf>& leaves() const { return leaves_; }
    int side() const { return side_; }

    // Index into leaves() of the leaf covering cell (cx, cz), or -1 if out of
    // range. O(1): backed by a per-cell lookup filled during Build.
    int LeafAt(int cx, int cz) const;

   private:
    std::vector<Leaf> leaves_;
    std::vector<int32_t> cell_leaf_;  // side_*side_, leaf index per cell
    int side_ = 0;
};

}  // namespace badlands::nav
