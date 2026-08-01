// Timed statuses: the one place a condition is applied, queried, and expired.
//
// The subsystem owns TIMERS ONLY. It never enforces what a status means --
// each system asks `has_status` where it already makes its own decision (the
// think dispatch in sim.cpp, movement.cpp's path following, combat.cpp's
// effective_combatant). Adding a status kind therefore costs one enum value
// plus the gates that care, and nothing here has to know about any of them.
//
// Determinism: durations are int64 ticks decremented by the
// compile-time kTicksPerStep (components.h), so every expiry lands on the
// same tick live and on replay.
//
// Application usually arrives through a Command handler (never a brain), which
// keeps the status stream in the command log. The one exception is
// StatusKind::Disengaged, applied directly by update_melee_locks
// (game/src/movement.h) -- it is a PURE SYSTEM RULE over deterministic state
// ("you walked out of contact this tick"), not a decision anyone took, and it
// replays by re-running the same system. Exactly the precedent
// advance_projectiles sets by applying arrival damage outside the log.

#pragma once

#include "components.h"  // Statuses, StatusEntry (+ badlands_sim.hpp's StatusKind)

#include <entt/entt.hpp>

#include <cstdint>

struct BadlandsGame;

namespace badlands {

// True while `e` carries `kind`. Safe on an entity with no Statuses component.
bool has_status(const entt::registry& reg, entt::entity e, StatusKind kind);

// Ticks left on `kind`, or 0 when it is not present.
int64_t remaining_ticks_of(const entt::registry& reg, entt::entity e, StatusKind kind);

// Applies (or refreshes) `kind` for `millis` ticks, emplacing Statuses
// on demand. Returns whether anything was applied.
//
//  * A non-positive `millis` is a no-op -- "for no time at all" is not an
//    affliction, and a skill whose duration constant is missing must not
//    stun forever (skill_abi.h's contract lets an effect emit a 0-duration op).
//  * A refresh keeps the LONGER remaining, never shortens: a second, weaker
//    stun landing mid-stun cannot cure the first one.
//  * A full component warns and drops rather than evicting something.
//
// StatusKind::Stunned additionally ABORTS the victim's running
// CurrentIntention (intention.h's abort_current_intention), so the brain
// re-decides from scratch on its first wake after the stun rather than
// resuming a plan it made before being hit. Emits GameEventKind::StatusApplied
// on success.
bool apply_status(BadlandsGame& game, entt::entity e, StatusKind kind, int64_t millis,
                  uint32_t source_slot);

// Ends `kind` NOW, ahead of its timer. Returns whether anything was cleared.
//
// The counterpart to apply_status, and used where a status has a condition
// other than time that ends it -- Sneaking, whose whole shape is "until you do
// something aggressive" (combat.h's end_sneak_on_aggression). Emits nothing: an
// expiry is not an event today either, and a status ending early should not read
// differently from one running out.
bool clear_status(BadlandsGame& game, entt::entity e, StatusKind kind);

// Per-tick sweep (step_world, sim.cpp): decrements every entry by
// kTicksPerStep and compacts out the expired ones. Runs unconditionally --
// live and replaying alike -- because it is pure derived state over a timer,
// not a decision.
void advance_statuses(BadlandsGame& game);

}  // namespace badlands
