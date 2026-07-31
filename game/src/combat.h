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

// The defender's tactical stats AS THEY COUNT RIGHT NOW -- the single place a
// status is allowed to change what resolve_attack sees. A Stunned defender has
// no ACTIVE defense: parry (defense) and evasion both read 0, because both are
// things a defender DOES rather than wears. Armour is unaffected (it is worn),
// and so is accuracy (that is the entity's own attacking, not its defending).
//
// Every CombatRequest.defender in the codebase goes through here, melee
// (fire_attack) and projectile-arrival (advance_projectiles) alike -- which is
// what makes "stunned means defenceless" true for a shot already in flight
// when the stun lands, without either call site knowing about statuses.
// Returns a default-constructed Combatant for an entity that has none.
Combatant effective_combatant(const entt::registry& reg, entt::entity e);

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

// PURE single-attack usability check: can atk.defs[idx] be used right now
// against a target at `dist`? In bounds, off cooldown, in range, and legal
// under a melee lock (no Ranged attack while locked). The one definition of
// "can I use attack i right now" -- pick_attack's scan (below) calls it per
// candidate, and so does anything that already knows WHICH index it wants
// and only needs to re-validate that one (fire_attack's explicit-index path,
// resolve_action's resolve-time validation, game/src/intention.h) rather
// than re-deriving the same four checks independently.
bool attack_usable(const Attacks& atk, int idx, float dist, bool melee_locked);

// PURE attack chooser: index into atk.defs of the attack to use against a target
// at `dist`, or -1 if none is usable. Considers only off-cooldown attacks whose
// range covers the target; excludes ranged attacks while melee-locked; prefers a
// ranged attack when not locked (a melee-stance unit still fires a ranged opener
// while closing). Built on attack_usable, above, one candidate at a time.
int pick_attack(const Attacks& atk, float dist, bool melee_locked);

// The "pick the enemy" contract -- returns nearest_enemy today, the one place a
// Threat-Score model drops in later. Null if there is no enemy.
entt::entity select_target(const BadlandsGame& game, entt::entity self);

// Resolve one attack by `attacker_slot` against `target_slot` right now: the
// single entry point the Attack command handler calls. Authoritative -- it
// picks the attack, re-validates, sets the attack's cooldown, and either
// applies melee damage immediately or spawns a projectile. A target_slot of
// UINT32_MAX means "engine picks the nearest enemy" (select_target), so
// mock/scripted brains can say only "attack" while the hunter names its
// prey. No-op if nothing is usable.
//
// `attack_index` picks WHICH attack: >= 0 fires exactly that index, re-
// validated here (attack_usable, above -- range/cooldown/lock) rather than
// trusted from whoever chose it (resolve_action, game/src/intention.h,
// validated it too, but the world may have moved on since -- this is the
// authoritative re-check, same discipline as the target itself). -1 is the
// legacy auto-pick: select_attack chooses for the caller, preserved for
// command-log compatibility with producers that still say only "attack"
// (apply_intention's Shoot case, intention.cpp -- explicit -1, not
// Command::param_a's own 0 default, which as of the explicit-index plumbing
// means "exactly attack 0"). Single-gateway combat (docs/superpowers/specs/
// 2026-07-25-contract-v3-alignment-design.md): no producer emits -1 for an
// AGENT's own swings anymore (resolve_action always names an explicit
// index, wasm hero or simple monster brain alike) -- Shoot (the hunter's
// prey-hunting) is the one surviving -1 producer, out of this slice's scope.
void fire_attack(BadlandsGame& game, uint32_t attacker_slot, uint32_t target_slot,
                 int32_t attack_index = -1);

// Attack index (into the actor's Attacks) to use against `target` right now, or
// -1. Wraps pick_attack with the live distance + melee-lock state. Single-
// gateway combat (docs/superpowers/specs/2026-07-25-contract-v3-alignment-
// design.md): host picking on an AGENT's behalf is retired -- this
// function's one remaining caller is fire_attack's own -1 legacy-auto-pick
// path (Shoot's producer, out of this slice's scope). pick_attack itself,
// above, is a POLICY HELPER a brain may call directly now, guest-side or
// host-side: hero.nim reimplements the same preference itself (pickBestAttack,
// scripts/brains/nim/hero.nim), and the simple monster brain calls pick_attack
// directly, host-side (monster_brain.cpp) -- neither goes through this
// wrapper, since a brain already knows its own live distance/lock state from
// its own tick.
int select_attack(const BadlandsGame& game, entt::entity self, entt::entity target);

// Fly every in-flight projectile toward its target and resolve it on arrival
// (applying resolve_attack to the target's Health), despawning on hit or when the
// target is gone. A pure system rule -- runs identically live and on replay.
void advance_projectiles(BadlandsGame& game, float dt);

}  // namespace badlands
