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
