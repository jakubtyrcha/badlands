// The C++ townfolk brain. The only townfolk today is the tax collector: it
// spawns at the Castle, makes a round of every building that owes tax (banking
// each into its carry), then returns to a Castle/Watchtower to deposit the carry
// into the player's gold and despawn. A sequential route with memory (the
// visited set) -- the control shape noiser could not express.
//
// WorldView-in / Commands-out, like the other brains. Runs from the mock-brain
// dispatch (sim.cpp) for Archetype::Townfolk; the tax collector is peaceful and
// never fights, so it skips the combat pre-empt.

#pragma once

#include <cstdint>

struct BadlandsGame;

namespace badlands {

void townfolk_think(BadlandsGame& game, uint32_t slot);

}  // namespace badlands
