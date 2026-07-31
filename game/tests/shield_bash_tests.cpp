// Casting, end to end through the engine seams: target resolution, the
// pre-rolled attack test, the effect's op batch, and what the UseSkill command
// handler does with it (game/src/skill_cast.h). Every case pins the combat
// gates at a DETERMINISTIC extreme (a chance of 0 or 1 makes the seeded roll
// certain), the same discipline combat_tests.cpp uses, so nothing here depends
// on a particular draw while still running the real resolver.

#include "skill_cast.h"

#include "combat.h"
#include "command.h"
#include "intention.h"    // resolve_action -- the gateway the brain uses
#include "progression.h"  // award_xp -- reaching the level the grant fires at
#include "components.h"
#include "game_state.h"
#include "sim_internal.hpp"
#include "brain_abi.h"  // BL_ACT_USE_SKILL
#include "skills.h"
#include "status.h"

#include <catch_amalgamated.hpp>

#include <entt/entt.hpp>

#include <memory>

using namespace badlands;

namespace {

// A duel staged for a certain outcome: the basher always lands (accuracy 1 vs
// no defense, no evasion) unless the caller dials the victim's gates up.
struct BashFixture {
    std::unique_ptr<BadlandsGame> game;
    uint32_t caster_slot = 0;
    uint32_t victim_slot = 0;

    BashFixture(float victim_defense = 0.0f, float victim_evasion = 0.0f,
                float apart = 1.0f, int32_t victim_team = 2) {
        game = make_flat_world();

        CharacterDesc basher{};
        basher.archetype = Archetype::Hero;
        basher.hero_class = HERO_MERCENARY;
        basher.team = 1;
        basher.hp = 30.0f;
        basher.move_speed = 2.5f;
        basher.size_x = basher.size_y = basher.size_z = 1.0f;
        basher.accuracy = 1.0f;
        basher.attack_count = 1;
        basher.attacks[0] = {AttackCategory::Melee, DamageType::Slashing, 6.0f, 1.5f, 1.0f, 0.0f};
        caster_slot = spawn_into(*game, basher);

        CharacterDesc victim{};
        victim.archetype = Archetype::Monster;
        victim.team = victim_team;
        victim.pos_x = apart;
        victim.hp = 18.0f;
        victim.move_speed = 3.0f;
        victim.size_x = victim.size_y = victim.size_z = 1.0f;
        victim.accuracy = 0.8f;
        victim.defense = victim_defense;
        victim.evasion = victim_evasion;
        victim.attack_count = 1;
        victim.attacks[0] = {AttackCategory::Melee, DamageType::Slashing, 3.0f, 1.2f, 0.8f, 0.0f};
        victim_slot = spawn_into(*game, victim);

        // The bash sits at skill slot 0 of the caster's loadout; the level-gated
        // grant path is progression's business, not this file's.
        learn_skill(game->registry.get<Skills>(caster()), SkillId::ShieldBash);
    }

    entt::entity caster() const { return game->slots[caster_slot]; }
    entt::entity victim() const { return game->slots[victim_slot]; }

    int64_t bash() {
        return apply_command(*game, Command{CommandKind::UseSkill, caster_slot, victim_slot,
                                            {0.0f, 0.0f}, /*param_a=*/0});
    }

    bool victim_stunned() const {
        return has_status(game->registry, victim(), StatusKind::Stunned);
    }
    float victim_hp() const { return game->registry.get<Health>(victim()).hp; }
    float skill_cooldown(int32_t idx = 0) const {
        return game->registry.get<Skills>(caster()).cooldown_remaining[idx];
    }
};

}  // namespace

TEST_CASE("a landed bash stuns the target and deals no damage", "[skills][cast]") {
    BashFixture f;
    const float hp_before = f.victim_hp();

    f.bash();

    CHECK(f.victim_stunned());
    CHECK(remaining_millis_of(f.game->registry, f.victim(), StatusKind::Stunned) == 3000);
    CHECK(f.victim_hp() == Catch::Approx(hp_before));  // pure control
}

TEST_CASE("a blocked bash stuns nobody", "[skills][cast]") {
    BashFixture f(/*victim_defense=*/1.0f);
    f.bash();
    CHECK_FALSE(f.victim_stunned());
}

TEST_CASE("a dodged bash stuns nobody", "[skills][cast]") {
    BashFixture f(/*victim_defense=*/0.0f, /*victim_evasion=*/1.0f);
    f.bash();
    CHECK_FALSE(f.victim_stunned());
}

TEST_CASE("casting stamps the skill's own cooldown and nothing else",
          "[skills][cast]") {
    BashFixture f;
    f.bash();
    CHECK(f.skill_cooldown() == Catch::Approx(12.0f));
    // The bash is not a swing: the melee attack it borrowed for the TEST is
    // left ready, so a bash and a sword blow can both land in one wake.
    CHECK(f.game->registry.get<Attacks>(f.caster()).cooldown_remaining[0] ==
          Catch::Approx(0.0f));
}

TEST_CASE("a cast on cooldown does nothing", "[skills][cast]") {
    BashFixture f;
    f.bash();
    REQUIRE(f.victim_stunned());
    // Clear the stun so a second application would be visible, then re-cast.
    f.game->registry.get<Statuses>(f.victim()).count = 0;
    f.game->events.clear();

    f.bash();
    CHECK_FALSE(f.victim_stunned());
    CHECK(f.game->events.empty());  // no SkillUsed either
}

TEST_CASE("a cast out of range does nothing", "[skills][cast]") {
    // The bash declares a melee test, so its range IS the caster's melee reach
    // (1.5) -- a victim 6 units away is out of it.
    BashFixture f(0.0f, 0.0f, /*apart=*/6.0f);
    f.bash();
    CHECK_FALSE(f.victim_stunned());
    CHECK(f.skill_cooldown() == Catch::Approx(0.0f));
}

TEST_CASE("a SelfOnly skill cast at somebody else is refused, not remapped",
          "[skills][cast]") {
    BashFixture f;
    Skills& sk = f.game->registry.get<Skills>(f.caster());
    learn_skill(sk, SkillId::Calcify);  // SelfOnly, skill slot 1
    REQUIRE(sk.count == 2);

    apply_command(*f.game, Command{CommandKind::UseSkill, f.caster_slot, f.victim_slot,
                                   {0.0f, 0.0f}, /*param_a=*/1});
    CHECK(f.skill_cooldown(1) == Catch::Approx(0.0f));  // nothing was cast

    // Cast at itself it is accepted (Calcify's own effect is a no-op, but the
    // cast still happens: cooldown stamped).
    apply_command(*f.game, Command{CommandKind::UseSkill, f.caster_slot, f.caster_slot,
                                   {0.0f, 0.0f}, /*param_a=*/1});
    CHECK(f.skill_cooldown(1) == Catch::Approx(20.0f));
}

TEST_CASE("a trigger the engine cannot execute is refused", "[skills][cast]") {
    for (SkillTrigger trigger : {SkillTrigger::Passive, SkillTrigger::Intention}) {
        BashFixture f;
        SkillCatalog cat = f.game->skills;
        cat.specs[static_cast<size_t>(SkillId::ShieldBash)].trigger = trigger;
        f.game->skills = cat;

        f.bash();
        CHECK_FALSE(f.victim_stunned());
        CHECK(f.skill_cooldown() == Catch::Approx(0.0f));
    }
}

TEST_CASE("a targeting mode the engine cannot resolve is refused",
          "[skills][cast]") {
    for (SkillTargetMode mode : {SkillTargetMode::Multi, SkillTargetMode::Point}) {
        BashFixture f;
        SkillCatalog cat = f.game->skills;
        cat.specs[static_cast<size_t>(SkillId::ShieldBash)].target = mode;
        f.game->skills = cat;

        f.bash();
        CHECK_FALSE(f.victim_stunned());
        CHECK(f.skill_cooldown() == Catch::Approx(0.0f));
    }
}

TEST_CASE("an op naming an entity outside the context is dropped",
          "[skills][cast]") {
    // The guard that bounds a (later, scripted) effect to the entities the
    // engine chose to show it.
    BashFixture f;
    const uint32_t targets[1] = {f.victim_slot};
    const SkillSpec& spec = f.game->skills.specs[static_cast<size_t>(SkillId::ShieldBash)];
    const BlSkillCastContext ctx =
        build_cast_context(*f.game, f.caster(), SkillId::ShieldBash, spec, targets, 1);

    BlSkillEffectBatch batch{};
    push_effect_op(batch, BlSkillEffectOp{BL_FX_APPLY_STATUS, 999u,
                                          static_cast<int32_t>(StatusKind::Stunned), 5000.0f});
    push_effect_op(batch, BlSkillEffectOp{BL_FX_APPLY_STATUS, f.victim_slot,
                                          static_cast<int32_t>(StatusKind::Stunned), 5000.0f});
    apply_effect_batch(*f.game, f.caster_slot, ctx, batch);

    CHECK(f.victim_stunned());  // the legitimate op still applied
    CHECK(remaining_millis_of(f.game->registry, f.victim(), StatusKind::Stunned) == 5000);
}

TEST_CASE("the cast context carries the pre-rolled test and the constants",
          "[skills][cast]") {
    BashFixture f;
    const uint32_t targets[1] = {f.victim_slot};
    const SkillSpec& spec = f.game->skills.specs[static_cast<size_t>(SkillId::ShieldBash)];
    const BlSkillCastContext ctx =
        build_cast_context(*f.game, f.caster(), SkillId::ShieldBash, spec, targets, 1);

    CHECK(ctx.version == BL_SKILL_ABI_VERSION);
    CHECK(ctx.skill_id == static_cast<int32_t>(SkillId::ShieldBash));
    CHECK(ctx.caster.slot == f.caster_slot);
    CHECK(ctx.caster.melee_range == Catch::Approx(1.5f));
    REQUIRE(ctx.target_count == 1);
    CHECK(ctx.targets[0].slot == f.victim_slot);
    CHECK(ctx.targets[0].relation == BL_REL_ENEMY);
    CHECK(ctx.targets[0].attack_test == BL_TEST_HIT);  // accuracy 1 vs no defense
    CHECK(ctx.targets[0].test_damage > 0.0f);
    CHECK(skill_constant(ctx, "stun_seconds", -1.0f) == Catch::Approx(3.0f));
}

TEST_CASE("a stunned target is defenceless against the bash's own test",
          "[skills][cast]") {
    // evasion 1.0 would dodge every bash -- unless the victim is already
    // stunned, which zeroes it (effective_combatant), so the context the effect
    // sees reports a hit.
    BashFixture f(/*victim_defense=*/0.0f, /*victim_evasion=*/1.0f);
    apply_status(*f.game, f.victim(), StatusKind::Stunned, 500, UINT32_MAX);

    const uint32_t targets[1] = {f.victim_slot};
    const SkillSpec& spec = f.game->skills.specs[static_cast<size_t>(SkillId::ShieldBash)];
    const BlSkillCastContext ctx =
        build_cast_context(*f.game, f.caster(), SkillId::ShieldBash, spec, targets, 1);
    CHECK(ctx.targets[0].evasion == Catch::Approx(0.0f));
    CHECK(ctx.targets[0].attack_test == BL_TEST_HIT);
}

TEST_CASE("a cast emits SkillUsed and StatusApplied", "[skills][events]") {
    BashFixture f;
    f.game->events.clear();
    f.bash();

    bool used = false;
    bool applied = false;
    for (const GameEvent& ev : f.game->events) {
        if (ev.kind == GameEventKind::SkillUsed) {
            used = true;
            CHECK(ev.actor_id == f.caster_slot);
            CHECK(ev.target_id == f.victim_slot);
            CHECK(ev.amount == Catch::Approx(static_cast<float>(SkillId::ShieldBash)));
        }
        if (ev.kind == GameEventKind::StatusApplied) {
            applied = true;
            CHECK(ev.target_id == f.victim_slot);
        }
    }
    CHECK(used);
    CHECK(applied);
}

TEST_CASE("a cast is appended to the command log", "[skills][cast]") {
    BashFixture f;
    const size_t before = f.game->command_log.size();
    f.bash();
    REQUIRE(f.game->command_log.size() == before + 1);
    const Command& logged = f.game->command_log.back();
    CHECK(logged.kind == CommandKind::UseSkill);
    CHECK(logged.actor == f.caster_slot);
    CHECK(logged.target_id == f.victim_slot);
    CHECK(logged.param_a == 0);
}

// --- end to end: a level-3 mercenary against a goblin ------------------------

TEST_CASE("a mercenary learns ShieldBash at level 3 and stuns what it hits",
          "[skills][e2e]") {
    auto game = make_flat_world();
    // Catalog spawns: the mercenary's grant list rides on its desc, so leveling
    // is the ONLY thing standing between it and the skill.
    const uint32_t merc = spawn_creature_into(*game, CreatureId::Mercenary,
                                              kPlayerTeam, {0.0f, 0.0f});
    const uint32_t gob =
        spawn_creature_into(*game, CreatureId::Goblin, /*team=*/1, {1.0f, 0.0f});
    const entt::entity me = game->slots[merc];
    const entt::entity ge = game->slots[gob];

    REQUIRE(game->registry.get<Skills>(me).count == 0);  // level 1: nothing yet
    award_xp(*game, merc, 100 + 303);                    // -> level 3
    REQUIRE(game->registry.get<HeroSimulationState>(me).level == 3);

    // Pin the combat gates so the OUTCOME is certain and this case tests the
    // pipeline rather than the dice (the same discipline as the cases above:
    // the catalog's own 0.9 accuracy vs the goblin's 0.05 defense / 0.10
    // evasion would land ~76% of the time, which is not a test).
    //
    // AFTER the level-up, deliberately: apply_level_stats (progression.h)
    // recomputes the Combatant from BaseStats + growth, so anything written
    // straight onto the live component before a level crossing is overwritten
    // by design.
    game->registry.get<Combatant>(me).accuracy = 1.0f;
    game->registry.get<Combatant>(ge).defense = 0.0f;
    game->registry.get<Combatant>(ge).evasion = 0.0f;
    const Skills& sk = game->registry.get<Skills>(me);
    REQUIRE(sk.count == 1);
    REQUIRE(sk.ids[0] == SkillId::ShieldBash);

    // Through the gateway, exactly as the wasm brain would.
    REQUIRE(resolve_action(*game, merc, AgentAction{BL_ACT_USE_SKILL, gob, /*arg=*/0}));
    apply_commands(*game);

    // The goblin is stunned: no defense, no thinking, no walking.
    REQUIRE(has_status(game->registry, ge, StatusKind::Stunned));
    CHECK(effective_combatant(game->registry, ge).evasion == Catch::Approx(0.0f));

    // Whether the goblin ACTED over a window: a swing always stamps its
    // attack cooldown, hit or miss, so this is roll-independent -- unlike
    // "did the mercenary take damage", and unlike "did it move" (the two are
    // already in contact, so a free goblin stands and fights rather than
    // walking anywhere).
    auto swung_within = [&](int ticks) {
        bool swung = false;
        for (int i = 0; i < ticks; ++i) {
            tick_world(*game, 1.0f / 30.0f);
            if (game->registry.get<Attacks>(ge).cooldown_remaining[0] > 0.0f) {
                swung = true;
            }
        }
        return swung;
    };

    const glm::vec2 start = game->registry.get<Position>(ge).pos;
    CHECK_FALSE(swung_within(60));  // ~2s of the 3s stun: no thinking, no swings
    CHECK(has_status(game->registry, ge, StatusKind::Stunned));
    CHECK(glm::distance(game->registry.get<Position>(ge).pos, start) ==
          Catch::Approx(0.0f));

    CHECK(swung_within(60));  // past the end of it: the goblin fights again
    CHECK_FALSE(has_status(game->registry, ge, StatusKind::Stunned));
}

TEST_CASE("the bash is on cooldown for its authored time", "[skills][e2e]") {
    auto game = make_flat_world();
    const uint32_t merc = spawn_creature_into(*game, CreatureId::Mercenary,
                                              kPlayerTeam, {0.0f, 0.0f});
    const uint32_t gob =
        spawn_creature_into(*game, CreatureId::Goblin, /*team=*/1, {1.0f, 0.0f});
    award_xp(*game, merc, 100 + 303);
    const entt::entity me = game->slots[merc];
    // Cooldown, not outcome, is what this case is about -- but a blocked bash
    // stamps the cooldown just the same, so no gates need pinning here.

    REQUIRE(resolve_action(*game, merc, AgentAction{BL_ACT_USE_SKILL, gob, 0}));
    apply_commands(*game);
    CHECK(game->registry.get<Skills>(me).cooldown_remaining[0] == Catch::Approx(12.0f));

    // Refused while it cools, and the cooldown really does tick with the world.
    CHECK_FALSE(resolve_action(*game, merc, AgentAction{BL_ACT_USE_SKILL, gob, 0}));
    for (int i = 0; i < 30; ++i) {
        tick_world(*game, 1.0f / 30.0f);
    }
    CHECK(game->registry.get<Skills>(me).cooldown_remaining[0] ==
          Catch::Approx(11.0f).margin(0.05f));
}

// --- review findings (2026-07-31): the vocabulary's uncast corners -----------

TEST_CASE("an unranged skill is castable at a distant target, not just at itself",
          "[skills][cast]") {
    // attack_test = none and no "range" constant means UNBOUNDED, not zero
    // reach (skill_cast.h). Treating 0 as a distance refused every target
    // except the caster, which would have made a support skill uncastable.
    BashFixture f(0.0f, 0.0f, /*apart=*/9.0f);
    SkillCatalog cat = f.game->skills;
    SkillSpec& bash = cat.specs[static_cast<size_t>(SkillId::ShieldBash)];
    bash.attack_test = SkillAttackTest::None;  // no test -> no borrowed reach
    f.game->skills = cat;

    CastPlan plan;
    CHECK(validate_cast(*f.game, f.caster_slot, 0, f.victim_slot, plan));
    CHECK(plan.target_count == 1);

    // An authored range constant is still honoured as a real limit.
    bash.constants[bash.constant_count].name = "range";
    bash.constants[bash.constant_count].value = 2.0f;
    ++bash.constant_count;
    f.game->skills = cat;
    CHECK_FALSE(validate_cast(*f.game, f.caster_slot, 0, f.victim_slot, plan));
}

TEST_CASE("an unknown targeting mode is refused, not silently cast at nobody",
          "[skills][cast]") {
    // Sim::SetSkillCatalog is public API; sanitize_skill_catalog clamps a
    // garbled mode, but validate_cast refuses one regardless rather than
    // resolving zero targets and reporting success.
    BashFixture f;
    SkillCatalog cat = f.game->skills;
    cat.specs[static_cast<size_t>(SkillId::ShieldBash)].target =
        static_cast<SkillTargetMode>(99);
    f.game->skills = cat;

    CastPlan plan;
    CHECK_FALSE(validate_cast(*f.game, f.caster_slot, 0, f.victim_slot, plan));
    f.bash();
    CHECK(f.skill_cooldown() == Catch::Approx(0.0f));  // no cooldown stamped
}

TEST_CASE("SetSkillCatalog clamps a garbled trigger/target/attack_test",
          "[skills]") {
    Sim sim{BrainDesc{}};
    SkillCatalog cat;
    cat.specs[0].trigger = static_cast<SkillTrigger>(42);
    cat.specs[0].target = static_cast<SkillTargetMode>(-3);
    cat.specs[0].attack_test = static_cast<SkillAttackTest>(9);
    sim.SetSkillCatalog(cat);

    const SkillSpec& s = sim.Skills().specs[0];
    CHECK(s.trigger == SkillTrigger::Passive);      // inert, never castable
    CHECK(s.target == SkillTargetMode::SelfOnly);   // cannot reach anyone else
    CHECK(s.attack_test == SkillAttackTest::None);
}
