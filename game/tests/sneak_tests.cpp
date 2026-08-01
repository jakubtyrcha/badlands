// Sneak and Calcify: two statuses that share one hook and one rule each.
//
// Sneak's rule is IMPERCEPTIBILITY, and the point of these cases is that it is
// enforced in exactly two functions -- nearest_enemy (game.cpp) and
// collect_threats (behaviours/perception.cpp) -- which between them are every
// way anything in this sim learns another entity is there. Nothing else has a
// special case, and the Attack-intention abort below is the proof: it falls out
// of select_target rather than being a branch anyone wrote.
//
// Calcify's rule is one stat and no others.

#include "combat.h"

#include "behaviours/perception.h"
#include "behaviours/world_view.h"
#include "command.h"
#include "components.h"
#include "game_state.h"
#include "intention.h"
#include "sim_internal.hpp"
#include "skill_cast.h"
#include "skills.h"
#include "status.h"
#include "strike.h"

#include <catch_amalgamated.hpp>

#include <entt/entt.hpp>

#include <memory>

using namespace badlands;

namespace {

// A grave robber and something hostile, a few metres apart on flat ground.
// Neither is melee-locked at construction, so a Sneak cast is legal until a
// case makes it otherwise.
struct SneakFixture {
    std::unique_ptr<BadlandsGame> game;
    uint32_t robber_slot = 0;
    uint32_t foe_slot = 0;

    explicit SneakFixture(float apart = 6.0f) {
        game = make_flat_world();

        CharacterDesc gr{};
        gr.archetype = Archetype::Hero;
        gr.hero_class = HERO_GRAVE_ROBBER;
        gr.team = 1;
        gr.hp = 24.0f;
        gr.move_speed = 2.8f;
        gr.size_x = gr.size_y = gr.size_z = 1.0f;
        gr.accuracy = 0.5f;   // room to see the sneak bonus move it
        gr.attack_count = 1;
        gr.attacks[0] = {AttackCategory::Melee, DamageType::Slashing, 4.0f, 1.3f, 0.7f, 0.0f};
        robber_slot = spawn_into(*game, gr);

        CharacterDesc foe{};
        foe.archetype = Archetype::Monster;
        foe.team = 2;
        foe.pos_x = apart;
        foe.hp = 30.0f;
        foe.move_speed = 2.0f;
        foe.size_x = foe.size_y = foe.size_z = 1.0f;
        foe.accuracy = 0.8f;
        foe.attack_count = 1;
        foe.attacks[0] = {AttackCategory::Melee, DamageType::Slashing, 3.0f, 1.2f, 0.8f, 0.0f};
        foe_slot = spawn_into(*game, foe);

        learn_skill(game->registry.get<Skills>(robber()), SkillId::Sneak);
    }

    entt::entity robber() const { return game->slots[robber_slot]; }
    entt::entity foe() const { return game->slots[foe_slot]; }

    // Casts through the authoritative gate (the UseSkill command handler), so
    // every case here exercises the same path a brain's action would.
    int64_t cast_sneak() {
        return apply_command(*game, Command{CommandKind::UseSkill, robber_slot, robber_slot,
                                            {0.0f, 0.0f}, /*param_a=*/0});
    }
    bool sneaking() const { return has_status(game->registry, robber(), StatusKind::Sneaking); }
};

// Does `observer` perceive `subject` as a threat right now?
bool perceives(const BadlandsGame& game, entt::entity observer, entt::entity subject,
               uint32_t subject_slot) {
    WorldView v{};
    collect_threats(game, observer, game.registry.get<Position>(observer).pos,
                    /*radius=*/50.0f, ThreatPolicy::HostileTeam, v);
    (void)subject;
    for (int32_t i = 0; i < v.threat_count; ++i) {
        if (v.threats[i].slot == subject_slot) {
            return true;
        }
    }
    return false;
}

}  // namespace

TEST_CASE("a sneaking entity is not selected as a target", "[sneak]") {
    SneakFixture f;
    REQUIRE(select_target(*f.game, f.foe()) == f.robber());

    REQUIRE(f.cast_sneak() >= 0);
    REQUIRE(f.sneaking());
    CHECK(select_target(*f.game, f.foe()) == entt::entity{entt::null});

    // ...and it comes back when the status does not: this is a status, not a
    // one-way disappearance.
    clear_status(*f.game, f.robber(), StatusKind::Sneaking);
    CHECK(select_target(*f.game, f.foe()) == f.robber());
}

TEST_CASE("a sneaking entity is not perceived as a threat", "[sneak]") {
    SneakFixture f;
    REQUIRE(perceives(*f.game, f.foe(), f.robber(), f.robber_slot));

    REQUIRE(f.cast_sneak() >= 0);
    CHECK_FALSE(perceives(*f.game, f.foe(), f.robber(), f.robber_slot));
}

TEST_CASE("an attack intention aborts when its target vanishes", "[sneak][intention]") {
    // Nothing in advance_intentions knows about Sneak. Its Attack case ends the
    // intention when select_target finds nobody, and a sneaking entity is
    // nobody -- so the abort is the perception skip showing through rather than
    // a rule of its own.
    SneakFixture f;
    Intention attack;
    attack.kind = IntentionKind::Attack;
    REQUIRE(apply_intention(*f.game, f.foe_slot, attack));
    REQUIRE(f.game->registry.get<CurrentIntention>(f.foe()).kind == IntentionKind::Attack);

    REQUIRE(f.cast_sneak() >= 0);
    advance_intentions(*f.game);
    CHECK(f.game->registry.get<CurrentIntention>(f.foe()).kind == IntentionKind::None);
}

TEST_CASE("sneak cannot be cast while melee-locked", "[sneak][skill]") {
    // The gate, not the plumbing: the same fixture casts fine without the lock,
    // so what the lock changes is the only thing under test.
    SneakFixture f;
    f.game->registry.emplace<MeleeLock>(f.robber());
    CHECK_FALSE(f.sneaking());
    f.cast_sneak();
    CHECK_FALSE(f.sneaking());

    f.game->registry.remove<MeleeLock>(f.robber());
    REQUIRE(f.cast_sneak() >= 0);
    CHECK(f.sneaking());
}

TEST_CASE("declaring a strike ends sneak, and that strike keeps the bonus", "[sneak][combat]") {
    // The ordering IS the skill: declare_strike captures the attacker's stats
    // and only then clears the status, so the blow that gives away the sneak is
    // the blow that was paid for by it.
    SneakFixture f;
    const Combatant plain = effective_combatant(f.game->registry, f.robber());

    REQUIRE(f.cast_sneak() >= 0);
    REQUIRE(f.sneaking());
    const Combatant hidden = effective_combatant(f.game->registry, f.robber());
    CHECK(hidden.accuracy > plain.accuracy);
    CHECK(hidden.crit_multiplier > plain.crit_multiplier);
    // Defensive stats are untouched -- unseen is not harder to hit.
    CHECK(hidden.evasion == plain.evasion);
    CHECK(hidden.defense == plain.defense);

    REQUIRE(declare_strike(*f.game, f.robber(), 0, f.foe_slot));
    CHECK_FALSE(f.sneaking());
    const StrikeInProgress& s = f.game->registry.get<StrikeInProgress>(f.robber());
    CHECK(s.attacker.accuracy == Catch::Approx(hidden.accuracy));
    CHECK(s.attacker.crit_multiplier == Catch::Approx(hidden.crit_multiplier));
}

TEST_CASE("a damaging cast ends sneak; a self-buff does not", "[sneak][skill]") {
    // What makes a skill aggressive is what it DID -- an op that hurt somebody
    // else -- not what it is called. So this is checked per op.
    SneakFixture f(/*apart=*/1.0f);
    learn_skill(f.game->registry.get<Skills>(f.robber()), SkillId::Calcify);
    learn_skill(f.game->registry.get<Skills>(f.robber()), SkillId::Backstab);
    const auto slot_of = [&](SkillId id) {
        const Skills& sk = f.game->registry.get<Skills>(f.robber());
        for (int32_t i = 0; i < sk.count; ++i) {
            if (sk.ids[i] == id) return i;
        }
        return -1;
    };

    REQUIRE(f.cast_sneak() >= 0);
    REQUIRE(f.sneaking());
    apply_command(*f.game, Command{CommandKind::UseSkill, f.robber_slot, f.robber_slot,
                                   {0.0f, 0.0f}, slot_of(SkillId::Calcify)});
    CHECK(f.sneaking());  // hardening your own hide is not an act of aggression

    apply_command(*f.game, Command{CommandKind::UseSkill, f.robber_slot, f.foe_slot,
                                   {0.0f, 0.0f}, slot_of(SkillId::Backstab)});
    CHECK_FALSE(f.sneaking());
}

TEST_CASE("sneak expires on its own timer", "[sneak]") {
    SneakFixture f;
    REQUIRE(f.cast_sneak() >= 0);
    const int64_t left = remaining_ticks_of(f.game->registry, f.robber(), StatusKind::Sneaking);
    REQUIRE(left > 0);

    // Ticked by the compile-time per-tick constant, so the deadline is exact
    // rather than a count of frames. Rounded UP: a duration that is not a whole
    // number of ticks has a remainder tick, and truncating would assert the
    // status was gone while it still had milliseconds left.
    const int64_t ticks = (left + kTicksPerStep - 1) / kTicksPerStep;
    for (int64_t i = 0; i < ticks - 1; ++i) {
        advance_statuses(*f.game);
    }
    CHECK(f.sneaking());
    advance_statuses(*f.game);
    CHECK_FALSE(f.sneaking());
}

TEST_CASE("calcify raises armour for its duration and no other stat", "[calcify]") {
    SneakFixture f;
    learn_skill(f.game->registry.get<Skills>(f.robber()), SkillId::Calcify);
    const Skills& sk = f.game->registry.get<Skills>(f.robber());
    int32_t idx = -1;
    for (int32_t i = 0; i < sk.count; ++i) {
        if (sk.ids[i] == SkillId::Calcify) idx = i;
    }
    REQUIRE(idx >= 0);

    const Combatant before = effective_combatant(f.game->registry, f.robber());
    REQUIRE(apply_command(*f.game, Command{CommandKind::UseSkill, f.robber_slot, f.robber_slot,
                                           {0.0f, 0.0f}, idx}) >= 0);
    REQUIRE(has_status(f.game->registry, f.robber(), StatusKind::Calcified));

    const Combatant hard = effective_combatant(f.game->registry, f.robber());
    CHECK(hard.armour > before.armour);
    // A ward, not a general buff: everything else is exactly where it was.
    CHECK(hard.accuracy == before.accuracy);
    CHECK(hard.evasion == before.evasion);
    CHECK(hard.defense == before.defense);
    CHECK(hard.crit_multiplier == before.crit_multiplier);

    clear_status(*f.game, f.robber(), StatusKind::Calcified);
    CHECK(effective_combatant(f.game->registry, f.robber()).armour ==
          Catch::Approx(before.armour));
}
