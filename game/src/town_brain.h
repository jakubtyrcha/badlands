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

struct BadlandsGame;

namespace badlands {

// The chosen behaviour for one tick (also recorded in HeroSimulationState for
// inspection). Shared id space with the noiser brain's behaviour report.
enum class Behavior : int32_t {
    Idle = 0,
    Roam,
    Buy,
    GoHome,
    VisitTavern,
    Combat,
    Graze,
    VisitTax,
    Deposit,
};

// Decide + enqueue commands for the hero in `slot` (no enemy case). Reads its
// needs/time/world; writes HeroSimulationState.behavior; enqueues a MoveTo plus
// an optional enter/buy command that the handler gates on arrival.
void town_think(BadlandsGame& game, uint32_t slot);

}  // namespace badlands
