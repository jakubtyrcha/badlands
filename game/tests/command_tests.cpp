// Event-sourced command layer (game/src/command.h): the single deterministic
// mutation point. These tests pin the apply/enqueue/log contract independent of
// any specific mechanic.

#include "badlands_game.h"
#include "command.h"
#include "components.h"
#include "game_state.h"

#include <catch_amalgamated.hpp>

#include <entt/entt.hpp>

using namespace badlands;

TEST_CASE("apply_command MoveTo sets the actor's MoveTarget and logs it") {
    BadlandsGame* g = game_create(nullptr);
    GameCharacterDesc d = game_desc_mercenary(0.0f, 0.0f);
    uint32_t slot = game_spawn(g, &d);

    Command cmd{CommandKind::MoveTo, slot, UINT32_MAX, {5.0f, 7.0f}, 0, 0};
    apply_command(*g, cmd);

    entt::entity e = g->slots[slot];
    const MoveTarget& mt = g->registry.get<MoveTarget>(e);
    CHECK(mt.kind == MoveTarget::Kind::Point);
    CHECK(mt.point.x == Catch::Approx(5.0f));
    CHECK(mt.point.y == Catch::Approx(7.0f));

    REQUIRE(g->command_log.size() == 1);
    CHECK(g->command_log[0].kind == CommandKind::MoveTo);
    CHECK(g->command_log[0].actor == slot);

    game_destroy(g);
}

TEST_CASE("game_dispatch applies player commands synchronously and logs them") {
    BadlandsGame* g = game_create(nullptr);

    GameAction a{GAME_ACTION_PLACE_BUILDING, 0, 24.0f, 24.0f, GAME_BUILDING_TAVERN, 0};
    int64_t id = game_dispatch(g, &a);

    CHECK(id >= 0);  // synchronous return preserved (no tick needed)
    REQUIRE(g->command_log.size() == 1);
    CHECK(g->command_log[0].kind == CommandKind::PlaceBuilding);
    CHECK(g->command_log[0].param_a == GAME_BUILDING_TAVERN);

    game_destroy(g);
}

TEST_CASE("apply_commands drains the queue in FIFO order and logs each") {
    BadlandsGame* g = game_create(nullptr);
    GameCharacterDesc d = game_desc_mercenary(0.0f, 0.0f);
    uint32_t slot = game_spawn(g, &d);

    g->command_queue.push_back({CommandKind::MoveTo, slot, UINT32_MAX, {1.0f, 0.0f}, 0, 0});
    g->command_queue.push_back({CommandKind::MoveTo, slot, UINT32_MAX, {2.0f, 0.0f}, 0, 0});
    apply_commands(*g);

    CHECK(g->command_queue.empty());
    REQUIRE(g->command_log.size() == 2);
    CHECK(g->command_log[0].point.x == Catch::Approx(1.0f));
    CHECK(g->command_log[1].point.x == Catch::Approx(2.0f));
    // last applied wins
    CHECK(g->registry.get<MoveTarget>(g->slots[slot]).point.x == Catch::Approx(2.0f));

    game_destroy(g);
}

TEST_CASE("game_tick drains queued AI commands (apply + log) during the tick") {
    BadlandsGame* g = game_create(nullptr);
    GameCharacterDesc d = game_desc_mercenary(0.0f, 0.0f);
    uint32_t slot = game_spawn(g, &d);

    g->command_queue.push_back({CommandKind::MoveTo, slot, UINT32_MAX, {3.0f, 4.0f}, 0, 0});
    size_t log_before = g->command_log.size();

    game_tick(g, 1.0f / 30.0f);

    CHECK(g->command_queue.empty());
    CHECK(g->command_log.size() > log_before);

    game_destroy(g);
}
