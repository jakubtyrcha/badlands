#pragma once

// Wasm hero brain (v2, the intention contract): loads/ticks one compiled
// brain wasm module (via the brainhost C ABI, src/crates/brainhost/include/
// brainhost.h) and drives every BrainKind::Town entity's no-enemy,
// should_wake-gated tick through it -- the SOLE hero decision layer; the C++
// reference (town_think) it replaced is gone entirely (a hero with no wasm
// brain loaded simply idles, sim.cpp's mock_think) -- see
// docs/design/intention-contract.html for the full contract this implements.
//
// One instance drives every hero slot (per-slot state lives in the guest's
// own bl_spawn bookkeeping) -- so a BadlandsGame owns exactly one WasmBrainRuntime
// (game_state.h's `wasm_brains`), not one per entity.
//
// Failure policy (unchanged from v1, Task 7's decision): ANY wasm-brain
// failure is FATAL -- a wasm brain crash is a crash-and-error scenario, not
// something to paper over with a downgrade/retry path. wasm_brain.cpp's
// file-local `brain_fatal` helper (spdlog critical + std::abort) is the
// single place that enforces this -- see WasmBrainRuntime::create's and
// tick_wasm_brain's doc comments below for exactly which failures route
// through it. `BrainDesc{}`/no wasm bytes provided means every hero simply
// idles (no C++ decision layer left to fall back to, sim.cpp's mock_think).

#include <brainhost.h>

#include "brain_abi.h"        // BlSuggestionWire, BlViewWire
#include "hero_perception.h"  // observe_hero/weights_for/WorldView/ActivityWeights
#include "intention.h"        // Intention

#include <entt/entt.hpp>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

struct BadlandsGame;

namespace badlands {

// One bl_enqueue_action call (brain_abi.h's BL_ACT_* vocabulary), captured in
// the order the guest made it -- {kind, target_slot, arg} 1:1 with the
// BhActionFn callback's own parameters (src/crates/brainhost/include/
// brainhost.h), and 1:1 with intention.h's AgentAction (tick_wasm_brain
// converts one to the other before calling resolve_action). LIVE: hero.nim
// enqueues one BL_ACT_ATTACK per combat wake (its highest-damage ready,
// lock-legal, in-range pick), and tick_wasm_brain drains `pending_actions`
// through resolve_action every wake.
struct PendingAction {
    int32_t kind;
    uint32_t target_slot;
    int32_t arg;
};

// RAII owner of one loaded + instantiated brain wasm module.
struct WasmBrainRuntime {
    // Compiles + instantiates `wasm_bytes` against BL_ABI_VERSION
    // (brain_abi.h) with world_seed = 0 (world gen is currently
    // seedless/static -- see make_world). Registers a log callback that
    // forwards the guest's bl_log calls to spdlog with a "[brain]" prefix
    // (level 0/1/2 -> info/warn/error; anything else -> warn), plus an action
    // callback (forward_action, wasm_brain.cpp) that appends every
    // bl_enqueue_action call into this runtime's own pending_actions, in call
    // order (drained every wake by tick_wasm_brain through resolve_action,
    // game/src/intention.h -- Task 2's action resolver). A bh_load/
    // bh_instantiate failure is FATAL (routes through
    // brain_fatal, stage "load"/"instantiate") -- this never returns null; a
    // caller (sim.cpp's make_world) only reaches the next line with a live
    // runtime, or the process has already aborted.
    static std::unique_ptr<WasmBrainRuntime> create(const uint8_t* wasm_bytes, size_t len);

    WasmBrainRuntime() = default;
    ~WasmBrainRuntime();
    WasmBrainRuntime(const WasmBrainRuntime&) = delete;
    WasmBrainRuntime& operator=(const WasmBrainRuntime&) = delete;

    BhProgram* program = nullptr;
    BhInstance* instance = nullptr;
    // Slots bh_spawn has already been called for, by slot index (lazy: a
    // slot's first WAKE calls bh_spawn before its first bh_tick -- a hero
    // that never wakes before dying never spawns a guest at all, which is
    // fine: v2 brains are stateless the same way v1's were). A dead hero's
    // slot id is never reassigned (BadlandsGame::slots), but this stays
    // cheap insurance regardless.
    std::vector<bool> spawned;

    // This wake's bl_enqueue_action calls, in call order -- the sink the
    // BhActionFn callback registered at bh_instantiate appends into. The
    // callback's `user` pointer is fixed for the instance's whole lifetime
    // (bh_instantiate's own contract), so it points at this runtime rather
    // than at any one wake's stack frame; tick_wasm_brain clears this vector
    // before every bh_tick call, so by the time bh_tick returns it holds
    // exactly (only) that wake's enqueues, and drains it through
    // resolve_action (game/src/intention.h) right after, in the same order.
    std::vector<PendingAction> pending_actions;
};

// Packs one hero's BlViewWire from an already-observed WorldView
// (hero_perception.h's observe_hero, reused verbatim -- perception stays
// entirely host-side) plus its class weights row: self/suggest/factors 1:1 with WorldView/
// ActivityWeights (see wasm_brain.cpp's field-by-field doc comment),
// statuses assembled from whichever of ChattingState/MeleeLock/InsideBuilding
// `e` currently carries (advisory only this slice -- brain_abi.h's BL_ST_*
// doc), events copied 1:1 from `e`'s EventInbox, chars from EntityMemory
// (slot-ascending, for determinism). Exposed (like decode_suggestion below)
// so tests can inspect the packed wire directly without a live wasm module.
BlViewWire pack_view_wire(const BadlandsGame& game, entt::entity e, const WorldView& view,
                          const ActivityWeights& weights);

// The wire trust boundary: `out` came back through bh_tick from
// guest-controlled memory, so its fields are untrusted input even though the
// guest is host-compiled -- a buggy or adversarial module can write anything
// to bl_out_buf(). The boundary distinguishes two different kinds of "wrong"
// (Fix 5 of the review's fix wave; see wasm_brain.cpp's own doc comment on
// this function for the full reasoning):
//
//  - MALFORMED, corruption-shaped -> rejected (returns std::nullopt,
//    escalated to FATAL by the caller, tick_wasm_brain): a non-finite
//    point_x/point_z, or a duration_millis/idle_hint_millis outside
//    [0, INT32_MAX] (both narrow into a Command's int32_t param_b downstream
//    -- command.cpp's enqueue_set_behavior -- so a value beyond that range
//    would corrupt silently rather than merely being policy-noncompliant).
//    These shapes cannot come from a well-formed guest of any wire version.
//  - UNKNOWN VOCABULARY, forward-compat -> NOT rejected: an intention_kind
//    outside [BL_INT_NONE, BL_INT_USE_SKILL] decodes to IntentionKind::None
//    (warn once); an activity_label outside [-1, kActivityCount) clamps to
//    -1 (activity_label's own "none" sentinel -- warn once). Both are
//    exactly the shape a newer guest talking to an OLDER host produces (a
//    value this build has not learned about yet), so treating them as fatal
//    would make every host upgrade a breaking change for brains built
//    against a newer vocabulary.
//
// BL_INT_USE_SKILL is a third case: well-formed but reserved (docs/design/
// intention-contract.html's vocab table) -- decodes to IntentionKind::None
// with its own warning, same outcome as an unknown kind. Every other
// well-formed-but-infeasible suggestion (e.g. an unknown target_slot) is NOT
// rejected here at all -- that is apply_intention's own warn+ignore
// validation, one layer up.
//
// Kept out of the anonymous namespace so tests can drive it directly with a
// synthetic BlSuggestionWire (see wasm_brain_tests.cpp), the same way
// apply_intention (intention.h) is unit-tested.
std::optional<Intention> decode_suggestion(const BlSuggestionWire& out, uint32_t slot);

// One WAKE for `slot` (caller contract: sim.cpp's think loop already gated
// this call on should_wake -- tick_wasm_brain does not re-check it): observe
// (hero_perception.h's observe_hero/weights_for) -> pack into a BlViewWire
// (brain_abi.h; statuses from Chatting/MeleeLock/InsideBuilding, events
// copied from EventInbox) -> bh_tick -> on BH_OK, decode_suggestion the
// returned BlSuggestionWire (above) and, if it passes the wire trust
// boundary, apply_intention (intention.h) -- the same seam a test driving
// apply_intention directly uses -- then note_think_outcome, then drain this
// wake's pending_actions (the callback-filled vector, above) through
// resolve_action (intention.h), in call order.
//
// Fail-fast (see the policy note atop this header): bh_spawn/bh_tick
// returning nonzero, or decode_suggestion rejecting the wire, all route
// through wasm_brain.cpp's file-local brain_fatal (spdlog::critical with the
// stage/slot/bh_last_error() text, then std::abort()) -- there is no
// downgrade, retry-next-tick, or reinstantiation path left; a wasm brain
// failure crashes the process by design.
//
// Caller contract: game.wasm_brains must be non-null and `slot` must name a
// live BrainKind::Town entity (the sim.cpp think loop's dispatch already
// guarantees both before calling this).
void tick_wasm_brain(BadlandsGame& game, uint32_t slot);

}  // namespace badlands
