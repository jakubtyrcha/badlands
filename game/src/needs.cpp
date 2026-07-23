#include "needs.h"

#include "components.h"
#include "game_state.h"

#include <algorithm>

#include <entt/entt.hpp>

namespace badlands {

void advance_needs(BadlandsGame& game) {
    const HeroFactors& hf = game.factors.hero;
    // Per-tick decay from a per-second rate, to match how the factor reads.
    const float chat_decay =
        hf.chat_boredom_rate * static_cast<float>(kMillisPerTick) / 1000.0f;

    // Hidden (inside) heroes are excluded — they are resting/entertained.
    for (auto [e, sim] :
         game.registry.view<HeroSimulationState>(entt::exclude<InsideBuilding>).each()) {
        sim.fatigue = std::min(1.0f, sim.fatigue + kFatiguePerTick);

        if (game.registry.all_of<ChattingState>(e)) {
            // Company takes the edge off, but only down to a floor -- which is
            // what keeps the tavern worth the walk. Standing and talking is
            // still tiring, so fatigue above is unaffected.
            sim.boredom = std::max(hf.chat_boredom_floor, sim.boredom - chat_decay);
        } else {
            sim.boredom = std::min(1.0f, sim.boredom + kBoredomPerTick);
        }
    }
}

}  // namespace badlands
