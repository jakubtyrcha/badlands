#include "badlands_game.h"

#include "brain.h"
#include "components.h"
#include "game_state.h"
#include "heroes.h"
#include "movement.h"
#include "placement.h"
#include "sim_internal.hpp"

#include <entt/entt.hpp>
#include <glm/glm.hpp>

#include <cstdio>
#include <limits>
#include <string>
#include <vector>

// Discriminant pins for the generic action trigger + building kinds. The Rust
// mirrors in src/game_ffi.rs assert the same literals (a #[test]); together
// they keep the C enum and its #[repr(i32)] mirror in lockstep.
static_assert(GAME_ACTION_PLACE_BUILDING == 0, "GameActionKind order");
static_assert(GAME_ACTION_RECRUIT_HERO == 1, "GameActionKind order");
static_assert(GAME_ACTION_DESTROY_BUILDING == 2, "GameActionKind order");
static_assert(GAME_ACTION_KIND_COUNT == 3, "GameActionKind count");
static_assert(GAME_BUILDING_CASTLE == 0, "GameBuildingKind order");
static_assert(GAME_BUILDING_KIND_COUNT == 10, "GameBuildingKind count");

using namespace badlands;

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

// The game_* C ABI now forwards to the badlands:: free functions extracted into
// sim.cpp (make_world / tick_world / *_of snapshots / spawn_into / dispatch_into
// / reload_script) plus the placement.cpp helpers. Signatures + behavior are
// unchanged; badlands::Sim shares the same implementation.
extern "C" {

BadlandsGame* game_create(const char* brain_script_source) {
    return badlands::make_world(brain_script_source).release();
}

GameCharacterDesc game_desc_mercenary(float pos_x, float pos_z) {
    const badlands::CharacterDesc d = badlands::MercenaryDesc(pos_x, pos_z);
    return GameCharacterDesc{
        .pos_x = d.pos_x,
        .pos_z = d.pos_z,
        .team = d.team,
        .hp = d.hp,
        .move_speed = d.move_speed,
        .attack_range = d.attack_range,
        .attack_damage = d.attack_damage,
        .attack_cooldown = d.attack_cooldown,
        .size_x = d.size_x,
        .size_y = d.size_y,
        .size_z = d.size_z,
        .color_r = d.color_r,
        .color_g = d.color_g,
        .color_b = d.color_b,
    };
}

GameCharacterDesc game_desc_goblin(float pos_x, float pos_z) {
    const badlands::CharacterDesc d = badlands::GoblinDesc(pos_x, pos_z);
    return GameCharacterDesc{
        .pos_x = d.pos_x,
        .pos_z = d.pos_z,
        .team = d.team,
        .hp = d.hp,
        .move_speed = d.move_speed,
        .attack_range = d.attack_range,
        .attack_damage = d.attack_damage,
        .attack_cooldown = d.attack_cooldown,
        .size_x = d.size_x,
        .size_y = d.size_y,
        .size_z = d.size_z,
        .color_r = d.color_r,
        .color_g = d.color_g,
        .color_b = d.color_b,
    };
}

void game_destroy(BadlandsGame* game) {
    delete game;
}

uint32_t game_spawn(BadlandsGame* game, const GameCharacterDesc* desc) {
    const badlands::CharacterDesc d{
        .pos_x = desc->pos_x,
        .pos_z = desc->pos_z,
        .team = desc->team,
        .hp = desc->hp,
        .move_speed = desc->move_speed,
        .attack_range = desc->attack_range,
        .attack_damage = desc->attack_damage,
        .attack_cooldown = desc->attack_cooldown,
        .size_x = desc->size_x,
        .size_y = desc->size_y,
        .size_z = desc->size_z,
        .color_r = desc->color_r,
        .color_g = desc->color_g,
        .color_b = desc->color_b,
    };
    return badlands::spawn_into(*game, d);
}

void game_tick(BadlandsGame* game, float dt) {
    badlands::tick_world(*game, dt);
}

uint32_t game_state(const BadlandsGame* game, GameCharacterState* out, uint32_t cap) {
    const auto rows = badlands::characters_of(*game);
    for (uint32_t i = 0; i < rows.size() && i < cap; ++i) {
        const badlands::CharacterState& r = rows[i];
        out[i] = GameCharacterState{
            .id = r.id,
            .team = r.team,
            .pos_x = r.pos_x,
            .pos_z = r.pos_z,
            .hp = r.hp,
            .max_hp = r.max_hp,
            .size_x = r.size_x,
            .size_y = r.size_y,
            .size_z = r.size_z,
            .color_r = r.color_r,
            .color_g = r.color_g,
            .color_b = r.color_b,
            .home_building_id = r.home_building_id,
            .inside_building_id = r.inside_building_id,
        };
    }
    return static_cast<uint32_t>(rows.size());
}

void game_stats(const BadlandsGame* game, GameStats* out) {
    const badlands::SimStats s = badlands::stats_of(*game);
    *out = GameStats{
        .ticks = s.ticks,
        .script_intents = s.script_intents,
        .noiser_bugs = s.noiser_bugs,
    };
}

bool game_reload_script(BadlandsGame* game, const char* source) {
    if (source == nullptr) {
        return false;
    }
    return badlands::reload_script(*game, std::string(source));
}

int64_t game_dispatch(BadlandsGame* game, const GameAction* action) {
    if (game == nullptr || action == nullptr) {
        return -1;
    }
    const badlands::Action a{
        .kind = static_cast<badlands::ActionKind>(action->kind),
        .target_id = action->target_id,
        .world_x = action->world_x,
        .world_z = action->world_z,
        .param_a = action->param_a,
        .param_b = action->param_b,
    };
    return badlands::dispatch_into(*game, a);
}

void game_set_pathfinder(BadlandsGame* game, const GamePathfinder* pathfinder) {
    if (game == nullptr) {
        return;
    }
    game->pathfinder = pathfinder ? *pathfinder : GamePathfinder{};
    if (game->pathfinder.add_obstacle == nullptr) {
        return;
    }
    // Back-fill the provider with every alive building already placed (the
    // prebuilt castle, and any placed before registration).
    const auto& buildings = game->placement.buildings;
    for (uint32_t id = 0; id < buildings.size(); ++id) {
        if (buildings[id].alive) {
            badlands::notify_obstacle_added(*game, id);
        }
    }
}

}  // extern "C"
