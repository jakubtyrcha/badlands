// Attack commitment: an attack takes TIME to throw.
//
// Declaring one starts a WIND-UP; the blow lands when it elapses; a RECOVERY
// follows. Through both phases the attacker neither moves nor thinks -- which
// is what makes standing still to shoot a real cost, and so what makes kiting
// cost ground rather than being free.
//
// The wind-up is CANCELLABLE and the recovery is not. That asymmetry is the
// mechanic: a stun landing mid-wind-up drops the blow entirely (turning Stunned
// from a debuff into an interrupt, and giving shield-bash a second purpose),
// while a stun during recovery only prevents what comes next -- the swing was
// already thrown.
//
// Phase is DERIVED FROM THE CLOCK rather than stored, so there is no phase enum
// to keep in sync with the timers:
//     world_millis <  resolve_at  ->  WIND-UP   (cancellable)
//     world_millis >= resolve_at  ->  RECOVERY  (committed)
//     world_millis >= free_at     ->  gone
//
// Determinism: both deadlines are int64 milliseconds off the world clock, so a
// strike resolves on the same tick live and on replay. A strike is derived
// state -- it follows from a logged Attack command plus the clock -- so it is
// NOT itself a command, exactly like projectile arrival (combat.h's
// advance_projectiles), which resolves damage outside the log for the same
// reason.

#pragma once

#include "components.h"  // StrikeInProgress

#include <entt/entt.hpp>

#include <cstdint>

struct BadlandsGame;

namespace badlands {

// Commits `e` to attack `attack_index` against `target_slot`, reading the
// wind-up/recovery off that attack. Captures the attacker's stats and the
// attack itself AT DECLARATION -- a growth-driven stat change, a curse, or the
// attacker's own death mid-swing must not retroactively change a blow already
// in flight, the same capture-at-fire-time rule Projectile follows.
//
// False (and no strike) if `e` already has one: one commitment at a time is
// what stops a brain queueing swings faster than it can throw them.
bool declare_strike(BadlandsGame& game, entt::entity e, int32_t attack_index,
                    uint32_t target_slot);

// True while `e` is committed, EITHER phase. The single gate movement, the
// think dispatch, and the action channel all ask.
bool striking(const entt::registry& reg, entt::entity e);

// True only during the cancellable wind-up -- the blow has not landed yet.
bool winding_up(const entt::registry& reg, entt::entity e);

// Drops a strike still in WIND-UP: no damage, no cooldown spent, and a
// GameEventKind::StrikeCancelled for the log. A strike already in recovery is
// left alone (the blow was thrown; only what comes next is preventable).
// Returns whether anything was cancelled.
bool cancel_strike(BadlandsGame& game, entt::entity e);

// Per-tick sweep (tick_world, sim.cpp): resolves strikes whose wind-up has
// elapsed -- melee damage applied, or the projectile spawned, via combat.h's
// deliver_strike -- stamping the attack's cooldown AT RESOLVE, then drops
// those whose recovery has elapsed. Runs unconditionally, live and replaying
// alike, because it is a timer over derived state rather than a decision.
void advance_strikes(BadlandsGame& game);

}  // namespace badlands
