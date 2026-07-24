// Movement + collision pipeline (plan/follow/melee-lock/separate). The path
// GEOMETRY is tested in game/tests/navmesh_tests.cpp; here the C++ movement
// systems are exercised against the game's own navmesh (real, or a synthetic
// NavSource injected straight into game.navmesh for the routing cases).

#include "components.h"
#include "game_state.h"
#include "movement.h"
#include "nav_world.h"  // sim_nav_params / rebuild_navmesh_if_stale
#include "navmesh/source.h"
#include "placement.h"
#include "sim_internal.hpp"  // make_world / spawn_into / tick_world / characters_of

#include <catch_amalgamated.hpp>
#include <entt/entt.hpp>
#include <glm/glm.hpp>

#include <array>
#include <cmath>
#include <vector>

using namespace badlands;

namespace {

CharacterDesc unit(float x, float z, int32_t team, float range = 1.0f) {
    CharacterDesc d{};
    d.pos_x = x;
    d.pos_z = z;
    d.team = team;
    d.hp = 10.0f;
    d.move_speed = 3.0f;
    d.attack_range = range;
    d.attack_damage = 1.0f;
    d.attack_cooldown = 1.0f;
    d.size_x = d.size_y = d.size_z = 1.0f;  // radius 0.5
    return d;
}

uint32_t spawn_unit(BadlandsGame* game, float x, float z, int32_t team, float range = 1.0f) {
    CharacterDesc d = unit(x, z, team, range);
    return spawn_into(*game, d);
}

glm::vec2 pos_of(BadlandsGame* game, uint32_t id) {
    return game->registry.get<Position>(game->slots[id]).pos;
}

// A synthetic full-map NavSource matching the sim's grid (256 cells, 1 m, origin
// -128): all passable at cost 1 except caller-marked wall cells. Lets a movement
// test stage an obstacle without depending on where the map puts biomes.
struct WallSource : nav::NavSource {
    std::vector<char> blk = std::vector<char>(kGridSize * kGridSize, 0);
    int side() const override { return kGridSize; }
    float cell_size_m() const override { return 1.0f; }
    glm::vec2 origin_m() const override { return glm::vec2(-static_cast<float>(kGridHalf)); }
    float cost(int, int) const override { return 1.0f; }
    float height(int, int) const override { return 0.0f; }
    bool blocked(int x, int z) const override { return blk[z * kGridSize + x] != 0; }
    void wall(int x, int z) { blk[z * kGridSize + x] = 1; }
};

}  // namespace

TEST_CASE("plan/follow drive a unit to a clear point") {
    auto owned = make_flat_world();
    BadlandsGame* game = owned.get();

    uint32_t id = spawn_unit(game, 10.0f, 10.0f, 0);
    entt::entity e = game->slots[id];
    MoveTarget& mt = game->registry.get<MoveTarget>(e);
    mt.kind = MoveTarget::Kind::Point;
    mt.point = {15.0f, 10.0f};
    mt.stop_distance = 0.1f;

    for (int i = 0; i < 400 && glm::distance(pos_of(game, id), {15.0f, 10.0f}) > 0.15f; ++i) {
        plan_paths(*game, 1.0f / 30.0f);
        follow_paths(*game, 1.0f / 30.0f);
    }
    CHECK(glm::distance(pos_of(game, id), glm::vec2{15.0f, 10.0f}) < 0.2f);
}

TEST_CASE("follow_paths tracks a multi-waypoint route without cutting the corner") {
    auto owned = make_flat_world();
    BadlandsGame* game = owned.get();
    uint32_t id = spawn_unit(game, 10.0f, 10.0f, 0);
    entt::entity e = game->slots[id];

    // Hand the unit an explicit L-route; follow_paths must walk the legs in order
    // (MoveTarget stays None, so plan_paths never rewrites the path).
    NavPath& np = game->registry.get<NavPath>(e);
    np.waypoints = {{10.0f, 10.0f}, {10.0f, 16.0f}, {16.0f, 16.0f}};
    np.cursor = 0;

    float max_x_before_turn = -1e9f;
    for (int i = 0; i < 600 && glm::distance(pos_of(game, id), {16.0f, 16.0f}) > 0.15f; ++i) {
        follow_paths(*game, 1.0f / 30.0f);
        glm::vec2 p = pos_of(game, id);
        if (p.y < 15.0f) {
            max_x_before_turn = std::max(max_x_before_turn, p.x);
        }
    }
    CHECK(max_x_before_turn < 11.0f);  // walked the x=10 leg first
    CHECK(glm::distance(pos_of(game, id), glm::vec2{16.0f, 16.0f}) < 0.3f);
}

TEST_CASE("plan_paths routes a unit around a navmesh obstacle") {
    auto owned = make_flat_world();
    BadlandsGame* game = owned.get();
    // A wall on world x=0 (cell 128) spanning z in [-2, 72], leaving gaps at both
    // ends. The straight shot from (-10,0) to (10,0) crosses it.
    WallSource src;
    for (int cz = 126; cz <= 200; ++cz) src.wall(kGridHalf, cz);
    game->navmesh.Build(src, sim_nav_params());

    uint32_t id = spawn_unit(game, -10.0f, 0.0f, 0);
    entt::entity e = game->slots[id];
    MoveTarget& mt = game->registry.get<MoveTarget>(e);
    mt.kind = MoveTarget::Kind::Point;
    mt.point = {10.0f, 0.0f};
    mt.stop_distance = 0.1f;

    plan_paths(*game, 1.0f);
    NavPath& np = game->registry.get<NavPath>(e);
    REQUIRE(!np.waypoints.empty());
    CHECK(np.waypoints.size() >= 3);  // detoured, not a straight shot
    for (const glm::vec2& w : np.waypoints) {
        const glm::ivec2 c = game->navmesh.WorldToCell(w);
        CHECK_FALSE(src.blocked(c.x, c.y));  // no waypoint on the wall
    }
}

TEST_CASE("plan_paths raises MoveBlocked for an unreachable goal") {
    auto owned = make_flat_world();
    BadlandsGame* game = owned.get();
    // A full wall on cell x=128 severs the map; (10,0) is unreachable from (-10,0).
    WallSource src;
    for (int cz = 0; cz < kGridSize; ++cz) src.wall(kGridHalf, cz);
    game->navmesh.Build(src, sim_nav_params());

    uint32_t id = spawn_unit(game, -10.0f, 0.0f, 0);
    entt::entity e = game->slots[id];
    MoveTarget& mt = game->registry.get<MoveTarget>(e);
    mt.kind = MoveTarget::Kind::Point;
    mt.point = {10.0f, 0.0f};
    mt.stop_distance = 0.1f;
    REQUIRE_FALSE(game->registry.all_of<MoveBlocked>(e));

    plan_paths(*game, 1.0f);
    CHECK(game->registry.all_of<MoveBlocked>(e));
    CHECK(game->registry.get<NavPath>(e).waypoints.empty());
}

TEST_CASE("separate_units pushes two coincident units to r_i + r_j") {
    auto owned = make_world(nullptr);
    BadlandsGame* game = owned.get();

    uint32_t a = spawn_unit(game, 20.0f, 20.0f, 0);
    uint32_t b = spawn_unit(game, 20.0f, 20.0f, 1);

    separate_units(*game);

    float dist = glm::distance(pos_of(game, a), pos_of(game, b));
    CHECK_THAT(dist, Catch::Matchers::WithinAbs(1.0f, 1e-3f));  // 0.5 + 0.5
}

TEST_CASE("melee lock freezes the locked unit's follow movement") {
    auto owned = make_world(nullptr);
    BadlandsGame* game = owned.get();

    // Two enemies 0.5 apart (well within attack_range 1.0) far from the castle.
    uint32_t a = spawn_unit(game, -20.0f, -20.0f, 0);
    spawn_unit(game, -19.5f, -20.0f, 1);
    entt::entity ea = game->slots[a];

    // Give unit a a distant goal it would otherwise walk toward.
    MoveTarget& mt = game->registry.get<MoveTarget>(ea);
    mt.kind = MoveTarget::Kind::Point;
    mt.point = {20.0f, -20.0f};
    mt.stop_distance = 0.1f;

    update_melee_locks(*game);
    REQUIRE(game->registry.all_of<MeleeLock>(ea));

    glm::vec2 before = pos_of(game, a);
    plan_paths(*game, 1.0f / 30.0f);
    follow_paths(*game, 1.0f / 30.0f);
    CHECK(glm::distance(pos_of(game, a), before) < 1e-5f);  // frozen
}

TEST_CASE("plan_paths repaths when nav_epoch bumps") {
    auto owned = make_world(nullptr);
    BadlandsGame* game = owned.get();
    rebuild_navmesh_if_stale(*game);  // real navmesh from the map + castle

    // Plains west of the castle (both endpoints passable, route unobstructed).
    uint32_t id = spawn_unit(game, -40.0f, 50.0f, 0);
    entt::entity e = game->slots[id];
    MoveTarget& mt = game->registry.get<MoveTarget>(e);
    mt.kind = MoveTarget::Kind::Point;
    mt.point = {-20.0f, 50.0f};
    mt.stop_distance = 0.1f;

    plan_paths(*game, 0.5f);
    NavPath& np = game->registry.get<NavPath>(e);
    REQUIRE(!np.waypoints.empty());
    CHECK(np.epoch == game->placement.nav_epoch);

    ++game->placement.nav_epoch;  // a building was placed/destroyed elsewhere
    plan_paths(*game, 0.5f);      // cooldown elapsed + epoch invalidated -> repath
    CHECK(np.epoch == game->placement.nav_epoch);
}

TEST_CASE("mock duel resolves through the movement pipeline and engages melee lock") {
    auto owned = make_flat_world();  // mock brains
    BadlandsGame* game = owned.get();

    CharacterDesc merc = MercenaryDesc(-6.0f, -12.0f);
    CharacterDesc gob = GoblinDesc(6.0f, -12.0f);
    spawn_into(*game, merc);
    spawn_into(*game, gob);

    bool saw_lock = false;
    int survivors = 2;
    for (int i = 0; i < 3000; ++i) {
        tick_world(*game, 1.0f / 30.0f);
        if (game->registry.view<MeleeLock>().begin() != game->registry.view<MeleeLock>().end()) {
            saw_lock = true;
        }
        survivors = static_cast<int>(characters_of(*game).size());
        if (survivors <= 1) {
            break;
        }
    }
    CHECK(survivors == 1);  // the duel resolved
    CHECK(saw_lock);        // the combatants locked at some point
}

// --- code-review guards (navmesh migration) ---------------------------------

TEST_CASE("a unit reaches a building's approach tile via the navmesh") {
    // Guards the refuted "units stop one cell short of doors" finding:
    // building_approach_tile is 2 cells out (past the footprint margin), so it is
    // reachable through the 1-cell navmesh dilation.
    auto owned = make_world(nullptr);
    BadlandsGame* game = owned.get();
    const uint32_t house = place_building(
        *game, PlacementDesc{static_cast<int32_t>(BuildingKind::House), 0, -40.0f, 50.0f}, false);
    REQUIRE(house != std::numeric_limits<uint32_t>::max());
    rebuild_navmesh_if_stale(*game);

    glm::vec2 tile;
    REQUIRE(building_approach_tile(game->placement, game->placement.buildings[house], tile));

    const uint32_t id = spawn_unit(game, -52.0f, 50.0f, 0);  // ~west of the house
    entt::entity e = game->slots[id];
    MoveTarget& mt = game->registry.get<MoveTarget>(e);
    mt.kind = MoveTarget::Kind::Point;
    mt.point = tile;
    mt.stop_distance = 0.1f;

    for (int i = 0; i < 600 && glm::distance(pos_of(game, id), tile) > kEntranceRadius; ++i) {
        plan_paths(*game, 1.0f / 30.0f);
        follow_paths(*game, 1.0f / 30.0f);
        separate_units(*game);
    }
    CHECK(glm::distance(pos_of(game, id), tile) <= kEntranceRadius);
}

TEST_CASE("footprint reprojection engages only once a navmesh exists") {
    // The reproject guard (F9) is the SAME signal as query_path: navmesh presence.
    auto owned = make_world(nullptr);
    BadlandsGame* game = owned.get();
    const uint32_t b = place_building(
        *game, PlacementDesc{static_cast<int32_t>(BuildingKind::House), 0, -40.0f, 50.0f}, false);
    REQUIRE(b != std::numeric_limits<uint32_t>::max());
    const glm::vec2 center = game->placement.buildings[b].center;

    const uint32_t id = spawn_unit(game, center.x, center.y, 0);  // inside the footprint

    // No navmesh yet -> reproject skipped -> the lone unit stays put.
    REQUIRE(game->navmesh.empty());
    separate_units(*game);
    CHECK(glm::distance(pos_of(game, id), center) < 1e-3f);

    // Navmesh present -> reproject engages -> the unit is pushed out of the footprint.
    rebuild_navmesh_if_stale(*game);
    REQUIRE_FALSE(game->navmesh.empty());
    separate_units(*game);
    CHECK(glm::distance(pos_of(game, id), center) > 1e-3f);
}
