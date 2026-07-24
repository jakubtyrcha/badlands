// Core sim-world helpers that are not part of the badlands::Sim public surface:
// the BadlandsGame destructor plus the shared perception/bookkeeping free
// functions (report_bug / entity_for_slot / nearest_enemy) declared in
// game_state.h and used by the systems (brain / movement / heroes / sim).

#include "brain.h"  // complete badlands::BrainRuntime for BadlandsGame's unique_ptr dtor
#include "components.h"
#include "game_state.h"
#include "wasm_brain.h"  // complete badlands::WasmBrainRuntime, same reason as brain.h above

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

uint32_t slot_for_entity(const BadlandsGame& game, entt::entity e) {
    if (e == entt::null) {
        return UINT32_MAX;
    }
    const auto it = game.entity_slot.find(e);
    return it != game.entity_slot.end() ? it->second : UINT32_MAX;
}

void emit_event(BadlandsGame& game, const badlands::GameEvent& ev) {
    game.events.push_back(ev);
}

void emit_char_hit(BadlandsGame& game, uint32_t actor_slot, uint32_t target_slot,
                   float amount, float hp_after, glm::vec2 pos) {
    emit_event(game, GameEvent{.kind = GameEventKind::DamageDealt,
                               .actor_id = actor_slot,
                               .target_id = target_slot,
                               .target_kind = kEventTargetCharacter,
                               .amount = amount,
                               .x = pos.x,
                               .z = pos.y,
                               .at_millis = game.world_millis});
    if (hp_after <= 0.0f) {
        emit_event(game, GameEvent{.kind = GameEventKind::HeroDowned,
                                   .actor_id = actor_slot,
                                   .target_id = target_slot,
                                   .target_kind = kEventTargetCharacter,
                                   .amount = 0.0f,
                                   .x = pos.x,
                                   .z = pos.y,
                                   .at_millis = game.world_millis});
    }
}

void emit_building_hit(BadlandsGame& game, uint32_t actor_slot, uint32_t bid,
                       float amount, float hp_after, glm::vec2 center) {
    emit_event(game, GameEvent{.kind = GameEventKind::DamageDealt,
                               .actor_id = actor_slot,
                               .target_id = bid,
                               .target_kind = kEventTargetBuilding,
                               .amount = amount,
                               .x = center.x,
                               .z = center.y,
                               .at_millis = game.world_millis});
    if (hp_after <= 0.0f) {
        emit_event(game, GameEvent{.kind = GameEventKind::BuildingDestroyed,
                                   .actor_id = actor_slot,
                                   .target_id = bid,
                                   .target_kind = kEventTargetBuilding,
                                   .amount = 0.0f,
                                   .x = center.x,
                                   .z = center.y,
                                   .at_millis = game.world_millis});
    }
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
        // Critters (deer) are NEUTRAL wildlife -- never a team-combat target.
        // Only a hunter engages them, via the Hunt block's targeted Shoot.
        if (game.registry.all_of<CritterState>(e)) {
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
