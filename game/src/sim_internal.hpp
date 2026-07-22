// The shared sim operations, extracted so both badlands::Sim (sim.cpp) and the
// game_* C ABI (game.cpp) call one implementation. This is what makes the
// migration additive: the C ABI and Sim are thin wrappers over these free
// functions over the UNCHANGED internal world (struct BadlandsGame).

#pragma once

#include <memory>
#include <string>
#include <vector>

#include "badlands_sim.hpp"
#include "game_state.h"  // struct BadlandsGame (UNCHANGED)

namespace badlands {

std::unique_ptr<BadlandsGame> make_world(const char* brain_script_source);
void tick_world(BadlandsGame&, float dt);
uint32_t spawn_into(BadlandsGame&, const CharacterDesc&);
int64_t dispatch_into(BadlandsGame&, const Action&);
bool reload_script(BadlandsGame&, const std::string&);
std::vector<CharacterState> characters_of(const BadlandsGame&);
std::vector<BuildingState> buildings_of(const BadlandsGame&);
WorldState world_of(const BadlandsGame&);
SimStats stats_of(const BadlandsGame&);
PlacementProbe probe_of(const BadlandsGame&, const PlacementDesc&, std::vector<GridTriangle>&);

}  // namespace badlands
