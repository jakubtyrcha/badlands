// The combat resolution pipeline (Stage-3). Two layers:
//   * resolve_attack -- a PURE function of its request, the CombatRequest ->
//     CombatResult transform the design doc calls for. All "randomness" is seeded
//     off the request's identity axes (attacker/target slot, world time, attack
//     index), which are all replay-reproducible, so a replayed command log
//     resolves every attack identically. No global RNG, no wall-clock.
//   * the game-coupled seams (select_target / select_attack / advance_projectiles)
//     are added alongside in later phases; this header starts with the pure core.

#pragma once

#include "components.h"  // Attacks, Combatant, Attack (+ badlands_sim.hpp transitively)

#include <entt/entt.hpp>

#include <cstdint>

struct BadlandsGame;

namespace badlands {

// One attack resolution's inputs. The four identity axes are the seed for the
// deterministic rolls -- captured at fire time for a projectile so resolution on
// arrival never depends on the attacker still being alive.
struct CombatRequest {
    Combatant attacker;
    Attack attack;
    Combatant defender;
    uint32_t attacker_slot = 0;
    uint32_t target_slot = 0;
    int64_t world_millis = 0;
    int32_t attack_index = 0;
};

// The outcome. `damage` is what to subtract from the defender's Health -- 0 when
// blocked, dodged, or fully stopped by armour. The booleans are for inspection /
// tests; only `damage` mutates the world.
struct CombatResult {
    bool blocked = false;  // stopped at the attack-vs-defense gate (parry or miss)
    bool dodged = false;   // evaded the on-target blow
    bool crit = false;     // penetrated damage was multiplied
    float damage = 0.0f;
};

// The seeded resolution pipeline (pure), early-exit in this order:
//   1. Attack vs Defense -> blocked | hit   (Blunt bypasses Defense)
//   2. Evade             -> dodged | through (evasion scaled by category/type)
//   3. Armour            -> penetrated damage (per damage type)
//   4. Crit              -> multiply penetrated damage
// Deterministic: identical requests produce identical results.
CombatResult resolve_attack(const CombatRequest& req);

// --- targeting + attack selection seams -------------------------------------
// Longest range among the loadout's melee / ranged attacks (0 if it has none of
// that category). Melee lock and stance engagement key off these rather than a
// single attack range.
float melee_range(const Attacks& atk);
float ranged_range(const Attacks& atk);

// How close this unit wants to get to its target: its ranged reach if it prefers
// to stay at range, its melee reach if it closes in. Falls back to the other
// category's reach when the preferred one is absent.
float engagement_range(const Combatant& stats, const Attacks& atk);

// PURE attack chooser: index into atk.defs of the attack to use against a target
// at `dist`, or -1 if none is usable. Considers only off-cooldown attacks whose
// range covers the target; excludes ranged attacks while melee-locked; prefers a
// ranged attack when not locked (a melee-stance unit still fires a ranged opener
// while closing).
int pick_attack(const Attacks& atk, float dist, bool melee_locked);

// The "pick the enemy" contract -- returns nearest_enemy today, the one place a
// Threat-Score model drops in later. Null if there is no enemy.
entt::entity select_target(const BadlandsGame& game, entt::entity self);

// Resolve one attack by `attacker_slot` against `target_slot` right now: the
// single entry point the Attack command handler calls. Authoritative -- it
// re-picks the attack (select_attack), re-validates, sets the attack's cooldown,
// and either applies melee damage immediately or spawns a projectile. A
// target_slot of UINT32_MAX means "engine picks the nearest enemy" (select_target),
// so mock/scripted brains can say only "attack" while the hunter names its prey.
// No-op if nothing is usable.
void fire_attack(BadlandsGame& game, uint32_t attacker_slot, uint32_t target_slot);

// Attack index (into the actor's Attacks) to use against `target` right now, or
// -1. Wraps pick_attack with the live distance + melee-lock state.
int select_attack(const BadlandsGame& game, entt::entity self, entt::entity target);

// Fly every in-flight projectile toward its target and resolve it on arrival
// (applying resolve_attack to the target's Health), despawning on hit or when the
// target is gone. A pure system rule -- runs identically live and on replay.
void advance_projectiles(BadlandsGame& game, float dt);

}  // namespace badlands
