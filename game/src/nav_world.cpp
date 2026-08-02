#include "nav_world.h"

#include "game_state.h"
#include "heroes.h"     // biome_at / height_at
#include "placement.h"  // kGridHalf / kGridSize / tri_index / in_bounds_tile

namespace badlands {

// Biome -> movement-cost multiplier. Open plains is the 1.0 baseline; forest and
// swamp are progressively slower; water and mountain cannot be crossed. Fixed
// constants (no tuning knobs) per the working agreement.
float biome_move_cost(mapgen::Biome biome) {
    switch (biome) {
        case mapgen::Biome::Plains:
            return 1.0f;
        case mapgen::Biome::Hills:
            return 1.2f;
        case mapgen::Biome::Forest:
            return 1.5f;
        case mapgen::Biome::Swamp:
            return 2.5f;
        case mapgen::Biome::Lake:
        case mapgen::Biome::Mountain:
            return nav::kImpassable;
    }
    return 1.0f;
}

nav::NavParams sim_nav_params() {
    // cost/height tolerances bound the merge error; cluster size is unused until
    // HPA* (deferred).
    //
    // 0.4 m of standoff: roughly a hero's body radius (Agent::radius is half the
    // min body extent, heroes.cpp -- 0.45 for a 0.9-wide hero), so a routed path
    // very nearly fits the thing walking it. The old value was 1 whole cell,
    // about twice that.
    //
    // This is ABOVE the sqrt(2)/6 ~ 0.236 ceiling in NavParams::clearance_m, and
    // that costs something real: a diagonal wall's half-covered boundary cells
    // are within 0.4 of their own solid half, so they fill in and those walls go
    // back to cell-quantised. Measured on the sandbox arenas, the all-diagonal
    // rhomboid keeps 64 partial cells instead of 256, and its blocked area is
    // 752 m2 rather than 656 -- still well under the 1192 the per-cell boolean
    // plus 1 m Chebyshev produced, because THAT is the part the triangle mask
    // and the metric dilation fix regardless of radius.
    //
    // The trade is standoff against diagonal fidelity, and on a 1 m lattice you
    // cannot have both: a 0.4 m offset from a 45-degree line does not fall on
    // the quarter-tile lattice at all. Halving the nav cell to 0.5 m would buy
    // both at 4x the cells and ~4x the rebuild (~100 ms -> ~400 ms per
    // nav_epoch bump), which is why it is not done here.
    return nav::NavParams{/*cost_epsilon=*/0.05f, /*height_epsilon=*/0.25f,
                          /*clearance_m=*/0.4f, /*cluster_cells=*/32};
}

int SimNavSource::side() const { return kGridSize; }
float SimNavSource::cell_size_m() const { return 1.0f; }
glm::vec2 SimNavSource::origin_m() const {
    return glm::vec2(-static_cast<float>(kGridHalf));
}

glm::vec2 SimNavSource::cell_center(int cx, int cz) const {
    return origin_m() + glm::vec2(static_cast<float>(cx) + 0.5f, static_cast<float>(cz) + 0.5f);
}

float SimNavSource::cost(int cx, int cz) const {
    return biome_move_cost(biome_at(*game_, cell_center(cx, cz)));
}

float SimNavSource::height(int cx, int cz) const {
    return height_at(*game_, cell_center(cx, cz));
}

// The placement grid IS a triangle grid, and its corner numbering is the one
// nav/tri.h mirrors -- so this is a straight copy of four bits the sim already
// stamped, not a re-rasterization. (It used to OR them into one boolean, which
// is what made a diagonal wall a cell fatter than its own geometry.)
uint8_t SimNavSource::blocked_mask(int cx, int cz) const {
    const int tx = cx - kGridHalf;
    const int tz = cz - kGridHalf;
    if (!in_bounds_tile(tx, tz)) {
        return nav::kMaskSolid;  // outside the world is a wall
    }
    uint8_t mask = nav::kMaskFree;
    for (int corner = 0; corner < nav::kTriPerCell; ++corner) {
        if (game_->placement.footprint[tri_index(tx, tz, corner)]) {
            mask |= nav::tri_bit(corner);
        }
    }
    return mask;
}

void rebuild_navmesh_if_stale(BadlandsGame& game) {
    if (!game.navmesh.empty() && game.navmesh_epoch == game.placement.nav_epoch) {
        return;
    }
    SimNavSource src(game);
    game.navmesh.Build(src, sim_nav_params());
    game.navmesh_epoch = game.placement.nav_epoch;
}

float nav_cost(const BadlandsGame& game, glm::vec2 from, glm::vec2 to) {
    if (game.navmesh.empty()) {
        return glm::distance(from, to);  // no mesh: straight-line ordering
    }
    return game.navmesh.Cost(from, to);
}

bool nav_point_free(BadlandsGame& game, glm::vec2 p) {
    if (!game.terrain_blocking) {
        // The documented flat-world contract: no navmesh is built at all and
        // movement is obstacle-oblivious. Refusing every point here would make
        // a point-targeted skill silently uncastable in exactly the worlds the
        // movement tests use.
        return true;
    }
    rebuild_navmesh_if_stale(game);
    return game.navmesh.PassableAt(p);
}

void nav_cells_near(BadlandsGame& game, glm::vec2 origin, float radius, size_t max_out,
                    std::vector<nav::NavMesh::DebugCell>& out) {
    out.clear();
    if (!game.terrain_blocking) {
        return;  // nothing to show: see nav_point_free
    }
    rebuild_navmesh_if_stale(game);
    game.navmesh.CellsNear(origin, radius, max_out, out);
}

}  // namespace badlands
