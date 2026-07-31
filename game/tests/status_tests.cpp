// The status subsystem (game/src/status.h): timers only -- application,
// refresh, per-tick decrement, expiry. What a status MEANS is enforced by the
// systems that read it; those gates are covered alongside the systems
// themselves (combat/movement/think).

#include "combat.h"     // effective_combatant
#include "components.h"
#include "game_state.h"
#include "intention.h"  // apply_intention -- the intention a stun must abort
#include "movement.h"   // plan_paths / follow_paths -- driven directly (see Walker)
#include "sim_internal.hpp"
#include "status.h"

#include <catch_amalgamated.hpp>

#include <memory>

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

// --- what Stunned MEANS: the four enforcement points ------------------------

TEST_CASE("a stunned defender has no defense and no evasion", "[status][combat]") {
    auto game = make_flat_world();
    const entt::entity e = game->registry.create();
    game->registry.emplace<badlands::Combatant>(
        e, badlands::Combatant{.accuracy = 0.9f, .evasion = 0.4f, .defense = 0.3f,
                               .armour = 2.0f});

    const badlands::Combatant before = badlands::effective_combatant(game->registry, e);
    CHECK(before.evasion == Catch::Approx(0.4f));
    CHECK(before.defense == Catch::Approx(0.3f));

    apply_status(*game, e, StatusKind::Stunned, 1000, UINT32_MAX);
    const badlands::Combatant after = badlands::effective_combatant(game->registry, e);
    CHECK(after.evasion == Catch::Approx(0.0f));
    CHECK(after.defense == Catch::Approx(0.0f));
    CHECK(after.armour == Catch::Approx(2.0f));    // armour is worn, not used
    CHECK(after.accuracy == Catch::Approx(0.9f));  // its own attacks are not the gate
}

TEST_CASE("stunning a hero aborts its running intention", "[status][intention]") {
    auto game = make_flat_world();
    const uint32_t slot = badlands::spawn_into(*game, badlands::MercenaryDesc(0.0f, 0.0f));
    const entt::entity e = game->slots[slot];

    badlands::Intention intent;
    intent.kind = badlands::IntentionKind::MoveTo;
    intent.point = {10.0f, 0.0f};
    REQUIRE(badlands::apply_intention(*game, slot, intent));
    REQUIRE(game->registry.get<badlands::CurrentIntention>(e).kind ==
            badlands::IntentionKind::MoveTo);

    apply_status(*game, e, StatusKind::Stunned, 2000, 3u);
    CHECK(game->registry.get<badlands::CurrentIntention>(e).kind ==
          badlands::IntentionKind::None);

    const badlands::EventInbox& inbox = game->registry.get<badlands::EventInbox>(e);
    REQUIRE(inbox.count > 0);
    const badlands::InboxEvent& newest = inbox.events[inbox.count - 1];
    CHECK(newest.kind == badlands::InboxEventKind::IntentionEnded);
    CHECK(newest.param == Catch::Approx(0.0f));  // 0 = aborted, not completed
}

namespace {

// A walker with a durable point goal, driven through the movement systems
// DIRECTLY (movement_tests.cpp's idiom) rather than through tick_world: a
// brainless Town entity has its MoveTarget cleared by mock_think every tick
// (sim.cpp), which would erase the goal before follow_paths ever saw it. What
// is under test here is the movement gate, not the brain layer.
struct Walker {
    std::unique_ptr<BadlandsGame> game;
    entt::entity e = entt::null;

    explicit Walker(glm::vec2 goal) : game(make_flat_world()) {
        badlands::CharacterDesc d{};
        d.hp = 10.0f;
        d.move_speed = 3.0f;
        d.size_x = d.size_y = d.size_z = 1.0f;
        e = game->slots[badlands::spawn_into(*game, d)];
        badlands::MoveTarget mt;
        mt.kind = badlands::MoveTarget::Kind::Point;
        mt.point = goal;
        game->registry.emplace_or_replace<badlands::MoveTarget>(e, mt);
    }

    glm::vec2 pos() const { return game->registry.get<badlands::Position>(e).pos; }

    // One tick of exactly the systems a status has to gate: status expiry,
    // then plan + follow.
    void step(int ticks) {
        for (int i = 0; i < ticks; ++i) {
            advance_statuses(*game);
            badlands::plan_paths(*game, 1.0f / 30.0f);
            badlands::follow_paths(*game, 1.0f / 30.0f);
        }
    }
};

}  // namespace

TEST_CASE("a stunned character does not move", "[status][movement]") {
    auto walked = [](bool stunned) {
        Walker w({10.0f, 0.0f});
        if (stunned) {
            apply_status(*w.game, w.e, StatusKind::Stunned, 2000, UINT32_MAX);
        }
        const glm::vec2 start = w.pos();
        w.step(15);
        return glm::distance(w.pos(), start);
    };

    CHECK(walked(/*stunned=*/false) > 0.5f);
    CHECK(walked(/*stunned=*/true) == Catch::Approx(0.0f));
}

TEST_CASE("a stunned character walks again once the stun lapses", "[status][movement]") {
    Walker w({10.0f, 0.0f});
    apply_status(*w.game, w.e, StatusKind::Stunned, 500, UINT32_MAX);

    w.step(16);  // 16 * 33ms = 528ms: the stun has just lapsed
    const glm::vec2 held = w.pos();
    REQUIRE_FALSE(has_status(w.game->registry, w.e, StatusKind::Stunned));

    w.step(15);
    CHECK(glm::distance(w.pos(), held) > 0.5f);
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
