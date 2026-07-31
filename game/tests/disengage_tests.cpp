// Melee contact, and the price of leaving it.
//
// MeleeLock used to freeze movement outright, which made disengaging
// impossible rather than expensive: a locked unit could not move, so the
// distance never grew, so the lock never released. It now forbids only ranged
// attacks. Walking out is possible -- and costs StatusKind::Disengaged, a few
// seconds of being able to do nothing at all.

#include "badlands_sim.hpp"
#include "brain_abi.h"  // BL_ACT_ATTACK
#include "combat.h"
#include "components.h"
#include "game_state.h"
#include "intention.h"
#include "movement.h"
#include "sim_internal.hpp"
#include "status.h"

#include <catch_amalgamated.hpp>

using namespace badlands;

namespace {

constexpr float kDt = 1.0f / 30.0f;

// Two melee fighters in contact. Neither has a brain (plain descs), so nothing
// re-decides underneath the case -- movement is driven directly.
struct Contact {
    std::unique_ptr<BadlandsGame> owned = make_flat_world();
    uint32_t a = 0, b = 0;
    entt::entity ae = entt::null, be = entt::null;

    Contact() {
        BadlandsGame& g = *owned;
        a = spawn_into(g, fighter(0, 0.0f));
        b = spawn_into(g, fighter(1, 1.0f));
        ae = g.slots[a];
        be = g.slots[b];
        // Lock them: one movement pass with nobody moving is enough, since
        // update_melee_locks keys off distance alone.
        step();
        REQUIRE(g.registry.all_of<MeleeLock>(ae));
        REQUIRE(g.registry.all_of<MeleeLock>(be));
    }

    static CharacterDesc fighter(int32_t team, float x) {
        CharacterDesc d{};
        d.team = team;
        d.hp = 100.0f;
        d.move_speed = 3.0f;
        d.pos_x = x;
        d.size_x = d.size_y = d.size_z = 1.0f;
        d.accuracy = 1.0f;
        d.attack_count = 1;
        // Reach 1.5 -> they are in contact at 1.0 apart. No wind-up: this file
        // is about contact, not commitment.
        d.attacks[0] = Attack{AttackCategory::Melee, DamageType::Slashing, 1.0f, 1.5f, 1.0f, 0.0f};
        return d;
    }

    BadlandsGame& game() { return *owned; }

    // The movement pipeline only -- no think pass, which would clear the
    // MoveTargets these cases set (mock_think, sim.cpp).
    void step(int n = 1) {
        for (int i = 0; i < n; ++i) {
            owned->world_millis += kMillisPerTick;
            advance_statuses(*owned);
            plan_paths(*owned, kDt);
            follow_paths(*owned, kDt);
            update_melee_locks(*owned);
            separate_units(*owned);
        }
    }

    void walk_away(entt::entity e, float to_x) {
        owned->registry.get<MoveTarget>(e) = MoveTarget{MoveTarget::Kind::Point, {to_x, 0.0f}};
    }
};

}  // namespace

TEST_CASE("a melee-locked unit can move again", "[disengage]") {
    Contact c;
    const glm::vec2 start = c.game().registry.get<Position>(c.ae).pos;
    c.walk_away(c.ae, -20.0f);
    c.step(20);
    CHECK(glm::distance(c.game().registry.get<Position>(c.ae).pos, start) > 1.0f);
}

TEST_CASE("walking out of contact earns the penalty", "[disengage]") {
    Contact c;
    c.walk_away(c.ae, -20.0f);
    c.step(30);

    CHECK_FALSE(c.game().registry.all_of<MeleeLock>(c.ae));
    CHECK(has_status(c.game().registry, c.ae, StatusKind::Disengaged));
    CHECK(remaining_millis_of(c.game().registry, c.ae, StatusKind::Disengaged) > 0);
}

TEST_CASE("being left behind does not", "[disengage]") {
    // B walks off; A stood still. A's contact broke through no act of its own,
    // so A pays nothing -- and B, which did the walking, pays.
    Contact c;
    c.walk_away(c.be, 20.0f);
    c.step(30);

    CHECK_FALSE(has_status(c.game().registry, c.ae, StatusKind::Disengaged));
    CHECK(has_status(c.game().registry, c.be, StatusKind::Disengaged));
}

TEST_CASE("losing an opponent to death does not", "[disengage]") {
    Contact c;
    c.game().registry.get<Health>(c.be).hp = 0.0f;
    c.game().registry.destroy(c.be);
    c.game().slots[c.b] = entt::null;
    c.step(5);

    CHECK_FALSE(c.game().registry.all_of<MeleeLock>(c.ae));
    CHECK_FALSE(has_status(c.game().registry, c.ae, StatusKind::Disengaged));
}

TEST_CASE("a separation nudge does not", "[disengage]") {
    // Overlap two locked, stationary units so separate_units pushes them
    // apart. That is not path-following, so it never sets the moved flag and
    // never charges the penalty -- otherwise a crowded scrum would disable
    // everyone in it.
    Contact c;
    c.game().registry.get<Position>(c.be).pos = glm::vec2{0.05f, 0.0f};
    c.step(30);

    CHECK_FALSE(has_status(c.game().registry, c.ae, StatusKind::Disengaged));
    CHECK_FALSE(has_status(c.game().registry, c.be, StatusKind::Disengaged));
}

TEST_CASE("Disengaged refuses every action", "[disengage]") {
    Contact c;
    BadlandsGame& g = c.game();
    REQUIRE(apply_status(g, c.ae, StatusKind::Disengaged, 3000, c.a));

    CHECK_FALSE(resolve_action(g, c.a, AgentAction{BL_ACT_ATTACK, c.b, 0}));
    CHECK(g.command_queue.empty());

    // ...and authoritatively, not only at the gateway: a command that reached
    // the queue some other way still lands on nothing.
    const float hp0 = g.registry.get<Health>(c.be).hp;
    g.command_queue.push_back({CommandKind::Attack, c.a, c.b, {0.0f, 0.0f}, /*param_a=*/0});
    apply_commands(g);
    CHECK(g.registry.get<Health>(c.be).hp == hp0);
}

TEST_CASE("Disengaged stops acting, not moving or defending", "[disengage]") {
    Contact c;
    BadlandsGame& g = c.game();
    apply_status(g, c.ae, StatusKind::Disengaged, 3000, c.a);

    // Still dodges and parries: this is a penalty on ACTING, unlike Stunned.
    const Combatant& base = g.registry.get<Combatant>(c.ae);
    const Combatant eff = effective_combatant(g.registry, c.ae);
    CHECK(eff.evasion == Catch::Approx(base.evasion));
    CHECK(eff.defense == Catch::Approx(base.defense));

    // And still runs -- being able to leave is the whole point of paying.
    const glm::vec2 start = g.registry.get<Position>(c.ae).pos;
    c.walk_away(c.ae, -20.0f);
    c.step(20);
    CHECK(glm::distance(g.registry.get<Position>(c.ae).pos, start) > 1.0f);
}

TEST_CASE("the penalty expires", "[disengage]") {
    Contact c;
    apply_status(c.game(), c.ae, StatusKind::Disengaged, 500, c.a);
    c.step(16);  // 528 ms
    CHECK_FALSE(has_status(c.game().registry, c.ae, StatusKind::Disengaged));
    CHECK(resolve_action(c.game(), c.a, AgentAction{BL_ACT_ATTACK, c.b, 0}));
}
