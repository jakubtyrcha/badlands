// The engine side of the intention contract
// (docs/design/intention-contract.html): validates a brain's suggestion,
// executes it through the existing command producers, tracks a hero's
// CurrentIntention to completion/abort, maintains the event inbox, and
// decides when a hero is worth waking.
//
// Wired in (the contract flip, task-3-brief.md): sim.cpp's think loop gates
// every wasm hero's tick_wasm_brain on should_wake, and wasm_brain.cpp
// decodes each wake's BlSuggestionWire into an Intention and adopts it via
// apply_intention. The C++ mock hero path (town_think) is untouched --
// should_wake/apply_intention are wasm-only this slice.

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
// direct command_queue pushes -- see town_brain.cpp's apply_brain_decision,
// the same pattern), stamps CurrentIntention, and logs the wake schedule via
// the SetBehavior duration (the same field the deliberation pause already
// rides on, so the schedule is replayable rather than recomputed). Invalid ->
// spdlog::warn + no execution + CurrentIntention left untouched. Returns
// whether the suggestion was adopted.
bool apply_intention(BadlandsGame& game, uint32_t slot, const Intention& intent);

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
// intention, an inbox event arrived since it last THOUGHT (CurrentIntention::
// last_think_millis, stamped by every apply_intention call -- not
// started_at_millis, which only advances on an adopted intention; see
// apply_intention's own comment), or its wake_at deadline has passed.
bool should_wake(const BadlandsGame& game, entt::entity e);

}  // namespace badlands
