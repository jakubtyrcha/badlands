// Timed statuses: the one place a condition is applied, queried, and expired.
//
// The subsystem owns TIMERS ONLY. It never enforces what a status means --
// each system asks `has_status` where it already makes its own decision (the
// think dispatch in sim.cpp, movement.cpp's path following, combat.cpp's
// effective_combatant). Adding a status kind therefore costs one enum value
// plus the gates that care, and nothing here has to know about any of them.
//
// Determinism: durations are int64 milliseconds decremented by the
// compile-time kMillisPerTick (components.h), so every expiry lands on the
// same tick live and on replay. Application itself always arrives through a
// Command handler (never a brain, never a system's own initiative), which is
// what keeps the status stream in the command log.

#pragma once

#include "components.h"  // Statuses, StatusEntry (+ badlands_sim.hpp's StatusKind)

#include <entt/entt.hpp>

#include <cstdint>

struct BadlandsGame;

namespace badlands {

// True while `e` carries `kind`. Safe on an entity with no Statuses component.
bool has_status(const entt::registry& reg, entt::entity e, StatusKind kind);

// Milliseconds left on `kind`, or 0 when it is not present.
int64_t remaining_millis_of(const entt::registry& reg, entt::entity e, StatusKind kind);

// Applies (or refreshes) `kind` for `millis` milliseconds, emplacing Statuses
// on demand. Returns whether anything was applied.
//
//  * A non-positive `millis` is a no-op -- "for no time at all" is not an
//    affliction, and a skill whose duration constant is missing must not
//    stun forever (skill_abi.h's contract lets an effect emit a 0-duration op).
//  * A refresh keeps the LONGER remaining, never shortens: a second, weaker
//    stun landing mid-stun cannot cure the first one.
//  * A full component warns and drops rather than evicting something.
//
// Emits GameEventKind::StatusApplied on success.
bool apply_status(BadlandsGame& game, entt::entity e, StatusKind kind, int64_t millis,
                  uint32_t source_slot);

// Per-tick sweep (tick_world, sim.cpp): decrements every entry by
// kMillisPerTick and compacts out the expired ones. Runs unconditionally --
// live and replaying alike -- because it is pure derived state over a timer,
// not a decision.
void advance_statuses(BadlandsGame& game);

}  // namespace badlands
