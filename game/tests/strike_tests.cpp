// Attack commitment (game/src/strike.h): an attack takes time to throw, the
// wind-up is cancellable and the recovery is not.

#include "strike.h"

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

// Two adjacent fighters with the gates pinned so the outcome is certain: this
// file tests WHEN a blow lands, never whether a roll went its way.
struct Bout {
    std::unique_ptr<BadlandsGame> owned = make_flat_world();
    uint32_t attacker = 0, victim = 0;
    entt::entity ae = entt::null, ve = entt::null;

    // wind_up / recovery in seconds; cooldown fixed at 1s.
    Bout(float wind_up, float recovery, AttackCategory cat = AttackCategory::Melee) {
        BadlandsGame& g = *owned;
        CharacterDesc a{};
        a.team = 0;
        a.hp = 100.0f;
        a.move_speed = 3.0f;  // so "does it move?" cases have something to observe
        a.size_x = a.size_y = a.size_z = 1.0f;
        a.accuracy = 1.0f;
        a.attack_count = 1;
        a.attacks[0] = Attack{cat, DamageType::Slashing, 5.0f, 6.0f, 1.0f, 0.0f,
                              wind_up, recovery};
        attacker = spawn_into(g, a);

        CharacterDesc v{};
        v.team = 1;
        v.hp = 100.0f;
        v.size_x = v.size_y = v.size_z = 1.0f;
        v.pos_x = 1.0f;
        victim = spawn_into(g, v);  // no attacks: harmless, so only one side acts

        ae = g.slots[attacker];
        ve = g.slots[victim];
    }

    BadlandsGame& game() { return *owned; }
    float victim_hp() { return owned->registry.get<Health>(ve).hp; }
    void tick(int n = 1) {
        for (int i = 0; i < n; ++i) {
            tick_world(*owned, kDt);
        }
    }
    void declare() { fire_attack(*owned, attacker, victim, 0); }

    // Advance WITHOUT the think pass. mock_think (sim.cpp) clears a brainless
    // entity's MoveTarget every tick, so any case about a durable route has to
    // drive the pipeline directly -- the same reason movement_tests does.
    void step_no_think(int n = 1) {
        for (int i = 0; i < n; ++i) {
            owned->world_millis += kMillisPerTick;
            advance_statuses(*owned);
            advance_strikes(*owned);
            plan_paths(*owned, kDt);
            follow_paths(*owned, kDt);
        }
    }
};

}  // namespace

TEST_CASE("a swing resolves at the end of its wind-up, not on declaration", "[strike]") {
    Bout b(/*wind_up=*/0.5f, /*recovery=*/0.0f);
    const float hp0 = b.victim_hp();

    b.declare();
    CHECK(b.victim_hp() == hp0);          // committed, nothing thrown yet
    CHECK(striking(b.game().registry, b.ae));

    // 500 ms = 15.15 ticks, so 15 ticks is still short of the deadline.
    b.tick(15);
    CHECK(b.victim_hp() == hp0);
    b.tick(1);
    CHECK(b.victim_hp() < hp0);
}

TEST_CASE("cooldown is stamped at resolve, not at declaration", "[strike]") {
    Bout b(0.5f, 0.0f);
    b.declare();
    CHECK(b.game().registry.get<Attacks>(b.ae).cooldown_remaining[0] == 0.0f);
    b.tick(16);
    CHECK(b.game().registry.get<Attacks>(b.ae).cooldown_remaining[0] > 0.0f);
}

TEST_CASE("a stun mid-wind-up cancels the blow entirely", "[strike][status]") {
    Bout b(0.5f, 0.0f);
    const float hp0 = b.victim_hp();

    b.declare();
    b.tick(5);  // partway in
    REQUIRE(b.victim_hp() == hp0);
    REQUIRE(apply_status(b.game(), b.ae, StatusKind::Stunned, 1000, UINT32_MAX));

    CHECK_FALSE(striking(b.game().registry, b.ae));
    b.tick(60);
    CHECK(b.victim_hp() == hp0);  // never landed, and never will
    // The attack was not spent: an interrupt costs tempo, not the attack.
    CHECK(b.game().registry.get<Attacks>(b.ae).cooldown_remaining[0] == 0.0f);

    bool cancelled = false;
    for (const GameEvent& ev : b.game().events) {
        cancelled = cancelled || (ev.kind == GameEventKind::StrikeCancelled &&
                                  ev.actor_id == b.attacker);
    }
    CHECK(cancelled);
}

TEST_CASE("a stun during recovery does NOT undo the blow", "[strike][status]") {
    Bout b(/*wind_up=*/0.2f, /*recovery=*/1.0f);
    const float hp0 = b.victim_hp();

    b.declare();
    b.tick(7);  // past 200 ms: thrown, now recovering
    REQUIRE(b.victim_hp() < hp0);
    const float after_hit = b.victim_hp();

    apply_status(b.game(), b.ae, StatusKind::Stunned, 1000, UINT32_MAX);
    CHECK(b.victim_hp() == after_hit);  // damage stays applied
    // cancel_strike refuses a thrown blow, so the recovery is still there.
    CHECK_FALSE(winding_up(b.game().registry, b.ae));
}

TEST_CASE("a committed striker does not move", "[strike][movement]") {
    Bout b(/*wind_up=*/1.0f, /*recovery=*/0.0f);
    BadlandsGame& g = b.game();
    // Give it somewhere to be: a durable MoveTarget it would otherwise walk to.
    g.registry.get<MoveTarget>(b.ae) = MoveTarget{MoveTarget::Kind::Point, {20.0f, 0.0f}};
    const glm::vec2 start = g.registry.get<Position>(b.ae).pos;

    b.declare();
    b.step_no_think(20);  // well inside the wind-up
    CHECK(glm::distance(g.registry.get<Position>(b.ae).pos, start) ==
          Catch::Approx(0.0f).margin(1e-4f));

    // Once free it walks again -- the route survived the commitment.
    b.step_no_think(40);
    CHECK(glm::distance(g.registry.get<Position>(b.ae).pos, start) > 0.5f);
}

TEST_CASE("only one strike at a time", "[strike]") {
    Bout b(0.5f, 0.0f);
    CHECK(declare_strike(b.game(), b.ae, 0, b.victim));
    CHECK_FALSE(declare_strike(b.game(), b.ae, 0, b.victim));
}

TEST_CASE("a committed actor's actions are refused at the gateway", "[strike][intention]") {
    Bout b(0.5f, 0.0f);
    b.declare();
    REQUIRE(striking(b.game().registry, b.ae));
    CHECK_FALSE(resolve_action(b.game(), b.attacker,
                               AgentAction{BL_ACT_ATTACK, b.victim, 0}));
    CHECK(b.game().command_queue.empty());
}

TEST_CASE("a ranged strike spawns its projectile when the draw completes", "[strike]") {
    Bout b(/*wind_up=*/0.5f, /*recovery=*/0.0f, AttackCategory::Ranged);
    // Far enough that the arrow spends several ticks in the air: at contact
    // range advance_projectiles resolves it inside the very tick it spawned
    // (kProjectileSpeed 24 u/s vs a 1-unit gap), so there would be nothing to
    // observe.
    b.game().registry.get<Position>(b.ve).pos = glm::vec2{5.0f, 0.0f};
    const float hp0 = b.victim_hp();

    b.declare();
    b.tick(10);
    CHECK(b.game().registry.view<Projectile>().size() == 0);  // still drawing
    CHECK(b.victim_hp() == hp0);
    b.tick(7);
    CHECK(b.game().registry.view<Projectile>().size() >= 1);  // loosed, in flight
    b.tick(15);
    CHECK(b.victim_hp() < hp0);  // and arrived
}

TEST_CASE("a blow whiffs on a target that stepped out of reach", "[strike]") {
    Bout b(/*wind_up=*/0.5f, /*recovery=*/0.0f);
    BadlandsGame& g = b.game();
    // Reach is 6.0 for this fixture's attack; move the victim well past it
    // while the swing is still being wound up.
    const float hp0 = b.victim_hp();
    b.declare();
    b.tick(5);
    g.registry.get<Position>(b.ve).pos = glm::vec2{40.0f, 0.0f};
    b.tick(20);

    CHECK(b.victim_hp() == hp0);  // not re-aimed
    // ...but the swing was spent: committing costs the attack whether or not
    // it connects, which is what makes backing out of reach worth doing.
    CHECK(g.registry.get<Attacks>(b.ae).cooldown_remaining[0] > 0.0f);
}

TEST_CASE("a shot is NOT eaten by the target backing off during the draw", "[strike]") {
    // An arrow leaves the bow at the end of the draw and then homes on its
    // target, so a target that retreated mid-wind-up is chased by the arrow
    // rather than missed by it -- which is also what happened before commitment
    // existed. Re-checking weapon range at resolve would silently eat the shot
    // AND its cooldown, routinely, now that heroes and archers both back away.
    Bout b(/*wind_up=*/0.5f, /*recovery=*/0.0f, AttackCategory::Ranged);
    BadlandsGame& g = b.game();
    g.registry.get<Position>(b.ve).pos = glm::vec2{5.0f, 0.0f};
    const float hp0 = b.victim_hp();

    b.declare();
    b.tick(5);
    // Past the bow's 6.0 reach while the draw is still running.
    g.registry.get<Position>(b.ve).pos = glm::vec2{9.0f, 0.0f};
    b.tick(12);
    CHECK(g.registry.view<Projectile>().size() >= 1);  // loosed anyway
    b.tick(30);
    CHECK(b.victim_hp() < hp0);  // and it arrived
}

TEST_CASE("a zero-wind-up strike has no cancellable window", "[strike][status]") {
    // No wind-up means the blow is thrown the instant it is declared; only
    // advance_strikes' position in the tick makes it observable in between.
    // A stun landing in that gap must NOT interrupt it, or every legacy 0/0
    // attack would become interruptible by command ordering alone.
    Bout b(/*wind_up=*/0.0f, /*recovery=*/0.0f);
    b.declare();
    REQUIRE(striking(b.game().registry, b.ae));
    CHECK_FALSE(cancel_strike(b.game(), b.ae));

    const float hp0 = b.victim_hp();
    apply_status(b.game(), b.ae, StatusKind::Stunned, 1000, UINT32_MAX);
    b.tick(1);
    CHECK(b.victim_hp() < hp0);  // the blow still landed
}

TEST_CASE("a zero-timing attack still resolves inside its own tick", "[strike]") {
    // Every un-authored attack, and every legacy fixture, is 0/0 -- those must
    // behave exactly as they did before commitment existed.
    Bout b(/*wind_up=*/0.0f, /*recovery=*/0.0f);
    const float hp0 = b.victim_hp();
    b.game().command_queue.push_back(
        {CommandKind::Attack, b.attacker, b.victim, {0.0f, 0.0f}, /*param_a=*/0});
    b.tick(1);
    CHECK(b.victim_hp() < hp0);
    CHECK_FALSE(striking(b.game().registry, b.ae));  // no residue
}

TEST_CASE("the captured attack survives a mid-swing stat change", "[strike]") {
    // A blow in flight is what it was when it was thrown -- the same rule
    // Projectile follows. Otherwise a level-up or a debuff landing during a
    // slow wind-up would retroactively rewrite a swing already committed.
    Bout b(/*wind_up=*/0.5f, /*recovery=*/0.0f);
    BadlandsGame& g = b.game();
    const float hp0 = b.victim_hp();
    b.declare();
    g.registry.get<Attacks>(b.ae).defs[0].base_damage = 100.0f;  // after committing
    b.tick(20);
    CHECK(hp0 - b.victim_hp() == Catch::Approx(5.0f));  // the damage it committed with
}
