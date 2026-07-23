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
    // cost/height tolerances bound the merge error; clearance 1 cell ~ agent
    // radius; cluster size is unused until HPA* (deferred).
    return nav::NavParams{/*cost_epsilon=*/0.05f, /*height_epsilon=*/0.25f,
                          /*clearance_cells=*/1, /*cluster_cells=*/32};
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

bool SimNavSource::blocked(int cx, int cz) const {
    const int tx = cx - kGridHalf;
    const int tz = cz - kGridHalf;
    if (!in_bounds_tile(tx, tz)) {
        return true;  // outside the world is a wall
    }
    for (int corner = 0; corner < 4; ++corner) {
        if (game_->placement.footprint[tri_index(tx, tz, corner)]) {
            return true;  // a building tile
        }
    }
    return false;
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

}  // namespace badlands
