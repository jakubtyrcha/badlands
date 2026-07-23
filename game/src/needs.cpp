#include "needs.h"

#include "components.h"
#include "game_state.h"
#include "town_brain.h"  // badlands::Behavior

#include <algorithm>

#include <entt/entt.hpp>

namespace badlands {

void advance_needs(BadlandsGame& game) {
    const HeroFactors& hf = game.factors.hero;

    // Rates are read fresh every tick, so retuning them through
    // Sim::SetFactors takes effect immediately rather than at the next spawn.
    const float fatigue_drain = reserve_rate_per_tick(hf.fatigue_drain_hours);
    const float content_drain = reserve_rate_per_tick(hf.content_drain_hours);
    const float rest_fill = reserve_rate_per_tick(hf.rest_fill_hours);
    const float tavern_fill = reserve_rate_per_tick(hf.tavern_fill_hours);
    const float chat_fill = reserve_rate_per_tick(hf.chat_fill_hours);

    for (auto [e, sim] : game.registry.view<HeroSimulationState>().each()) {
        const auto* inside = game.registry.try_get<InsideBuilding>(e);
        const bool chatting = game.registry.all_of<ChattingState>(e);

        if (inside != nullptr) {
            // Asleep or being entertained: the reserve it went in for refills,
            // and nothing drains. A hero cannot get bored of the tavern -- an
            // acceptable simplification while `purpose` is the only thing that
            // distinguishes one stay from another.
            if (inside->purpose == static_cast<int32_t>(Behavior::VisitTavern)) {
                sim.content = std::min(1.0f, sim.content + tavern_fill);
            } else {
                sim.fatigue = std::min(1.0f, sim.fatigue + rest_fill);
            }
            continue;
        }

        // Out in the world: both reserves run down.
        sim.fatigue = std::max(0.0f, sim.fatigue - fatigue_drain);

        if (chatting) {
            // Company refills content, but only up to a ceiling -- and it is
            // still the weaker option, so it must not outpace the drain into
            // a net loss either. Below the ceiling it gains; at or above it,
            // it simply holds (rather than snapping down to the ceiling, which
            // would punish a hero for chatting after a good night out).
            if (sim.content < hf.chat_content_ceiling) {
                sim.content = std::min(hf.chat_content_ceiling, sim.content + chat_fill);
            }
        } else {
            sim.content = std::max(0.0f, sim.content - content_drain);
        }
    }
}

}  // namespace badlands
