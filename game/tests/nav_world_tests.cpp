// The sim<->nav bridge (game/src/nav_world): the biome cost policy, the
// SimNavSource adapter over a live world, and the stale-rebuild trigger.

#include "nav_world.h"

#include "game_state.h"
#include "heroes.h"  // biome_at
#include "navmesh/source.h"
#include "placement.h"
#include "sim_internal.hpp"  // make_world

#include <catch_amalgamated.hpp>

using namespace badlands;

TEST_CASE("biome_move_cost: water/mountain impassable, land scales from plains", "[navworld]") {
    CHECK(biome_move_cost(mapgen::Biome::Lake) == nav::kImpassable);
    CHECK(biome_move_cost(mapgen::Biome::Mountain) == nav::kImpassable);
    CHECK(biome_move_cost(mapgen::Biome::Plains) == Catch::Approx(1.0f));
    // Plains is the cheapest passable terrain.
    CHECK(biome_move_cost(mapgen::Biome::Forest) > biome_move_cost(mapgen::Biome::Plains));
    CHECK(biome_move_cost(mapgen::Biome::Swamp) > biome_move_cost(mapgen::Biome::Forest));
    CHECK(biome_move_cost(mapgen::Biome::Hills) >= biome_move_cost(mapgen::Biome::Plains));
}

TEST_CASE("SimNavSource geometry matches the placement grid", "[navworld]") {
    auto game = make_world(nullptr);
    SimNavSource src(*game);
    CHECK(src.side() == kGridSize);
    CHECK(src.cell_size_m() == Catch::Approx(1.0f));
    CHECK(src.origin_m().x == Catch::Approx(-static_cast<float>(kGridHalf)));
    CHECK(src.origin_m().y == Catch::Approx(-static_cast<float>(kGridHalf)));
}

TEST_CASE("SimNavSource cost mirrors biome_move_cost o biome_at", "[navworld]") {
    auto game = make_world(nullptr);
    SimNavSource src(*game);
    // Sample a spread of cells; the adapter must equal the biome policy at the
    // cell centre (this is the whole contract -- no map geography hardcoded).
    for (int cz = 10; cz < kGridSize; cz += 37) {
        for (int cx = 10; cx < kGridSize; cx += 37) {
            const glm::vec2 c = src.origin_m() + glm::vec2(cx + 0.5f, cz + 0.5f);
            const float expect = biome_move_cost(biome_at(*game, c));
            const float got = src.cost(cx, cz);
            if (expect == nav::kImpassable) {
                CHECK(got == nav::kImpassable);
            } else {
                CHECK(got == Catch::Approx(expect));
            }
        }
    }
}

TEST_CASE("SimNavSource marks the prebuilt castle footprint blocked", "[navworld]") {
    auto game = make_world(nullptr);
    SimNavSource src(*game);
    REQUIRE(!game->placement.buildings.empty());  // the castle

    // Every footprint triangle's tile is blocked in the adapter.
    int checked = 0;
    for (int tz = -kGridHalf; tz < kGridHalf; ++tz) {
        for (int tx = -kGridHalf; tx < kGridHalf; ++tx) {
            bool foot = false;
            for (int corner = 0; corner < 4; ++corner)
                foot |= game->placement.footprint[tri_index(tx, tz, corner)] != 0;
            if (foot) {
                CHECK(src.blocked(tx + kGridHalf, tz + kGridHalf));
                ++checked;
            }
        }
    }
    CHECK(checked > 0);  // the castle occupies some tiles
    // Out-of-world cells are blocked.
    CHECK(src.blocked(-1, 0));
    CHECK(src.blocked(kGridSize, 0));
}

TEST_CASE("rebuild_navmesh_if_stale builds once and tracks the nav epoch", "[navworld]") {
    auto game = make_world(nullptr);
    REQUIRE(game->navmesh.empty());
    rebuild_navmesh_if_stale(*game);
    CHECK_FALSE(game->navmesh.empty());
    CHECK(game->navmesh_epoch == game->placement.nav_epoch);

    // Idempotent while the epoch is unchanged.
    const uint32_t epoch = game->navmesh_epoch;
    rebuild_navmesh_if_stale(*game);
    CHECK(game->navmesh_epoch == epoch);
}

TEST_CASE("nav_cost falls back to straight-line distance with no navmesh", "[navworld]") {
    auto game = make_world(nullptr);
    REQUIRE(game->navmesh.empty());  // not built until a tick / explicit rebuild
    const glm::vec2 a{-40.0f, 50.0f}, b{-20.0f, 50.0f};
    CHECK(nav_cost(*game, a, b) == Catch::Approx(glm::distance(a, b)));
}

TEST_CASE("nav_cost with a navmesh is a weighted length >= the straight line", "[navworld]") {
    auto game = make_world(nullptr);
    rebuild_navmesh_if_stale(*game);
    REQUIRE_FALSE(game->navmesh.empty());
    // Two clear plains points west of the castle: reachable, and the terrain-cost
    // weighted length is never shorter than the geometric distance (cost >= 1).
    const glm::vec2 a{-40.0f, 50.0f}, b{-20.0f, 50.0f};
    const float c = nav_cost(*game, a, b);
    CHECK(c < nav::kImpassable);
    CHECK(c >= Catch::Approx(glm::distance(a, b)).margin(1e-3f));
    CHECK(c == Catch::Approx(game->navmesh.Cost(a, b)));  // same as the mesh query
}
