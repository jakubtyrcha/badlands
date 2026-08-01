// A skill that takes TIME to cast -- the execution of SkillTrigger::Intention.
//
// The trigger has been declared and refused since the skill contract shipped,
// and its documented meaning is exactly a focus: "adopted as an intention, its
// effect landing after intention_duration_seconds of uninterrupted execution".
// This is that sentence, implemented.
//
// Deliberately shaped like strike.h, because it is the same idea on the other
// channel: one commitment at a time, and the phase is DERIVED FROM THE CLOCK
// rather than stored, so there is no state to fall out of sync.
//
//     world_ticks <  resolve_at  ->  focusing
//     world_ticks >= resolve_at  ->  the cast runs, the focus is dropped
//
// Where it differs from a strike, and the difference is the mechanic: a strike
// CAPTURES the attacker's stats at declaration, while a focus captures nothing
// and re-runs validate_cast at its deadline. A target that died, walked out of
// range, or became untargetable in those seconds simply gets no shot. That is
// what makes a long cast a real commitment rather than a delayed certainty.
//
// WHAT ENDS A FOCUS, and nothing else does:
//   * the deadline elapses and the cast is still legal -> it runs;
//   * a stun -> cancel_focus (status.cpp), the same interrupt that already
//     drops a wind-up;
//   * the brain adopts any intention that is not this identical focus
//     (apply_intention) -- which is the ONLY way "moving abandons it" is
//     reachable, since a focusing entity does not move;
//   * validate_cast disagreeing at the deadline (out of range, target gone).
//
// Determinism: the deadline is int64 ticks off the world clock, so a
// focus resolves on the same tick live and on replay. A focus is DERIVED
// STATE -- it follows from a logged intention plus the clock -- so it is not
// itself a Command, exactly like a strike and like projectile arrival.

#pragma once

#include "components.h"  // SkillFocus

#include <entt/entt.hpp>

#include <cstdint>

struct BadlandsGame;

namespace badlands {

// Commits `e` to `skill_index` against `target_slot`, reading the duration off
// that skill's spec. False (and no focus) if one is already running, if the
// index is not a live slot of its loadout, or if the skill declares no
// duration -- an "instant focus" is a contradiction, and silently running it
// as an ordinary cast would make the trigger meaningless.
bool begin_focus(BadlandsGame& game, entt::entity e, int32_t skill_index,
                 uint32_t target_slot);

// True while committed. The single gate movement and the think dispatch ask.
bool focusing(const entt::registry& reg, entt::entity e);

// Drops it with no cast and no cooldown spent; emits FocusCancelled. Returns
// whether anything was cancelled, so callers can be unconditional.
bool cancel_focus(BadlandsGame& game, entt::entity e);

// Per-tick sweep (step_world, sim.cpp): re-validates every focus whose
// deadline has elapsed and, when it still holds, runs the ordinary cast. Runs
// unconditionally, live and replaying alike, because it is a timer over
// derived state rather than a decision.
void advance_focus(BadlandsGame& game);

}  // namespace badlands
