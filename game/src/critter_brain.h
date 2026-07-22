// The C++ critter brain: a deer's reactive daily loop, expressed WorldView-in /
// Commands-out like the town brain. Deer graze and wander around their spawn
// range in Forest/Plains and bolt from any non-critter that comes within sight.
//
// Combat is not their concern; critter_think runs from the mock-brain dispatch
// (sim.cpp) for entities spawned with Archetype::Critter. Thresholds are policy
// in SimFactors::critter (assets/creatures/factors.json).

#pragma once

#include <cstdint>

struct BadlandsGame;

namespace badlands {

// Decide + enqueue commands for the critter in `slot`. Priority: Flee a nearby
// threat, else graze/wander biome-filtered terrain. Writes behaviour for
// inspection via the command layer, like every other decision.
void critter_think(BadlandsGame& game, uint32_t slot);

}  // namespace badlands
