// The C++ reference town brain: the decision logic for a hero's daily loop,
// expressed as WorldView-in / Commands-out (it enqueues Commands, never mutates
// the registry directly). It is the deterministic reference + fallback partner
// of the noiser hero brain (scripts/brains/hero.noiser) — a noiser failure just
// means that entity's commands come from here instead.
//
// Combat is a separate pre-empt handled by the C++ mock (game.cpp); town_think
// runs when there is no enemy. Thresholds/weights are policy placeholders.

#pragma once

#include <cstdint>
#include <span>

#include "badlands_sim.hpp"     // badlands::ActivityId (the shared id space)
#include "behaviours/blocks.h"  // badlands::ActivityDef

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

}  // namespace badlands
