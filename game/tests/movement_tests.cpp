// Movement + collision pipeline (plan/follow/melee-lock/separate). The path
// GEOMETRY is a Rust concern tested in src/nav.rs; here we inject a stub
// Pathfinder so the C++ movement systems are exercised in isolation.

#include "components.h"
#include "game_state.h"
#include "movement.h"
#include "placement.h"
#include "sim_internal.hpp"  // make_world / spawn_into / tick_world / characters_of

#include <catch_amalgamated.hpp>
#include <entt/entt.hpp>
#include <glm/glm.hpp>

#include <cmath>
#include <vector>

using namespace badlands;

namespace {

// A stub path provider. Returns `scripted` verbatim if set, else a straight
// [start, goal] line. Records call count and the exempt building it saw.
struct StubNav {
    std::vector<glm::vec2> scripted;
    int calls = 0;
    uint32_t last_exempt = UINT32_MAX;
};

extern "C" {
void mv_stub_add(void*, uint32_t, const float*, int32_t) {}
void mv_stub_remove(void*, uint32_t) {}
int32_t mv_stub_find(void* ctx, float sx, float sz, float gx, float gz, float /*radius*/,
                     uint32_t exempt, float* out, int32_t cap) {
    auto* nav = static_cast<StubNav*>(ctx);
    nav->calls++;
    nav->last_exempt = exempt;
    // The scripted detour is returned only on the first query (like routing
    // past a wall once); later requeries go straight from the current start,
    // as a real find_path always begins at the caller's position.
    std::vector<glm::vec2> wp;
    if (!nav->scripted.empty() && nav->calls == 1) {
        wp = nav->scripted;
    } else {
        wp = {{sx, sz}, {gx, gz}};
    }
    int n = static_cast<int>(wp.size());
    for (int i = 0; i < n && i < cap; ++i) {
        out[2 * i] = wp[i].x;
        out[2 * i + 1] = wp[i].y;
    }
    return n;
}
}  // extern "C"

Pathfinder stub_pathfinder(StubNav& nav) {
    return Pathfinder{&nav, mv_stub_add, mv_stub_remove, mv_stub_find};
}

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

}  // namespace

TEST_CASE("plan/follow drive a unit to a clear point") {
    auto owned = make_world(nullptr);
    BadlandsGame* game = owned.get();
    StubNav nav;
    Pathfinder pf = stub_pathfinder(nav);
    game->pathfinder = pf;

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
    CHECK(nav.calls >= 1);  // the pathfinder contract was exercised
}

TEST_CASE("follow_paths tracks a scripted multi-waypoint route without cutting the corner") {
    auto owned = make_world(nullptr);
    BadlandsGame* game = owned.get();
    StubNav nav;
    nav.scripted = {{10.0f, 10.0f}, {10.0f, 16.0f}, {16.0f, 16.0f}};  // L around a wall
    Pathfinder pf = stub_pathfinder(nav);
    game->pathfinder = pf;

    uint32_t id = spawn_unit(game, 10.0f, 10.0f, 0);
    entt::entity e = game->slots[id];
    MoveTarget& mt = game->registry.get<MoveTarget>(e);
    mt.kind = MoveTarget::Kind::Point;
    mt.point = {16.0f, 16.0f};
    mt.stop_distance = 0.1f;

    float max_x_before_turn = -1e9f;
    for (int i = 0; i < 600 && glm::distance(pos_of(game, id), {16.0f, 16.0f}) > 0.15f; ++i) {
        plan_paths(*game, 1.0f / 30.0f);
        follow_paths(*game, 1.0f / 30.0f);
        glm::vec2 p = pos_of(game, id);
        if (p.y < 15.0f) {
            max_x_before_turn = std::max(max_x_before_turn, p.x);
        }
    }
    // It walked up the x=10 leg first (never cut diagonally toward the goal).
    CHECK(max_x_before_turn < 11.0f);
    CHECK(glm::distance(pos_of(game, id), glm::vec2{16.0f, 16.0f}) < 0.3f);
}

TEST_CASE("separate_units pushes two coincident units to r_i + r_j") {
    auto owned = make_world(nullptr);
    BadlandsGame* game = owned.get();
    StubNav nav;
    Pathfinder pf = stub_pathfinder(nav);
    game->pathfinder = pf;

    uint32_t a = spawn_unit(game, 20.0f, 20.0f, 0);
    uint32_t b = spawn_unit(game, 20.0f, 20.0f, 1);

    separate_units(*game);

    float dist = glm::distance(pos_of(game, a), pos_of(game, b));
    CHECK_THAT(dist, Catch::Matchers::WithinAbs(1.0f, 1e-3f));  // 0.5 + 0.5
}

TEST_CASE("melee lock freezes the locked unit's follow movement") {
    auto owned = make_world(nullptr);
    BadlandsGame* game = owned.get();
    StubNav nav;
    Pathfinder pf = stub_pathfinder(nav);
    game->pathfinder = pf;

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
    StubNav nav;
    Pathfinder pf = stub_pathfinder(nav);
    game->pathfinder = pf;

    uint32_t id = spawn_unit(game, 10.0f, 10.0f, 0);
    entt::entity e = game->slots[id];
    MoveTarget& mt = game->registry.get<MoveTarget>(e);
    mt.kind = MoveTarget::Kind::Point;
    mt.point = {30.0f, 10.0f};
    mt.stop_distance = 0.1f;

    plan_paths(*game, 0.5f);  // first plan: queries once
    REQUIRE(nav.calls == 1);
    plan_paths(*game, 0.5f);  // still valid (same epoch, has a route): no requery
    CHECK(nav.calls == 1);

    ++game->placement.nav_epoch;  // a building was placed/destroyed elsewhere
    plan_paths(*game, 0.5f);      // epoch invalidated -> requery
    CHECK(nav.calls == 2);
}

TEST_CASE("mock duel resolves through the movement pipeline and engages melee lock") {
    auto owned = make_world(nullptr);  // mock brains
    BadlandsGame* game = owned.get();
    StubNav nav;
    Pathfinder pf = stub_pathfinder(nav);
    game->pathfinder = pf;

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
    CHECK(survivors == 1);   // the duel resolved
    CHECK(saw_lock);         // the combatants locked at some point
}
