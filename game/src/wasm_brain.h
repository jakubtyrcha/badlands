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
// own bl_spawn bookkeeping, not a per-entity coroutine like the noiser
// BrainState) -- so a BadlandsGame owns exactly one WasmBrainRuntime
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

// RAII owner of one loaded + instantiated brain wasm module.
struct WasmBrainRuntime {
    // Compiles + instantiates `wasm_bytes` against BL_ABI_VERSION
    // (brain_abi.h) with world_seed = 0 (world gen is currently
    // seedless/static -- see make_world). Registers a log callback that
    // forwards the guest's bl_log calls to spdlog with a "[brain]" prefix
    // (level 0/1/2 -> info/warn/error; anything else -> warn). A bh_load/
    // bh_instantiate failure is FATAL (routes through brain_fatal, stage
    // "load"/"instantiate") -- this never returns null; a caller (sim.cpp's
    // make_world) only reaches the next line with a live runtime, or the
    // process has already aborted.
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
// to bl_out_buf(). Rejects (returns std::nullopt -- MALFORMED, escalated to
// FATAL by the caller, tick_wasm_brain) an intention_kind outside
// [BL_INT_NONE, BL_INT_USE_SKILL], a non-finite point_x/point_z, an
// activity_label outside [0, kActivityCount), or a duration_millis/
// idle_hint_millis outside [0, INT32_MAX] (both narrow into a Command's
// int32_t param_b downstream -- command.cpp's enqueue_set_behavior -- so a
// value beyond that range would corrupt silently rather than merely being
// policy-noncompliant).
//
// BL_INT_USE_SKILL is well-formed but reserved (docs/design/
// intention-contract.html's vocab table): this is the ONE case decode logs a
// warning of its own (not a game-state side effect, so the function stays
// safely callable from tests without spdlog noise mattering) and decodes to
// IntentionKind::None ("nothing to adopt") rather than nullopt -- a
// malformed-wire FATAL would be the wrong response to a brain naming a
// reserved-but-recognized kind. Every other well-formed-but-infeasible
// suggestion (e.g. an unknown target_slot) is NOT rejected here at all --
// that is apply_intention's own warn+ignore validation, one layer up.
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
// apply_intention directly uses. script_intents is bumped only when
// apply_intention reports the suggestion was actually adopted.
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
