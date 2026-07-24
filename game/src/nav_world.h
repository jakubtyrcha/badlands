// The bridge between the sim world and the pure-CPU nav core (game/src/navmesh).
//
// SimNavSource adapts a BadlandsGame's terrain (MapData biomes + heights, via
// heroes.h's biome_at/height_at) and building footprints (placement.footprint)
// into the nav::NavSource the quadtree reads. The nav core stays biome-agnostic;
// the biome -> cost policy lives here.
//
// Coordinates line up 1:1: the placement/movement grid is kGridSize (256) tiles
// of 1 world unit = 1 m, centred on the origin, which is exactly the nav grid
// (side 256, cell 1 m, origin (-kGridHalf, -kGridHalf)).

#pragma once

#include "navmesh/navmesh.h"

#include "mapgen/biomes.hpp"

#include <glm/glm.hpp>

struct BadlandsGame;

namespace badlands {

// Terrain movement-cost multiplier for a biome. Water and mountain are
// kImpassable; the rest scale from open plains (1.0) up.
float biome_move_cost(mapgen::Biome biome);

// The nav tuning the sim builds its navmesh with (single source of truth).
nav::NavParams sim_nav_params();

// A NavSource view over a live BadlandsGame. Borrows the game (must outlive it).
class SimNavSource : public nav::NavSource {
   public:
    explicit SimNavSource(const BadlandsGame& game) : game_(&game) {}

    int side() const override;
    float cell_size_m() const override;
    glm::vec2 origin_m() const override;
    float cost(int cx, int cz) const override;
    float height(int cx, int cz) const override;
    bool blocked(int cx, int cz) const override;

   private:
    glm::vec2 cell_center(int cx, int cz) const;
    const BadlandsGame* game_;
};

// Rebuild game.navmesh from the current map + placement if it is stale (empty,
// or built against an older placement.nav_epoch). Cheap no-op when current.
void rebuild_navmesh_if_stale(BadlandsGame& game);

// Travel cost from `from` to `to` for AI goal ranking, via the navmesh (cached).
// Returns nav::kImpassable when the goal is unreachable. Falls back to
// straight-line distance when no navmesh is built (flat/obstacle-oblivious
// worlds), so a selector gets a sensible ordering either way. Const: reads the
// navmesh's mutable cost cache, never rebuilds -- the sim rebuilds before think.
float nav_cost(const BadlandsGame& game, glm::vec2 from, glm::vec2 to);

}  // namespace badlands
