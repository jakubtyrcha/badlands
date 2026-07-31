// Spec of the pure combat pipeline (game/src/combat.h). Every case pins a gate
// at a DETERMINISTIC extreme (a chance of 0 or 1 makes the seeded roll's outcome
// certain), tests the armour/damage-type formulas (no roll), or asserts a
// relationship that holds across seeds -- so nothing here depends on a specific
// draw while still exercising the real seeded resolver.

#include "combat.h"

#include "components.h"
#include "game_state.h"
#include "movement.h"
#include "strike_test_util.h"  // land_strikes -- fire_attack commits, it no longer resolves
#include "sim_internal.hpp"

#include <catch_amalgamated.hpp>

#include <entt/entt.hpp>

#include <cmath>

using namespace badlands;

namespace {

// A request whose gates are all wide open (accuracy 1, no defense/evasion/crit,
// no armour) so a plain slashing blow lands its full base damage. Cases narrow
// one dial at a time from here.
CombatRequest clean_hit(float base_damage) {
    CombatRequest r;
    r.attacker.accuracy = 1.0f;
    r.defender.defense = 0.0f;
    r.defender.evasion = 0.0f;
    r.defender.armour = 0.0f;
    r.attack.category = AttackCategory::Melee;
    r.attack.damage_type = DamageType::Slashing;
    r.attack.base_damage = base_damage;
    r.attack.crit_chance = 0.0f;
    r.attacker_slot = 1;
    r.target_slot = 2;
    r.world_millis = 1000;
    r.attack_index = 0;
    return r;
}

}  // namespace

TEST_CASE("a wide-open blow lands its full base damage", "[combat]") {
    CombatResult res = resolve_attack(clean_hit(10.0f));
    CHECK_FALSE(res.blocked);
    CHECK_FALSE(res.dodged);
    CHECK_FALSE(res.crit);
    CHECK(res.damage == 10.0f);
}

TEST_CASE("zero accuracy never lands", "[combat]") {
    CombatRequest r = clean_hit(10.0f);
    r.attacker.accuracy = 0.0f;
    CombatResult res = resolve_attack(r);
    CHECK(res.blocked);
    CHECK(res.damage == 0.0f);
}

TEST_CASE("full defense blocks the blow", "[combat]") {
    CombatRequest r = clean_hit(10.0f);
    r.defender.defense = 1.0f;  // parry beats accuracy 1.0 -> blocked
    CombatResult res = resolve_attack(r);
    CHECK(res.blocked);
    CHECK(res.damage == 0.0f);
}

TEST_CASE("blunt bypasses defense entirely", "[combat]") {
    CombatRequest r = clean_hit(10.0f);
    r.defender.defense = 1.0f;
    r.attack.damage_type = DamageType::Blunt;  // crushes through the shield
    CombatResult res = resolve_attack(r);
    CHECK_FALSE(res.blocked);
    CHECK(res.damage > 0.0f);
}

TEST_CASE("full evasion dodges an on-target blow", "[combat]") {
    CombatRequest r = clean_hit(10.0f);
    r.defender.evasion = 1.0f;  // melee slashing: mult 1.0 -> effective 1.0
    CombatResult res = resolve_attack(r);
    CHECK_FALSE(res.blocked);
    CHECK(res.dodged);
    CHECK(res.damage == 0.0f);
}

TEST_CASE("slashing damage is base minus armour, floored at 1", "[combat]") {
    CombatRequest r = clean_hit(10.0f);
    r.defender.armour = 3.0f;
    CHECK(resolve_attack(r).damage == 7.0f);

    r.defender.armour = 20.0f;  // over-armoured: still a scratch, never 0
    CHECK(resolve_attack(r).damage == 1.0f);
}

TEST_CASE("piercing is all-or-nothing against armour", "[combat]") {
    CombatRequest r = clean_hit(10.0f);
    r.attack.damage_type = DamageType::Piercing;

    r.defender.armour = 3.0f;  // penetrates: 10 - 3
    CHECK(resolve_attack(r).damage == 7.0f);

    r.defender.armour = 10.0f;  // no penetration -> no damage at all
    CHECK(resolve_attack(r).damage == 0.0f);

    r.defender.armour = 15.0f;
    CHECK(resolve_attack(r).damage == 0.0f);
}

TEST_CASE("blunt crushes through most of the armour", "[combat]") {
    CombatRequest r = clean_hit(10.0f);
    r.attack.damage_type = DamageType::Blunt;
    r.defender.armour = 10.0f;  // only 30% of armour applies: 10 - 3
    CHECK(resolve_attack(r).damage == Catch::Approx(7.0f));
}

TEST_CASE("a certain crit multiplies the penetrated damage", "[combat]") {
    CombatRequest r = clean_hit(10.0f);
    r.attack.crit_chance = 1.0f;
    CombatResult res = resolve_attack(r);
    CHECK(res.crit);
    CHECK(res.damage == 20.0f);  // 2x the penetrated 10

    r.defender.armour = 4.0f;  // crit applies AFTER armour: 2 * (10 - 4)
    CHECK(resolve_attack(r).damage == 12.0f);
}

TEST_CASE("ranged shots are harder to dodge than melee", "[combat]") {
    // evasion_mult scales the dodge chance: ranged < melee. For a fixed set of
    // seeds, every ranged dodge must also be a melee dodge (subset), and melee
    // must dodge strictly more often overall. Both gates ahead are wide open so
    // the roll reaches the evade gate.
    int melee_dodges = 0, ranged_dodges = 0, ranged_only = 0;
    for (uint32_t slot = 0; slot < 200; ++slot) {
        CombatRequest m = clean_hit(10.0f);
        m.defender.evasion = 0.6f;
        m.attacker_slot = slot;

        CombatRequest rg = m;
        rg.attack.category = AttackCategory::Ranged;
        rg.attack.damage_type = DamageType::Piercing;  // "ranged will be piercing"

        const bool md = resolve_attack(m).dodged;
        const bool rd = resolve_attack(rg).dodged;
        melee_dodges += md;
        ranged_dodges += rd;
        if (rd && !md) {
            ++ranged_only;  // a ranged-only dodge would break the subset property
        }
    }
    CHECK(ranged_only == 0);
    CHECK(ranged_dodges < melee_dodges);
    CHECK(ranged_dodges > 0);  // sanity: ranged still dodges sometimes
}

namespace {

// A ranged (index 0) + melee (index 1) loadout, both off cooldown.
Attacks bow_and_blade() {
    Attacks a{};
    a.count = 2;
    a.defs[0] = Attack{AttackCategory::Ranged, DamageType::Piercing, 5.0f, 6.0f, 1.0f, 0.3f};
    a.defs[1] = Attack{AttackCategory::Melee, DamageType::Slashing, 4.0f, 1.5f, 1.0f, 0.1f};
    return a;
}

}  // namespace

TEST_CASE("melee_range and ranged_range report the loadout's reach", "[combat]") {
    Attacks a = bow_and_blade();
    CHECK(melee_range(a) == 1.5f);
    CHECK(ranged_range(a) == 6.0f);

    Attacks melee_only{};
    melee_only.count = 1;
    melee_only.defs[0] = Attack{AttackCategory::Melee, DamageType::Blunt, 3.0f, 1.2f, 1.0f, 0.0f};
    CHECK(melee_range(melee_only) == 1.2f);
    CHECK(ranged_range(melee_only) == 0.0f);
}

TEST_CASE("pick_attack prefers a ranged shot when unlocked and in range", "[combat]") {
    Attacks a = bow_and_blade();
    CHECK(pick_attack(a, 4.0f, /*melee_locked=*/false) == 0);  // only ranged reaches
    CHECK(pick_attack(a, 1.0f, /*melee_locked=*/false) == 0);  // both reach -> prefer ranged
}

TEST_CASE("pick_attack falls back to melee when melee-locked", "[combat]") {
    Attacks a = bow_and_blade();
    CHECK(pick_attack(a, 1.0f, /*melee_locked=*/true) == 1);  // ranged disabled in a lock
}

TEST_CASE("pick_attack skips an attack still on cooldown", "[combat]") {
    Attacks a = bow_and_blade();
    a.cooldown_remaining[0] = 0.5f;  // the bow is recovering
    CHECK(pick_attack(a, 1.0f, /*melee_locked=*/false) == 1);  // so use the blade
}

TEST_CASE("pick_attack returns -1 when nothing is usable", "[combat]") {
    Attacks a = bow_and_blade();
    CHECK(pick_attack(a, 10.0f, false) == -1);  // beyond every range

    Attacks bow_only{};
    bow_only.count = 1;
    bow_only.defs[0] = Attack{AttackCategory::Ranged, DamageType::Piercing, 5.0f, 6.0f, 1.0f, 0.3f};
    CHECK(pick_attack(bow_only, 4.0f, /*melee_locked=*/true) == -1);  // ranged-only, locked
}

TEST_CASE("spawn derives a melee attack + default combatant from the desc", "[combat]") {
    auto owned = make_flat_world();
    BadlandsGame& g = *owned;
    CharacterDesc d{};
    d.hp = 10.0f;
    d.move_speed = 2.0f;
    d.attack_range = 1.5f;
    d.attack_damage = 4.0f;
    d.attack_cooldown = 1.0f;
    d.size_x = d.size_y = d.size_z = 1.0f;
    const uint32_t slot = spawn_into(g, d);
    const entt::entity e = g.slots[slot];

    REQUIRE(g.registry.all_of<Combatant>(e));
    REQUIRE(g.registry.all_of<Attacks>(e));
    const Attacks& atk = g.registry.get<Attacks>(e);
    REQUIRE(atk.count == 1);
    CHECK(atk.defs[0].category == AttackCategory::Melee);
    CHECK(atk.defs[0].base_damage == 4.0f);
    CHECK(atk.defs[0].range == 1.5f);
    CHECK(atk.defs[0].cooldown == 1.0f);
    // Default combatant => resolve_attack reduces to the old deterministic melee.
    const Combatant& cb = g.registry.get<Combatant>(e);
    CHECK(cb.accuracy == 1.0f);
    CHECK(cb.armour == 0.0f);
}

TEST_CASE("a zero-damage desc spawns with no attacks (harmless)", "[combat]") {
    auto owned = make_flat_world();
    BadlandsGame& g = *owned;
    CharacterDesc d{};
    d.hp = 8.0f;
    d.size_x = d.size_y = d.size_z = 1.0f;
    d.attack_damage = 0.0f;  // e.g. a deer: no derived attack
    const uint32_t slot = spawn_into(g, d);
    CHECK(g.registry.get<Attacks>(g.slots[slot]).count == 0);
}

TEST_CASE("spawn uses authored attacks + stats when the desc provides them", "[combat]") {
    auto owned = make_flat_world();
    BadlandsGame& g = *owned;
    CharacterDesc d{};
    d.hp = 10.0f;
    d.size_x = d.size_y = d.size_z = 1.0f;
    d.accuracy = 0.7f;
    d.armour = 2.0f;
    d.stance = CombatStance::Ranged;
    d.attack_count = 2;
    d.attacks[0] = Attack{AttackCategory::Ranged, DamageType::Piercing, 5.0f, 6.0f, 1.2f, 0.4f};
    d.attacks[1] = Attack{AttackCategory::Melee, DamageType::Slashing, 3.0f, 1.0f, 0.8f, 0.1f};
    const uint32_t slot = spawn_into(g, d);
    const entt::entity e = g.slots[slot];

    const Attacks& atk = g.registry.get<Attacks>(e);
    REQUIRE(atk.count == 2);
    CHECK(atk.defs[0].category == AttackCategory::Ranged);
    CHECK(atk.defs[0].range == 6.0f);
    CHECK(atk.defs[1].category == AttackCategory::Melee);
    const Combatant& cb = g.registry.get<Combatant>(e);
    CHECK(cb.accuracy == 0.7f);
    CHECK(cb.armour == 2.0f);
    CHECK(cb.stance == CombatStance::Ranged);
}

namespace {

// A stationary team-1 dummy target (no attacks) at `x`, and a team-0 shooter at
// the origin with a single long-range, always-hitting ranged attack.
uint32_t spawn_dummy(BadlandsGame& g, float x, int32_t team) {
    CharacterDesc d{};
    d.pos_x = x;
    d.team = team;
    d.hp = 20.0f;
    d.size_x = d.size_y = d.size_z = 1.0f;
    return spawn_into(g, d);  // attack_damage 0 -> harmless
}

uint32_t spawn_shooter(BadlandsGame& g) {
    CharacterDesc d{};
    d.team = 0;
    d.hp = 20.0f;
    d.size_x = d.size_y = d.size_z = 1.0f;
    d.accuracy = 1.0f;
    d.attack_count = 1;
    d.attacks[0] = Attack{AttackCategory::Ranged, DamageType::Piercing, 5.0f, 30.0f, 1.0f, 0.0f};
    return spawn_into(g, d);
}

}  // namespace

TEST_CASE("a ranged attack spawns a projectile that damages on arrival", "[combat]") {
    auto owned = make_flat_world();
    BadlandsGame& g = *owned;
    const uint32_t s = spawn_shooter(g);
    const uint32_t t = spawn_dummy(g, 10.0f, 1);
    const entt::entity te = g.slots[t];
    const float hp0 = g.registry.get<Health>(te).hp;

    // Fire at the named target; a projectile appears, no damage yet.
    // param_a left unnamed = its -1 default = auto-pick (Finding 2026-07-29
    // review fix): the shooter has only one attack, so auto-pick and an
    // explicit index 0 are equivalent here -- but say so, since which one
    // this is has flipped meaning across that fix.
    g.command_queue.push_back({CommandKind::Attack, s, t});
    apply_commands(g);
    testfix::land_strikes(g);  // the swing is committed here, then thrown
    CHECK(g.registry.view<Projectile>().size() == 1);
    CHECK(g.registry.get<Health>(te).hp == hp0);

    // Fly it in; damage lands only on arrival, then the projectile is spent.
    bool damaged = false;
    for (int i = 0; i < 90 && !damaged; ++i) {
        advance_projectiles(g, 1.0f / 30.0f);
        damaged = g.registry.get<Health>(te).hp < hp0;
    }
    CHECK(damaged);
    CHECK(g.registry.get<Health>(te).hp == hp0 - 5.0f);  // pierced armour 0
    CHECK(g.registry.view<Projectile>().size() == 0);
}

TEST_CASE("a projectile fizzles when its target dies mid-flight", "[combat]") {
    auto owned = make_flat_world();
    BadlandsGame& g = *owned;
    const uint32_t s = spawn_shooter(g);
    const uint32_t t = spawn_dummy(g, 20.0f, 1);  // far, so it won't arrive at once

    // param_a left unnamed = its -1 default = auto-pick (Finding 2026-07-29
    // review fix): the shooter has only one attack, so auto-pick and an
    // explicit index 0 are equivalent here -- but say so, since which one
    // this is has flipped meaning across that fix.
    g.command_queue.push_back({CommandKind::Attack, s, t});
    apply_commands(g);
    testfix::land_strikes(g);
    REQUIRE(g.registry.view<Projectile>().size() == 1);

    g.registry.destroy(g.slots[t]);  // target gone before the shot lands
    advance_projectiles(g, 1.0f / 30.0f);
    CHECK(g.registry.view<Projectile>().size() == 0);  // despawned, no crash
}

namespace {

// A melee(0)/ranged(1) loadout -- the OPPOSITE index order from
// bow_and_blade() above, deliberately: pick_attack/select_attack's auto-pick
// prefers ranged when unlocked, so this is the loadout that actually
// distinguishes "index 0" from "whatever the auto-pick chose" (the seam
// game/src/intention.h's resolve_action, and an explicit-index Attack
// command generally, depend on -- Command::param_a's default is -1/auto-pick
// since Finding 2026-07-29, so naming index 0 here is a deliberate choice,
// not what a bare command would do on its own).
Attacks blade_then_bow() {
    Attacks a{};
    a.count = 2;
    a.defs[0] = Attack{AttackCategory::Melee, DamageType::Slashing, 4.0f, 1.5f, 1.0f, 0.0f};
    a.defs[1] = Attack{AttackCategory::Ranged, DamageType::Piercing, 5.0f, 6.0f, 1.0f, 0.0f};
    return a;
}

}  // namespace

TEST_CASE("fire_attack: -1 still auto-picks (prefers ranged); an explicit index fires exactly "
          "that attack",
          "[combat]") {
    auto owned = make_flat_world();
    BadlandsGame& g = *owned;
    const uint32_t s = spawn_shooter(g);  // accuracy 1.0, one ranged attack by default
    const entt::entity se = g.slots[s];
    g.registry.get<Attacks>(se) = blade_then_bow();
    const uint32_t t = spawn_dummy(g, 1.0f, 1);  // within range of both attacks

    // -1 (the legacy/compat path, e.g. apply_intention's Shoot-case producer,
    // intention.cpp): auto-picks via select_attack, which prefers ranged
    // (index 1) when unlocked -- a projectile spawns, index 0's cooldown is
    // untouched.
    fire_attack(g, s, t, -1);
    testfix::land_strikes(g);
    CHECK(g.registry.view<Projectile>().size() == 1);
    CHECK(g.registry.get<Attacks>(se).cooldown_remaining[1] > 0.0f);
    CHECK(g.registry.get<Attacks>(se).cooldown_remaining[0] == 0.0f);
    g.registry.destroy(*g.registry.view<Projectile>().begin());
    g.registry.get<Attacks>(se).cooldown_remaining[1] = 0.0f;  // reset for the next call

    // Explicit index 0: fires the melee attack exactly, even though ranged
    // (index 1) is what auto-pick would have preferred.
    fire_attack(g, s, t, 0);
    testfix::land_strikes(g);
    CHECK(g.registry.view<Projectile>().size() == 0);  // melee, so no projectile
    CHECK(g.registry.get<Attacks>(se).cooldown_remaining[0] > 0.0f);
    CHECK(g.registry.get<Attacks>(se).cooldown_remaining[1] == 0.0f);  // ranged untouched
}

TEST_CASE("fire_attack: an explicit index re-validates range/cooldown/lock and no-ops if unusable",
          "[combat]") {
    auto owned = make_flat_world();
    BadlandsGame& g = *owned;
    const uint32_t s = spawn_shooter(g);
    const entt::entity se = g.slots[s];
    g.registry.get<Attacks>(se) = blade_then_bow();
    g.registry.emplace<MeleeLock>(se);  // locked: index 1 (Ranged) becomes illegal
    const uint32_t t = spawn_dummy(g, 1.0f, 1);
    const entt::entity te = g.slots[t];
    const float hp0 = g.registry.get<Health>(te).hp;

    fire_attack(g, s, t, /*attack_index=*/1);  // explicit, but Ranged-while-locked
    CHECK(g.registry.view<Projectile>().size() == 0);  // no-op: never fired
    CHECK(g.registry.get<Attacks>(se).cooldown_remaining[1] == 0.0f);  // untouched
    CHECK(g.registry.get<Health>(te).hp == hp0);
}

TEST_CASE(
    "fire_attack: an in-batch kill leaves a second explicit-index Attack against the same "
    "target as a no-op",
    "[combat]") {
    // Finding 4: the explicit-index path re-validates the index (attack_usable)
    // but never re-checked the target was still alive. Two attackers each name
    // the same victim's slot explicitly in the same command batch; the first
    // command to drain kills it, and the second must find a corpse, exactly
    // like select_target's auto-pick path (nearest_enemy's hp filter) already
    // refuses one.
    auto owned = make_flat_world();
    BadlandsGame& g = *owned;

    CharacterDesc victim_d{};
    victim_d.team = 1;
    victim_d.hp = 1.0f;  // dies to a single swing
    victim_d.size_x = victim_d.size_y = victim_d.size_z = 1.0f;
    const uint32_t v = spawn_into(g, victim_d);
    entt::entity ve = g.slots[v];

    CharacterDesc attacker_d{};
    attacker_d.team = 0;
    attacker_d.hp = 20.0f;
    attacker_d.size_x = attacker_d.size_y = attacker_d.size_z = 1.0f;
    attacker_d.accuracy = 1.0f;
    attacker_d.attack_count = 1;
    attacker_d.attacks[0] = Attack{AttackCategory::Melee, DamageType::Blunt, 10.0f, 1.5f, 1.0f, 0.0f};
    const uint32_t a1 = spawn_into(g, attacker_d);
    const uint32_t a2 = spawn_into(g, attacker_d);
    entt::entity a2e = g.slots[a2];

    // Both name the victim's slot explicitly (param_a = 0, this loadout's
    // only attack) in the same batch.
    g.command_queue.push_back({CommandKind::Attack, a1, v, {0.0f, 0.0f}, /*param_a=*/0});
    g.command_queue.push_back({CommandKind::Attack, a2, v, {0.0f, 0.0f}, /*param_a=*/0});
    apply_commands(g);
    testfix::land_strikes(g);

    CHECK(g.registry.get<Health>(ve).hp <= 0.0f);

    int32_t hits_on_victim = 0;
    for (const GameEvent& ev : g.events) {
        if (ev.kind == GameEventKind::DamageDealt && ev.target_id == v) {
            ++hits_on_victim;
        }
    }
    CHECK(hits_on_victim == 1);  // the second attacker's swing must not land

    // ...but it WAS thrown, and commitment (game/src/strike.h) moved where the
    // corpse is refused: fire_attack used to reject the second swing at
    // declaration, so its cooldown stayed untouched. Now both attackers commit
    // while the victim still lives, and deliver_strike drops the second when
    // it lands on a corpse. The swing is spent either way -- which is the
    // right way round, or an ally landing the killing blow first would refund
    // everyone else's attack.
    CHECK(g.registry.get<Attacks>(a2e).cooldown_remaining[0] > 0.0f);
}

TEST_CASE(
    "a raw Attack command with no param_a auto-picks a usable attack under MeleeLock",
    "[combat]") {
    // Finding 5: Command::param_a defaults to -1 (auto-pick), not 0 (an
    // explicit, and here illegal, index) -- a producer that pushes a bare
    // Attack command (no param_a named) must get the same auto-pick
    // resolve_action's own -1 path gets, not a forced index 0.
    auto owned = make_flat_world();
    BadlandsGame& g = *owned;
    const uint32_t s = spawn_shooter(g);  // accuracy 1.0
    const entt::entity se = g.slots[s];
    g.registry.get<Attacks>(se) = bow_and_blade();  // ranged (0) + melee (1)
    g.registry.emplace<MeleeLock>(se);              // ranged (index 0) is now illegal
    const uint32_t t = spawn_dummy(g, 1.0f, 1);      // within melee range
    const entt::entity te = g.slots[t];
    const float hp0 = g.registry.get<Health>(te).hp;

    g.command_queue.push_back({CommandKind::Attack, s, t});  // param_a left unnamed
    apply_commands(g);
    testfix::land_strikes(g);

    // Auto-pick chooses the usable melee attack (index 1): no projectile,
    // melee's cooldown spent, damage lands.
    CHECK(g.registry.view<Projectile>().size() == 0);
    CHECK(g.registry.get<Attacks>(se).cooldown_remaining[1] > 0.0f);
    CHECK(g.registry.get<Attacks>(se).cooldown_remaining[0] == 0.0f);  // ranged untouched
    CHECK(g.registry.get<Health>(te).hp < hp0);
}

TEST_CASE("the arena's blocked edges refuse a step past the wall", "[combat]") {
    WorldConfig cfg;
    cfg.terrain_blocking = false;
    cfg.prebuild_colony = false;
    cfg.arena_half_x = 10.0f;
    cfg.arena_half_z = 6.0f;
    auto owned = make_world(BrainDesc{}, cfg);
    BadlandsGame& g = *owned;

    CharacterDesc d{};
    d.pos_x = 8.0f;
    d.hp = 10.0f;
    d.move_speed = 6.0f;
    d.size_x = d.size_y = d.size_z = 1.0f;
    const entt::entity e = g.slots[spawn_into(g, d)];
    // Order it to march far past the east edge; drive movement directly (no brain).
    MoveTarget& mt = g.registry.get<MoveTarget>(e);
    mt.kind = MoveTarget::Kind::Point;
    mt.point = {100.0f, 0.0f};
    for (int i = 0; i < 200; ++i) {
        plan_paths(g, 1.0f / 30.0f);
        follow_paths(g, 1.0f / 30.0f);
    }
    CHECK(g.registry.get<Position>(e).pos.x <= cfg.arena_half_x + 1e-3f);  // never crossed
    CHECK(g.registry.all_of<MoveBlocked>(e));                              // and knows it
}

TEST_CASE("separation cannot shove a unit past the arena wall", "[combat]") {
    WorldConfig cfg;
    cfg.terrain_blocking = false;
    cfg.prebuild_colony = false;
    cfg.arena_half_x = 10.0f;
    cfg.arena_half_z = 6.0f;
    auto owned = make_world(BrainDesc{}, cfg);
    BadlandsGame& g = *owned;
    // Two units piled on the exact same spot hard against the east wall: the
    // push-apart shoves one outward, and without an arena clamp it lands outside
    // the wall (where follow_paths then refuses every inward step -- a freeze).
    CharacterDesc d{};
    d.pos_x = 9.9f;
    d.hp = 10.0f;
    d.size_x = d.size_y = d.size_z = 1.0f;
    spawn_into(g, d);
    spawn_into(g, d);
    separate_units(g);
    for (const CharacterState& row : characters_of(g)) {
        CHECK(std::abs(row.pos_x) <= cfg.arena_half_x + 1e-3f);
    }
}

TEST_CASE("Stats reach is derived from the primary attack, not a stale mirror", "[combat]") {
    auto owned = make_flat_world();
    BadlandsGame& g = *owned;
    CharacterDesc d{};
    d.hp = 10.0f;
    d.size_x = d.size_y = d.size_z = 1.0f;
    d.attack_count = 1;
    d.attacks[0] = Attack{AttackCategory::Ranged, DamageType::Piercing, 5.0f, 10.0f, 1.0f, 0.2f};
    d.attack_range = 3.0f;  // a legacy mirror that disagrees with the real attack
    const entt::entity e = g.slots[spawn_into(g, d)];
    // The behaviour-facing reach (perception's self_attack_range, the attack_range
    // host call) must follow the actual attack (10), not the stale mirror (3).
    CHECK(g.registry.get<Stats>(e).attack_range == 10.0f);
}

TEST_CASE("resolution is deterministic for identical requests", "[combat]") {
    CombatRequest r = clean_hit(10.0f);
    r.attacker.accuracy = 0.5f;  // mid-range so every gate actually rolls
    r.defender.defense = 0.3f;
    r.defender.evasion = 0.3f;
    r.attack.crit_chance = 0.3f;

    CombatResult a = resolve_attack(r);
    CombatResult b = resolve_attack(r);
    CHECK(a.blocked == b.blocked);
    CHECK(a.dodged == b.dodged);
    CHECK(a.crit == b.crit);
    CHECK(a.damage == b.damage);
}
