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
    auto game = make_world(BrainDesc{});
    SimNavSource src(*game);
    CHECK(src.side() == kGridSize);
    CHECK(src.cell_size_m() == Catch::Approx(1.0f));
    CHECK(src.origin_m().x == Catch::Approx(-static_cast<float>(kGridHalf)));
    CHECK(src.origin_m().y == Catch::Approx(-static_cast<float>(kGridHalf)));
}

TEST_CASE("SimNavSource cost mirrors biome_move_cost o biome_at", "[navworld]") {
    auto game = make_world(BrainDesc{});
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
    auto game = make_world(BrainDesc{});
    SimNavSource src(*game);
    REQUIRE(!game->placement.buildings.empty());  // the castle

    // The adapter carries the footprint BIT FOR BIT, not "any corner set". A
    // mismatch here is the whole defect this layer had: OR-ing the four bits
    // turned every half-covered cell solid, which fattened each diagonal wall
    // by a metre it does not occupy.
    int checked = 0;
    for (int tz = -kGridHalf; tz < kGridHalf; ++tz) {
        for (int tx = -kGridHalf; tx < kGridHalf; ++tx) {
            uint8_t want = nav::kMaskFree;
            for (int corner = 0; corner < nav::kTriPerCell; ++corner)
                if (game->placement.footprint[tri_index(tx, tz, corner)] != 0)
                    want |= nav::tri_bit(corner);
            if (want != nav::kMaskFree) {
                CHECK(src.blocked_mask(tx + kGridHalf, tz + kGridHalf) == want);
                ++checked;
            }
        }
    }
    CHECK(checked > 0);  // the castle occupies some tiles
    // Out-of-world cells are wholly solid.
    CHECK(src.blocked_mask(-1, 0) == nav::kMaskSolid);
    CHECK(src.blocked_mask(kGridSize, 0) == nav::kMaskSolid);
}

TEST_CASE("rebuild_navmesh_if_stale builds once and tracks the nav epoch", "[navworld]") {
    auto game = make_world(BrainDesc{});
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
    auto game = make_world(BrainDesc{});
    REQUIRE(game->navmesh.empty());  // not built until a tick / explicit rebuild
    const glm::vec2 a{-40.0f, 50.0f}, b{-20.0f, 50.0f};
    CHECK(nav_cost(*game, a, b) == Catch::Approx(glm::distance(a, b)));
}

TEST_CASE("nav_cost with a navmesh is a weighted length >= the straight line", "[navworld]") {
    auto game = make_world(BrainDesc{});
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

TEST_CASE("nav clearance is sized to the body that walks it", "[navworld]") {
    // Clearance is a standoff the PATH keeps from a footprint, bought by
    // declaring ground unwalkable. Two numbers bracket what is sensible:
    //
    //   1/6 ~ 0.167   below this the lattice cannot express any standoff at all
    //                 (NavParams::clearance_m), so a path grazes the footprint
    //   ~0.45         Agent::radius for a hero (heroes.cpp: half the min body
    //                 extent of a 0.9-wide body) -- the point at which the body
    //                 fits wherever its centre is routed
    //
    // The old value was 1 whole cell: expressible only because it was an
    // integer count, and about twice a hero. Combined with a per-cell obstacle
    // boolean that already rounded diagonal walls outward, that is what made
    // the arenas play far smaller than they draw.
    //
    // NB the mesh still does not GUARANTEE a body fits -- 0.4 is just under a
    // hero and well under the 0.8 of the widest creature. What is guaranteed is
    // that the routed CENTRE stays off the footprint, enforced exactly by
    // reproject_out_of_footprints (movement.cpp) against the true oriented
    // polygon.
    const nav::NavParams p = sim_nav_params();
    CHECK(p.clearance_m > 1.0f / 6.0f);
    CHECK(p.clearance_m <= 0.45f);
}

// --- the triangle lattice, across the two files that describe it ------------

TEST_CASE("nav's corner convention matches the placement grid's", "[navworld]") {
    // navmesh/tri.h re-declares the quarter-tile convention that placement.cpp
    // stamps footprints with, because the nav core may not include placement.h
    // (the injected-source boundary is what makes it testable without a sim).
    // Two copies of a convention drift; this is what stops them. A mismatch
    // would not crash -- units would just clip the wrong half of every
    // diagonal wall, which is far harder to notice.
    for (int corner = 0; corner < nav::kTriPerCell; ++corner) {
        CAPTURE(corner);
        for (int tz = -2; tz <= 2; ++tz) {
            for (int tx = -2; tx <= 2; ++tx) {
                // placement's representative point, read back through nav's
                // point -> corner test.
                const glm::vec2 p = triangle_centroid(tx, tz, corner);
                CHECK(nav::corner_at(p.x - static_cast<float>(tx), p.y - static_cast<float>(tz)) ==
                      corner);
                // ...and nav's own centroid must BE that point.
                const glm::vec2 q = nav::tri_centroid_cells(tx, tz, corner);
                CHECK(q.x == Catch::Approx(p.x));
                CHECK(q.y == Catch::Approx(p.y));
            }
        }
    }
}

TEST_CASE("a diagonal building leaves half its boundary cells walkable", "[navworld]") {
    // The defect in one test. A rotation-1 wall snaps to the (u,v) lattice, so
    // its edge runs through cells rather than between them. Collapsing those
    // cells to solid -- what the old per-cell boolean did -- is a metre of
    // phantom wall down both faces of every diagonal run in the sandbox.
    WorldConfig cfg;
    cfg.prebuild_colony = false;
    cfg.map = MapKind::FlatPlains;
    cfg.terrain_blocking = true;
    auto game = make_world(BrainDesc{}, cfg);
    REQUIRE(plop_building(*game, PlacementDesc{static_cast<int32_t>(BuildingKind::Wall), 1,
                                               0.0f, 0.0f}) != UINT32_MAX);

    SimNavSource src(*game);
    int partial = 0, solid = 0;
    for (int cz = 0; cz < kGridSize; ++cz) {
        for (int cx = 0; cx < kGridSize; ++cx) {
            const uint8_t m = src.blocked_mask(cx, cz);
            if (m == nav::kMaskFree) {
                continue;
            }
            (m == nav::kMaskSolid) ? ++solid : ++partial;
        }
    }
    // A diagonal footprint MUST produce half-covered cells; if it does not, the
    // adapter has rounded them off and the rest of this is moot.
    CHECK(partial > 0);
    CHECK(solid > 0);  // and a solid interior

    // Whether those free halves survive to the MESH is a question about the
    // standoff, not about the mask -- so ask it at both sides of the ceiling.
    // This is what keeps the shipped configuration honest: the half-covered
    // cells are genuinely there, and it is the radius, not a rounding bug, that
    // decides whether anything may stand on them.
    auto any_free_half_standable = [&](float clearance_m) {
        nav::NavMesh mesh;
        mesh.Build(src, nav::NavParams{0.05f, 0.25f, clearance_m, 32});
        for (int cz = 0; cz < kGridSize; ++cz) {
            for (int cx = 0; cx < kGridSize; ++cx) {
                const uint8_t m = src.blocked_mask(cx, cz);
                if (m == nav::kMaskFree || m == nav::kMaskSolid) {
                    continue;
                }
                for (int c = 0; c < nav::kTriPerCell; ++c) {
                    if (nav::mask_has(m, c)) {
                        continue;
                    }
                    const glm::vec2 w =
                        src.origin_m() + nav::tri_centroid_cells(cx, cz, c) * src.cell_size_m();
                    if (mesh.PassableAt(w)) {
                        return true;
                    }
                }
            }
        }
        return false;
    };
    // Under the ceiling the free half is walkable -- proof the mask reaches the
    // mesh intact, which is the whole of part B.
    CHECK(any_free_half_standable(0.2f));
    // At the shipped 0.4 it is not: a free corner is 0.236 from its own cell's
    // solid corners, so the standoff absorbs it and the diagonal face goes back
    // to cell-quantised. Deliberate (sim_nav_params has the reasoning) -- but
    // asserted, so it cannot become true again by accident.
    CHECK_FALSE(any_free_half_standable(sim_nav_params().clearance_m));
}

