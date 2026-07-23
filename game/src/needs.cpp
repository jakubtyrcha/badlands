#include "needs.h"

#include "components.h"
#include "game_state.h"

#include <algorithm>

#include <entt/entt.hpp>

namespace badlands {

void advance_needs(BadlandsGame& game) {
    // Hidden (inside) heroes are excluded — they are resting/entertained.
    for (auto [e, sim] :
         game.registry.view<HeroSimulationState>(entt::exclude<InsideBuilding>).each()) {
        sim.fatigue = std::min(1.0f, sim.fatigue + kFatiguePerTick);
        sim.boredom = std::min(1.0f, sim.boredom + kBoredomPerTick);
    }
}

}  // namespace badlands
