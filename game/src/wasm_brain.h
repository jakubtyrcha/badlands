#pragma once

// Wasm hero brain (Task 4 of the wasm-brain feature): loads/ticks one
// compiled brain wasm module (via the brainhost C ABI,
// src/crates/brainhost/include/brainhost.h) and drives every
// BrainKind::Town entity's no-enemy tick through it, in place of town_think
// (game/src/town_brain.h) -- see
// docs/superpowers/specs/2026-07-23-wasm-brain-contract-design.md's Scope
// for why this is hero-only (critters/townfolk/monsters stay on the C++
// brains).
//
// One instance drives every hero slot (per-slot state lives in the guest's
// own bl_spawn bookkeeping, not a per-entity coroutine like the noiser
// BrainState) -- so a BadlandsGame owns exactly one WasmBrainRuntime
// (game_state.h's `wasm_brains`), not one per entity.

#include <brainhost.h>

#include "brain_abi.h"  // BlDecisionWire
#include "town_brain.h" // BrainDecision

#include <glm/glm.hpp>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

struct BadlandsGame;

namespace badlands {

// RAII owner of one loaded + instantiated brain wasm module.
struct WasmBrainRuntime {
    // Compiles + instantiates `wasm_bytes` against BL_ABI_VERSION
    // (brain_abi.h) with world_seed = 0 (world gen is currently
    // seedless/static -- see make_world). Registers a log callback that
    // forwards the guest's bl_log calls to spdlog with a "[brain]" prefix
    // (level 0/1/2 -> info/warn/error; anything else -> warn). Returns null
    // and fills `out_error` with bh_last_error()'s text on any bh_load/
    // bh_instantiate failure; the caller (sim.cpp's make_world) records the
    // bug and leaves wasm_brains null (mock drives every hero instead).
    static std::unique_ptr<WasmBrainRuntime> create(const uint8_t* wasm_bytes, size_t len,
                                                      std::string& out_error);

    WasmBrainRuntime() = default;
    ~WasmBrainRuntime();
    WasmBrainRuntime(const WasmBrainRuntime&) = delete;
    WasmBrainRuntime& operator=(const WasmBrainRuntime&) = delete;

    BhProgram* program = nullptr;
    BhInstance* instance = nullptr;
    // Slots bh_spawn has already been called for, by slot index (lazy: a
    // slot's first tick calls bh_spawn before its first bh_tick). v1 brains
    // are stateless, so a slot being reused (a dead hero's slot id is never
    // reassigned -- see BadlandsGame::slots -- but this stays cheap insurance)
    // without a matching bh_despawn is benign: there is no per-slot state on
    // the guest side worth leaking. Cleared to all-false whenever `instance`
    // is replaced (see tick_wasm_brain's reinstantiation path below), since a
    // fresh instance has never had bh_spawn called against it for any slot.
    std::vector<bool> spawned;
    // How many times `instance` has been (re)instantiated from `program`:
    // 1 after create(), +1 each time tick_wasm_brain drops a trapped/fuel-
    // exhausted instance and replaces it. Purely observational (tests use it
    // to prove a reinstantiation actually happened); nothing reads it to make
    // a decision.
    uint32_t instantiation_count = 0;
};

// The wire trust boundary: BlDecisionWire came back through bh_tick from
// guest-controlled memory, so its fields are untrusted input even though the
// guest is host-compiled -- a buggy or adversarial module can write anything
// to bl_out_buf(). Rejects (report_bug(game, "wasm_decode", ...), returns
// std::nullopt) a non-finite goal_x/goal_z or an activity_id outside
// [0, kActivityCount); same containment as a script/trap error -- no
// commands this tick, retried next tick against a fresh wire. Otherwise
// decodes `out` into a BrainDecision (self_pos stands in for goal_kind ==
// "none"). Not anonymous-namespace-local so tests can drive it directly with
// a synthetic BlDecisionWire (see wasm_brain_tests.cpp), the same way
// apply_brain_decision (town_brain.h) is unit-tested.
std::optional<BrainDecision> decode_decision(BadlandsGame& game, const BlDecisionWire& out,
                                              uint32_t slot, glm::vec2 self_pos);

// One tick for `slot`: observe (town_brain.h's observe_hero/weights_for,
// reused verbatim so perception is identical to the C++ reference path) ->
// pack into a BlViewWire (brain_abi.h) -> bh_tick -> on BH_OK, decode_decision
// the returned BlDecisionWire (above) and, if it passes the wire trust
// boundary, apply it (town_brain.h's apply_brain_decision, the same seam
// town_think's tail uses). On ANY failure (bh_spawn or bh_tick returning
// nonzero, or decode_decision rejecting the wire): report_bug(game,
// "wasm_spawn" | "wasm_tick" | "wasm_decode", ...) and no commands this tick
// -- the entity simply idles (its last decision stands) and the tick is
// retried next frame. There is no downgrade flag, unlike the noiser path --
// see the spec doc's Runtime section.
//
// BH_ERR_TRAP/BH_ERR_FUEL specifically also invalidate `instance` (per
// brainhost.h: "a BhInstance that has trapped ... is not reused"): this
// drops it and instantiates a fresh one from the retained `program`,
// clearing `spawned` so every slot re-registers lazily. If re-instantiation
// itself fails there is no usable wasm runtime left, so game.wasm_brains is
// reset to null -- the think loop's dispatch condition then falls back to
// mock for every hero, rather than every hero wedging into permanent idle
// forever against a runtime that can never tick again.
// BH_ERR_SCRIPT/BH_ERR_ARGS/BH_ERR_PANIC do NOT invalidate the instance
// (only TRAP/FUEL do, per the header) -- those just report and retry next
// tick against the same instance.
//
// Caller contract: game.wasm_brains must be non-null and `slot` must name a
// live BrainKind::Town entity (the sim.cpp think loop's dispatch already
// guarantees both before calling this). game.wasm_brains may be null AFTER
// this call returns (see above) -- the caller must not assume it survives.
void tick_wasm_brain(BadlandsGame& game, uint32_t slot);

}  // namespace badlands
