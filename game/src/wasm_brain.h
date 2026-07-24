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
//
// Failure policy (Task 7, user decision): ANY wasm-brain failure is FATAL --
// a wasm brain crash is a crash-and-error scenario, not something to paper
// over with a downgrade/retry path (the graceful containment this replaces
// was a workaround for noiser-era bugs and does not apply here; see
// docs/superpowers/specs/2026-07-23-wasm-brain-contract-design.md's Runtime
// section). wasm_brain.cpp's file-local `brain_fatal` helper (spdlog
// critical + std::abort) is the single place that enforces this -- see
// WasmBrainRuntime::create's and tick_wasm_brain's doc comments below for
// exactly which failures route through it. `BrainDesc{}`/no wasm bytes
// provided is unaffected: mock drives every hero, as configured.

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
    // slot's first tick calls bh_spawn before its first bh_tick). v1 brains
    // are stateless, so a slot being reused (a dead hero's slot id is never
    // reassigned -- see BadlandsGame::slots -- but this stays cheap insurance)
    // without a matching bh_despawn is benign: there is no per-slot state on
    // the guest side worth leaking.
    std::vector<bool> spawned;
};

// The wire trust boundary: BlDecisionWire came back through bh_tick from
// guest-controlled memory, so its fields are untrusted input even though the
// guest is host-compiled -- a buggy or adversarial module can write anything
// to bl_out_buf(). Rejects (returns std::nullopt) a non-finite goal_x/
// goal_z, an activity_id outside [0, kActivityCount), a pause_kind outside
// {0, 1, 2}, or a pause_duration_millis that violates its pause_kind's
// contract (kind==1: 0 < duration <= factors.hero.think_max_millis; kind==2:
// duration == 0). Otherwise decodes `out` into a BrainDecision (self_pos
// stands in for goal_kind == "none").
//
// A pure function -- no report_bug/logging side effect of its own, unlike
// the pre-Task-7 version. Under the fail-fast policy above a rejected wire
// is a brain bug, not something this function contains: it is the CALLER
// (tick_wasm_brain) that escalates a std::nullopt to brain_fatal. Kept out
// of the anonymous namespace so tests can drive it directly with a synthetic
// BlDecisionWire (see wasm_brain_tests.cpp), the same way apply_brain_decision
// (town_brain.h) is unit-tested.
std::optional<BrainDecision> decode_decision(BadlandsGame& game, const BlDecisionWire& out,
                                              uint32_t slot, glm::vec2 self_pos);

// One tick for `slot`: observe (town_brain.h's observe_hero/weights_for,
// reused verbatim so perception is identical to the C++ reference path) ->
// pack into a BlViewWire (brain_abi.h) -> bh_tick -> on BH_OK, decode_decision
// the returned BlDecisionWire (above) and, if it passes the wire trust
// boundary, apply it (town_brain.h's apply_brain_decision, the same seam
// town_think's tail uses). script_intents is bumped only when
// apply_brain_decision reports the decision was actually applied (a commit or
// a pause-START; a pause-CONTINUE enqueues nothing and is not counted).
//
// Fail-fast (see the policy note atop this header): bh_spawn/bh_tick
// returning nonzero, or decode_decision rejecting the wire, all route
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
