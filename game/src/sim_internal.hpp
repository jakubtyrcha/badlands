// The shared sim operations, extracted so they have a single implementation.
// badlands::Sim (sim.cpp) is a thin wrapper over these free functions, and the
// internal system tests (make_world / dispatch_into / characters_of, exercised
// directly by the movement/heroes/placement tests) call them too — all over the
// UNCHANGED internal world (struct BadlandsGame).

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
void buildings_of(const BadlandsGame&, std::vector<BuildingState>& out);
WorldState world_of(const BadlandsGame&);
SimStats stats_of(const BadlandsGame&);
std::vector<CommandRecord> command_log_of(const BadlandsGame&);
PlacementProbe probe_of(const BadlandsGame&, const PlacementDesc&, std::vector<GridTriangle>&);

}  // namespace badlands
