// The C++ reference town brain: the decision logic for a hero's daily loop,
// expressed as WorldView-in / Commands-out (it enqueues Commands, never mutates
// the registry directly). It is the parity reference for the Nim/WASM hero
// brain (scripts/brains/nim/, see game/src/wasm_brain.h) and the fallback when
// no wasm brain is loaded or it fails; the noiser hero brain path
// (scripts/brains/hero.noiser) is dormant.
//
// Combat is a separate pre-empt handled by the C++ mock (game.cpp); town_think
// runs when there is no enemy. Thresholds/weights are policy placeholders.

#pragma once

#include <cstdint>
#include <optional>
#include <span>

#include <entt/entt.hpp>
#include <glm/glm.hpp>

#include "badlands_sim.hpp"     // badlands::ActivityId (the shared id space)
#include "behaviours/blocks.h"  // badlands::ActivityDef
#include "behaviours/world_view.h"  // badlands::WorldView
#include "command.h"                // badlands::Command

struct BadlandsGame;

namespace badlands {

// `badlands::Behavior` (the chosen activity for one tick, recorded in
// HeroSimulationState for inspection) is an alias of the public ActivityId,
// declared in behaviours/world_view.h so the whole behaviour layer shares it.

// Decide + enqueue commands for the hero in `slot` (no enemy case). Reads its
// needs/time/world; writes HeroSimulationState.behavior; enqueues a MoveTo plus
// an optional enter/buy command that the handler gates on arrival.
void town_think(BadlandsGame& game, uint32_t slot);

// The SHIPPING hero activity table, shared by every class (a class differs only
// in its weights). Exposed so tests and tools exercise the real table rather
// than a copy that can silently drift out of step with it.
std::span<const ActivityDef> hero_activities();

// Builds the hero's perception (the ONLY place a hero brain reads the
// registry/placement -- blocks and the wasm brain alike see only the returned
// WorldView). Shared verbatim by town_think and the wasm path
// (game/src/wasm_brain.cpp) so perception is identical on both: a hero must
// not decide differently just because its decisions come from wasm.
WorldView observe_hero(const BadlandsGame& game, uint32_t slot, entt::entity e,
                       const ActivityWeights& weights);

// The actor's preference table: which class this hero is. Shared by
// town_think and the wasm path, same reason as observe_hero above.
const ActivityWeights& weights_for(const BadlandsGame& game, entt::entity e);

// Host-native form of BlDecisionWire (game/src/brain_abi.h): what a brain --
// the C++ reference (town_think) or a wasm module (wasm_brain.cpp) -- decided
// for one hero this tick. `goal`/`follow_up`/`follow_up_on_arrival` mirror
// BehaviourResult; `pause`/`pause_duration_millis` mirror ThinkDecision
// (behaviours/deliberation.h) -- the two halves town_think's tail used to
// combine inline.
struct BrainDecision {
    ActivityId activity = ActivityId::Idle;
    glm::vec2 goal{0.0f, 0.0f};
    std::optional<Command> follow_up;
    bool follow_up_on_arrival = true;
    // true -> hold position and deliberate instead of committing. With
    // pause_duration_millis > 0 this STARTS a pause (worth logging); with
    // pause_duration_millis == 0 an already-running pause simply continues
    // (not a new decision -- must not enter the command log).
    bool pause = false;
    int64_t pause_duration_millis = 0;
};

// The shared decision-apply seam: turns one `BrainDecision` into Commands via
// the same edge-triggered enqueue_* producers every brain uses (command.h),
// so a live run and its replay -- and the C++ and wasm brains -- all speak the
// one command log. `self_pos` is the hero's current position (WorldView::pos),
// passed separately rather than folded into BrainDecision because it is also
// needed for the follow_up_on_arrival distance gate and the pause hold-point,
// neither of which the brain itself decides.
void apply_brain_decision(BadlandsGame& game, uint32_t slot, glm::vec2 self_pos,
                          const BrainDecision& decision);

}  // namespace badlands
