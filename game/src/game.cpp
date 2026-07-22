// Core sim-world helpers that are not part of the badlands::Sim public surface:
// the BadlandsGame destructor plus the shared perception/bookkeeping free
// functions (report_bug / entity_for_slot / nearest_enemy) declared in
// game_state.h and used by the systems (brain / movement / heroes / sim).

#include "brain.h"  // complete badlands::BrainRuntime for BadlandsGame's unique_ptr dtor
#include "components.h"
#include "game_state.h"

#include <entt/entt.hpp>
#include <glm/glm.hpp>

#include <cstdio>
#include <string>

BadlandsGame::~BadlandsGame() = default;

namespace badlands {

void report_bug(BadlandsGame& game, const char* stage, const std::string& message) {
    std::fprintf(stderr, "[noiser-bug] %s: %s\n", stage, message.c_str());
    ++game.noiser_bugs;
}

entt::entity entity_for_slot(const BadlandsGame& game, int32_t slot) {
    if (slot < 0 || static_cast<size_t>(slot) >= game.slots.size()) {
        return entt::null;
    }
    entt::entity e = game.slots[slot];
    return game.registry.valid(e) ? e : entt::null;
}

entt::entity nearest_enemy(const BadlandsGame& game, entt::entity self) {
    const auto& self_pos = game.registry.get<Position>(self).pos;
    int32_t self_team = game.registry.get<Team>(self).id;

    entt::entity best = entt::null;
    float best_dist = 0.0f;
    for (auto [e, pos, team, health] :
         game.registry.view<const Position, const Team, const Health>(entt::exclude<InsideBuilding>)
             .each()) {
        if (team.id == self_team || health.hp <= 0.0f) {
            continue;
        }
        float dist = glm::distance(pos.pos, self_pos);
        if (best == entt::null || dist < best_dist) {
            best = e;
            best_dist = dist;
        }
    }
    return best;
}

}  // namespace badlands
