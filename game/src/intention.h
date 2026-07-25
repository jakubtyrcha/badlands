// The engine side of the intention contract
// (docs/design/intention-contract.html): validates a brain's suggestion,
// executes it through the existing command producers, tracks a hero's
// CurrentIntention to completion/abort, maintains the event inbox, and
// decides when a hero is worth waking.
//
// Wired in: sim.cpp's think loop gates every wasm hero's tick_wasm_brain
// on should_wake, and wasm_brain.cpp
// decodes each wake's BlSuggestionWire into an Intention and adopts it via
// apply_intention. should_wake/apply_intention are wasm-only: the C++ hero
// decision layer they used to run alongside (town_think) is gone entirely
// (Task 4) -- a hero with no wasm brain loaded simply idles.

#pragma once

#include "components.h"  // IntentionKind, CurrentIntention, InboxEvent, EventInbox

#include <glm/glm.hpp>

#include <cstdint>

#include <entt/entt.hpp>

struct BadlandsGame;

namespace badlands {

// Host-native decoded suggestion -- what a brain wake hands the engine (the
// wire's BlSuggestionWire, once the flip task lands it).
struct Intention {
    IntentionKind kind = IntentionKind::None;
    glm::vec2 point{0.0f, 0.0f};
    uint32_t target_slot = UINT32_MAX;
    int32_t arg = 0;
    int64_t duration_millis = 0;   // Idle
    int32_t activity_label = -1;   // ActivityId, inspection only
    int64_t idle_hint_millis = 0;
};

// Validates + adopts a suggestion for the hero at `slot`: feasibility-checks
// it (known target, enterable building, sane point), maps it onto the
// existing command producers (enqueue_move_to / enqueue_set_behavior /
// direct command_queue pushes), stamps CurrentIntention, and logs the wake
// schedule via the SetBehavior duration, so the schedule is replayable
// rather than recomputed. Invalid -> spdlog::warn + no execution +
// CurrentIntention left untouched. Returns whether the suggestion was
// adopted.
//
// Purely validate-and-adopt: it does NOT touch the wake-bookkeeping fields
// (EventInbox::last_seen_seq, CurrentIntention::last_think_millis/wake_at_
// millis-on-rejection) -- that is note_think_outcome's job (below), a
// separate call the caller makes once per think regardless of what
// apply_intention decided. Splitting them keeps "did we adopt this
// suggestion" and "did the hero just get consulted" independently testable
// (a caller that never calls note_think_outcome, e.g. a test driving
// apply_intention directly, gets adoption semantics with no wake-schedule
// side effects).
bool apply_intention(BadlandsGame& game, uint32_t slot, const Intention& intent);

// --- action channel (v3, docs/superpowers/specs/2026-07-25-contract-v3-
// alignment-design.md): a second, write-only channel alongside the one
// suggestion a wake still returns -- fire-and-forget instant actions,
// independently validated at resolve time, any number per wake. Only
// BL_ACT_ATTACK is live this slice; BL_ACT_USE_SKILL/USE_POTION stay
// reserved. ------------------------------------------------------------------

// Host-native decoded action -- what one bl_enqueue_action call hands the
// engine (brain_abi.h's BL_ACT_* wire, an append-only mirror the same way
// Intention mirrors BL_INT_*). `kind` holds the raw BL_ACT_* value directly
// (no separate C++ enum -- unlike IntentionKind, this vocabulary has exactly
// one live member so far and gains members rarely enough that a mirror enum
// would only add a translation step); `arg` is BL_ACT_ATTACK's attack index.
struct AgentAction {
    int32_t kind = 0;                       // BL_ACT_*
    uint32_t target_slot = UINT32_MAX;      // UINT32_MAX = infer (see resolve_action)
    int32_t arg = 0;                        // BL_ACT_ATTACK: attack index
};

// The single action gateway: both the wasm callback drain (tick_wasm_brain,
// wasm_brain.cpp) and any future simple/engine-side brain call this to fire
// one instant action, validated against the world AS IT STANDS RIGHT NOW --
// never against a stale snapshot from earlier in the wake. Only
// BL_ACT_ATTACK is live; every other kind (BL_ACT_NONE, the reserved
// BL_ACT_USE_SKILL/USE_POTION, and anything unrecognized) is a warn + drop,
// same forward-compat posture as decode_suggestion's unknown-vocabulary case
// (wasm_brain.cpp) -- a newer guest's action kind this host hasn't learned
// about yet is not a crash.
//
// BL_ACT_ATTACK validation, in order, each a warn + false + no command on
// failure:
//   1. `arg` is a live index into the actor's Attacks (0 <= arg < count).
//   2. That attack is off cooldown.
//   3. Its category is legal under the actor's current MeleeLock (no
//      Ranged while locked).
//   4. The target resolves: a named target_slot must be a live entity;
//      UINT32_MAX only resolves while the actor's CurrentIntention.kind is
//      ALREADY Attack (melee whatever it's engaged with, the same "actor-
//      only" contract apply_intention's own Attack case documents) -- via a
//      live select_target (combat.h) scan, never a cached one (this can be
//      called from contexts a per-tick scan cache was never populated for).
//      Any other running kind (or none at all) has nothing to infer a
//      target from and drops.
//   5. The resolved target is within that attack's range of the actor.
//
// Valid -> pushes `{CommandKind::Attack, slot, resolved_target, param_a =
// arg}` onto game.command_queue (the FIFO queue the drain loop and every
// other action call this same wake share, so actions apply in the order
// they were enqueued) and returns true. `resolved_target` is always a
// concrete slot, never a UINT32_MAX pass-through, even when `action.
// target_slot` was UINT32_MAX -- the command_log is self-describing without
// having to re-run this resolution to read it back.
//
// Deliberately does NOT apply the command itself (apply_commands does that,
// once, in the same ordered drain every other AI command goes through) --
// so two actions resolved in the same wake both see the SAME pre-batch
// registry state; if they name the same attack index, both individually
// validate here, and it is fire_attack's own re-check (combat.h, the
// authoritative one) that lets only the first actually land once
// apply_commands runs. Never touches CurrentIntention, adopted or dropped --
// an action is orthogonal to the suggestion a wake also returns, not a
// replacement for it (apply_intention owns CurrentIntention exclusively).
bool resolve_action(BadlandsGame& game, uint32_t slot, const AgentAction& action);

// Wake bookkeeping for one think, called ONCE per wake regardless of what
// the brain suggested (tick_wasm_brain, wasm_brain.cpp, calls this right
// after apply_intention -- see that function's own doc comment on why the
// two are split). Unconditionally: stamps CurrentIntention::last_think_
// millis to now (inspection only -- should_wake no longer reads it, see its
// own comment) and EventInbox::last_seen_seq to the inbox's current
// last_pushed_seq (components.h), so should_wake's event clause reflects
// "nothing new since this look." When `adopted` is false (the suggestion
// was rejected or explicitly BL_INT_NONE), also re-arms
// CurrentIntention::wake_at_millis to `now + kRejectedSuggestionBackoffMillis`
// (components.h) -- otherwise a hero with no CurrentIntention running (which
// is exactly what a rejected/no-op decision leaves behind) satisfies
// should_wake's "nothing running" clause every single tick, forever. No-op
// if `slot` names no hero.
//
// Determinism: this is live-only engine scheduling state (when to bother
// calling the brain again), never read back into anything the command log
// or replay observes -- replay never calls tick_wasm_brain/apply_intention
// at all (docs/design/intention-contract.html §6), so it never calls this
// either. Observable state (positions, hp, building occupancy) still comes
// entirely from logged Commands.
void note_think_outcome(BadlandsGame& game, uint32_t slot, bool adopted);

// Aborts slot's CurrentIntention if its kind currently equals `expected`:
// kind -> None, target_slot/arg/wake_at_millis reset, and an
// IntentionEnded(aborted) event pushed to the inbox. No-op otherwise --
// including when `slot` names no hero, or a replayed world where
// CurrentIntention.kind is always None (apply_intention never runs there),
// which is what makes this safe to call unconditionally from a replayed
// command handler. Used by command handlers (command.cpp) that decline to
// actually start what a suggestion asked for (Chat's partner-unavailable/
// out-of-radius declines, say): without this, the actor's CurrentIntention
// would keep reporting the never-started kind indefinitely, since nothing
// else in the contract ends an intention that never began.
void abort_intention(BadlandsGame& game, uint32_t slot, IntentionKind expected);

// Push one event into a hero's inbox (newest evicts oldest when full);
// no-op if `e` has no EventInbox (heroes only). Stamps at_millis/ttl_millis
// from game.world_millis/kInboxTtlMillis -- callers fill in kind/source_slot/
// param only.
void push_inbox_event(BadlandsGame& game, entt::entity e, InboxEvent ev);

// Per-tick maintenance (tick_world, sim.cpp): decrements every hero's inbox
// TTLs and drops expired entries, then detects each hero's CurrentIntention
// completion/abort (arrival, dead target, gone building) and writes
// IntentionEnded. See sim.cpp's call site comment for why it runs after
// movement/combat but before the death sweep.
void advance_intentions(BadlandsGame& game);

// The wake rule (pure over the components,
// docs/design/intention-contract.html §2): true when the hero has no active
// intention AND no backoff is armed (CurrentIntention::kind == None and
// wake_at_millis == 0 -- a fresh/never-consulted hero), its wake_at deadline
// has passed (an Idle/idle-hint deadline OR a rejection backoff, both ride
// the same field), or an inbox event was pushed since it last thought
// (EventInbox::last_pushed_seq > last_seen_seq, timestamp-free -- see that
// component's own comment on why a sequence counter, not `at_millis` vs
// CurrentIntention::last_think_millis, is the comparison). Sequenced this
// way deliberately: "no CurrentIntention" is NOT an unconditional
// short-circuit on its own, because note_think_outcome (above) can arm a
// backoff deadline while kind stays None (a rejected/no-op suggestion) --
// that hero must still wake early on a genuinely new event, so the event
// check has to run even during a None-kind backoff window.
bool should_wake(const BadlandsGame& game, entt::entity e);

}  // namespace badlands
