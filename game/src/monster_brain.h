// The C++ monster brain. The only monster today is the rat: it spawns from the
// Sewer and attacks the nearest hostile unit, falling back to gnawing the
// nearest targettable building (Castle/House) when no unit is in reach.
//
// The UNIT case reuses the shared combat pre-empt (sim.cpp) -- so monster_think
// only handles the building fallback, invoked when no enemy unit exists. Its
// MoveTo + AttackBuilding go through the command layer, so building combat is
// logged and replayable (unlike the unit pre-empt, which mutates directly).

#pragma once

#include <cstdint>

struct BadlandsGame;

namespace badlands {

// No-enemy-unit case: walk to the nearest targettable building and, once in
// range and off cooldown, enqueue an AttackBuilding swing.
void monster_think(BadlandsGame& game, uint32_t slot);

}  // namespace badlands
