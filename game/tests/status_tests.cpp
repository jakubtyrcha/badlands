// The status subsystem (game/src/status.h): timers only -- application,
// refresh, per-tick decrement, expiry. What a status MEANS is enforced by the
// systems that read it; those gates are covered alongside the systems
// themselves (combat/movement/think).

#include "components.h"
#include "game_state.h"
#include "sim_internal.hpp"
#include "status.h"

#include <catch_amalgamated.hpp>

using badlands::advance_statuses;
using badlands::apply_status;
using badlands::has_status;
using badlands::make_flat_world;
using badlands::remaining_millis_of;
using badlands::StatusKind;
using badlands::Statuses;

TEST_CASE("apply_status stamps a timer that ticks down and expires", "[status]") {
    auto game = make_flat_world();
    const entt::entity e = game->registry.create();

    REQUIRE(apply_status(*game, e, StatusKind::Stunned, 100, 7u));
    CHECK(has_status(game->registry, e, StatusKind::Stunned));
    CHECK(remaining_millis_of(game->registry, e, StatusKind::Stunned) == 100);

    for (int i = 0; i < 3; ++i) {
        advance_statuses(*game);  // 3 * 33ms = 99ms
    }
    CHECK(has_status(game->registry, e, StatusKind::Stunned));

    advance_statuses(*game);
    CHECK_FALSE(has_status(game->registry, e, StatusKind::Stunned));
    CHECK(game->registry.get<Statuses>(e).count == 0);
}

TEST_CASE("a refresh keeps the longer remaining, never shortens", "[status]") {
    auto game = make_flat_world();
    const entt::entity e = game->registry.create();

    apply_status(*game, e, StatusKind::Stunned, 3000, 1u);
    apply_status(*game, e, StatusKind::Stunned, 500, 2u);
    CHECK(remaining_millis_of(game->registry, e, StatusKind::Stunned) == 3000);
    CHECK(game->registry.get<Statuses>(e).count == 1);  // refreshed, not appended

    apply_status(*game, e, StatusKind::Stunned, 5000, 2u);
    CHECK(remaining_millis_of(game->registry, e, StatusKind::Stunned) == 5000);
    CHECK(game->registry.get<Statuses>(e).count == 1);
}

TEST_CASE("a non-positive duration is a no-op", "[status]") {
    auto game = make_flat_world();
    const entt::entity e = game->registry.create();

    CHECK_FALSE(apply_status(*game, e, StatusKind::Stunned, 0, 1u));
    CHECK_FALSE(apply_status(*game, e, StatusKind::Stunned, -500, 1u));
    CHECK_FALSE(has_status(game->registry, e, StatusKind::Stunned));
}

TEST_CASE("querying an entity with no statuses is safe", "[status]") {
    auto game = make_flat_world();
    const entt::entity e = game->registry.create();

    CHECK_FALSE(has_status(game->registry, e, StatusKind::Stunned));
    CHECK(remaining_millis_of(game->registry, e, StatusKind::Stunned) == 0);
    advance_statuses(*game);  // no component -> nothing to sweep
    CHECK_FALSE(game->registry.all_of<Statuses>(e));
}

TEST_CASE("applying a status emits StatusApplied", "[status][events]") {
    auto game = make_flat_world();
    const entt::entity e = game->registry.create();
    game->events.clear();

    apply_status(*game, e, StatusKind::Stunned, 1000, 3u);
    REQUIRE(game->events.size() == 1);
    CHECK(game->events[0].kind == badlands::GameEventKind::StatusApplied);
    CHECK(game->events[0].actor_id == 3u);
    CHECK(game->events[0].amount ==
          Catch::Approx(static_cast<float>(StatusKind::Stunned)));
}

TEST_CASE("skill cooldowns tick down with the world", "[status][skills]") {
    auto game = make_flat_world();
    const entt::entity e = game->registry.create();
    badlands::Skills sk{};
    sk.ids[0] = badlands::SkillId::Calcify;
    sk.cooldown_remaining[0] = 1.0f;
    sk.count = 1;
    game->registry.emplace<badlands::Skills>(e, sk);

    for (int i = 0; i < 15; ++i) {
        badlands::tick_world(*game, 1.0f / 30.0f);
    }
    const float half = game->registry.get<badlands::Skills>(e).cooldown_remaining[0];
    CHECK(half == Catch::Approx(0.5f).margin(0.05f));

    for (int i = 0; i < 30; ++i) {
        badlands::tick_world(*game, 1.0f / 30.0f);
    }
    CHECK(game->registry.get<badlands::Skills>(e).cooldown_remaining[0] ==
          Catch::Approx(0.0f));
}
