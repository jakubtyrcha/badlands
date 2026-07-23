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

#include <cstddef>
#include <cstdint>
#include <memory>
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
    // the guest side worth leaking.
    std::vector<bool> spawned;
};

// One tick for `slot`: observe (town_brain.h's observe_hero/weights_for,
// reused verbatim so perception is identical to the C++ reference path) ->
// pack into a BlViewWire (brain_abi.h) -> bh_tick -> on BH_OK, decode the
// returned BlDecisionWire into a BrainDecision and apply it
// (town_brain.h's apply_brain_decision, the same seam town_think's tail
// uses). On ANY failure (bh_spawn or bh_tick returning nonzero, including a
// trap or fuel exhaustion): report_bug(game, "wasm_tick", ...) with
// bh_last_error()'s text and no commands this tick -- the entity simply
// idles (its last decision stands) and the tick is retried next frame.
// There is no downgrade flag, unlike the noiser path -- see the spec doc's
// Runtime section.
//
// Caller contract: game.wasm_brains must be non-null and `slot` must name a
// live BrainKind::Town entity (the sim.cpp think loop's dispatch already
// guarantees both before calling this).
void tick_wasm_brain(BadlandsGame& game, uint32_t slot);

}  // namespace badlands
