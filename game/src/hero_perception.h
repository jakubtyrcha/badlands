// Hero perception -- the ONLY place a hero brain reads the registry/
// placement. This is what remains of game/src/town_brain.{h,cpp} after the
// C++ hero decision layer (town_think, hero_activities, BrainDecision,
// apply_brain_decision) was deleted: perception survives because the wasm
// hero brain (game/src/wasm_brain.cpp, the sole hero decision-maker now)
// still needs it, unchanged, so a hero decides differently only for a reason
// its own logic produces -- never because perception itself differs.

#pragma once

#include <cstdint>

#include <entt/entt.hpp>
#include <glm/glm.hpp>

#include "badlands_sim.hpp"          // badlands::ActivityWeights
#include "behaviours/world_view.h"   // badlands::WorldView

struct BadlandsGame;

namespace badlands {

// Builds the hero's perception (the ONLY place a hero brain reads the
// registry/placement -- the wasm brain, via the wire, sees only the returned
// WorldView). Exposed so wasm_brain.cpp reuses it verbatim.
WorldView observe_hero(const BadlandsGame& game, uint32_t slot, entt::entity e,
                       const ActivityWeights& weights);

// The actor's preference table: which class this hero is. Exposed so
// wasm_brain.cpp packs the same weights row it sends over the wire from the
// identical lookup, rather than a copy that could drift.
const ActivityWeights& weights_for(const BadlandsGame& game, entt::entity e);

}  // namespace badlands
