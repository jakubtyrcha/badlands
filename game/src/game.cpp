#include "badlands_game.h"

#include "brain.h"
#include "components.h"
#include "game_state.h"
#include "placement.h"

#include <entt/entt.hpp>
#include <glm/glm.hpp>

#include <cstdio>
#include <mutex>
#include <string>
#include <vector>

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
         game.registry.view<const Position, const Team, const Health>().each()) {
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

namespace {

// Reference behavior, and the fallback whenever an entity has no (or a
// downgraded) script brain: close in, swing when ready.
Intent mock_think(const BadlandsGame& game, entt::entity self) {
    entt::entity target = nearest_enemy(game, self);
    if (target == entt::null) {
        return {.kind = 0, .dir = {0.0f, 0.0f}};
    }
    glm::vec2 self_pos = game.registry.get<Position>(self).pos;
    glm::vec2 target_pos = game.registry.get<Position>(target).pos;
    const auto& stats = game.registry.get<Stats>(self);
    float dist = glm::distance(self_pos, target_pos);
    if (dist <= stats.attack_range) {
        bool ready = game.registry.get<CooldownTimer>(self).remaining <= 0.0f;
        return {.kind = ready ? 2 : 0, .dir = {0.0f, 0.0f}};
    }
    return {.kind = 1, .dir = target_pos - self_pos};
}

}  // namespace
}  // namespace badlands

extern "C" {

BadlandsGame* game_create(const char* brain_script_source) {
    // One-time noiser runtime configuration. The profiling switch is
    // thread-local and defaults to ON, and upstream has no public API for it
    // yet (docs/noiser-feedback.md #3) — this is the only detail:: callsite.
    // All game_create/game_tick calls happen on the same (main) thread.
    static std::once_flag noiser_configured;
    std::call_once(noiser_configured,
                   [] { sampo::noiser::detail::SetHostCallProfiling(false); });

    auto* game = new BadlandsGame();
    if (brain_script_source != nullptr) {
        std::string error;
        game->brains = BrainRuntime::create(*game, brain_script_source, error);
        if (!game->brains) {
            report_bug(*game, "compile", error);
        }
    }
    // The colony starts with only the castle, prebuilt at the origin. Not a
    // player placement: it seeds no urban sprawl.
    place_building(*game, GamePlacementDesc{GAME_BUILDING_CASTLE, 0, 0.0f, 0.0f},
                   /*player=*/false);
    return game;
}

GameCharacterDesc game_desc_mercenary(float pos_x, float pos_z) {
    return GameCharacterDesc{
        .pos_x = pos_x,
        .pos_z = pos_z,
        .team = 0,
        .hp = 30.0f,
        .move_speed = 2.5f,
        .attack_range = 1.5f,
        .attack_damage = 4.0f,
        .attack_cooldown = 1.0f,
        .size_x = 1.0f,
        .size_y = 1.8f,
        .size_z = 1.0f,
        .color_r = 0.35f,
        .color_g = 0.45f,
        .color_b = 0.80f,
    };
}

GameCharacterDesc game_desc_goblin(float pos_x, float pos_z) {
    return GameCharacterDesc{
        .pos_x = pos_x,
        .pos_z = pos_z,
        .team = 1,
        .hp = 18.0f,
        .move_speed = 3.0f,
        .attack_range = 1.2f,
        .attack_damage = 2.0f,
        .attack_cooldown = 0.8f,
        .size_x = 0.8f,
        .size_y = 1.2f,
        .size_z = 0.8f,
        .color_r = 0.30f,
        .color_g = 0.75f,
        .color_b = 0.35f,
    };
}

void game_destroy(BadlandsGame* game) {
    delete game;
}

uint32_t game_spawn(BadlandsGame* game, const GameCharacterDesc* desc) {
    entt::entity e = game->registry.create();
    uint32_t slot = static_cast<uint32_t>(game->slots.size());
    game->slots.push_back(e);

    game->registry.emplace<Position>(e, glm::vec2{desc->pos_x, desc->pos_z});
    game->registry.emplace<Team>(e, desc->team);
    game->registry.emplace<Health>(e, desc->hp, desc->hp);
    game->registry.emplace<Stats>(e, desc->move_speed, desc->attack_range, desc->attack_damage,
                                  desc->attack_cooldown);
    game->registry.emplace<CooldownTimer>(e, 0.0f);
    game->registry.emplace<RenderShape>(e, glm::vec3{desc->size_x, desc->size_y, desc->size_z},
                                        glm::vec3{desc->color_r, desc->color_g, desc->color_b});
    game->registry.emplace<Intent>(e, 0, glm::vec2{0.0f, 0.0f});
    game->registry.emplace<Brain>(
        e, game->brains ? spawn_brain(*game->brains, slot) : nullptr);
    return slot;
}

void game_tick(BadlandsGame* game, float dt) {
    auto& registry = game->registry;

    for (auto [e, cooldown] : registry.view<CooldownTimer>().each()) {
        cooldown.remaining = std::max(0.0f, cooldown.remaining - dt);
    }

    // Brains: each living entity's coroutine resumes once; intents arrive via
    // host calls. Any failure permanently downgrades that entity to the mock.
    for (auto [e, intent] : registry.view<Intent>().each()) {
        intent = {.kind = 0, .dir = {0.0f, 0.0f}};
    }
    for (size_t slot = 0; slot < game->slots.size(); ++slot) {
        entt::entity e = game->slots[slot];
        if (!registry.valid(e)) {
            continue;
        }
        auto& brain = registry.get<Brain>(e);
        bool scripted = brain.state && !brain.state->downgraded && game->brains;
        if (scripted && !resume_brain(*game, static_cast<uint32_t>(slot), *brain.state)) {
            brain.state->downgraded = true;
            scripted = false;
        }
        if (!scripted) {
            registry.get<Intent>(e) = mock_think(*game, e);
        }
    }

    // Movement.
    for (auto [e, intent, pos, stats] : registry.view<const Intent, Position, const Stats>().each()) {
        if (intent.kind != 1) {
            continue;
        }
        float len = glm::length(intent.dir);
        if (len > 0.0f) {
            pos.pos += intent.dir / len * stats.move_speed * dt;
        }
    }

    // Combat: the engine is authoritative — an attack intent only lands when
    // the nearest enemy is in range and the cooldown has elapsed.
    for (auto [e, intent, pos, stats, cooldown] :
         registry.view<const Intent, const Position, const Stats, CooldownTimer>().each()) {
        if (intent.kind != 2 || cooldown.remaining > 0.0f) {
            continue;
        }
        entt::entity target = nearest_enemy(*game, e);
        if (target == entt::null) {
            continue;
        }
        float dist = glm::distance(pos.pos, registry.get<Position>(target).pos);
        if (dist <= stats.attack_range * 1.05f) {
            registry.get<Health>(target).hp -= stats.attack_damage;
            cooldown.remaining = stats.attack_cooldown;
        }
    }

    // Death.
    std::vector<entt::entity> dead;
    for (auto [e, health] : registry.view<const Health>().each()) {
        if (health.hp <= 0.0f) {
            dead.push_back(e);
        }
    }
    for (entt::entity e : dead) {
        registry.destroy(e);
    }

    ++game->ticks;
}

uint32_t game_state(const BadlandsGame* game, GameCharacterState* out, uint32_t cap) {
    uint32_t total = 0;
    for (uint32_t slot = 0; slot < game->slots.size(); ++slot) {
        entt::entity e = game->slots[slot];
        if (!game->registry.valid(e)) {
            continue;
        }
        if (total < cap) {
            const auto& pos = game->registry.get<Position>(e);
            const auto& health = game->registry.get<Health>(e);
            const auto& shape = game->registry.get<RenderShape>(e);
            out[total] = GameCharacterState{
                .id = slot,
                .team = game->registry.get<Team>(e).id,
                .pos_x = pos.pos.x,
                .pos_z = pos.pos.y,
                .hp = health.hp,
                .max_hp = health.max_hp,
                .size_x = shape.size.x,
                .size_y = shape.size.y,
                .size_z = shape.size.z,
                .color_r = shape.color.x,
                .color_g = shape.color.y,
                .color_b = shape.color.z,
            };
        }
        ++total;
    }
    return total;
}

void game_stats(const BadlandsGame* game, GameStats* out) {
    *out = GameStats{
        .ticks = game->ticks,
        .script_intents = game->script_intents,
        .noiser_bugs = game->noiser_bugs,
    };
}

bool game_reload_script(BadlandsGame* game, const char* source) {
    if (source == nullptr) {
        return false;
    }
    std::string error;
    auto fresh = BrainRuntime::create(*game, source, error);
    if (!fresh) {
        // Keep-last-good: the running program stays in place.
        std::fprintf(stderr, "[noiser] reload failed, keeping last-good: %s\n", error.c_str());
        return false;
    }
    game->brains = std::move(fresh);
    game->noiser_bugs = 0;
    game->script_intents = 0;
    for (uint32_t slot = 0; slot < game->slots.size(); ++slot) {
        entt::entity e = game->slots[slot];
        if (game->registry.valid(e)) {
            game->registry.get<Brain>(e).state = spawn_brain(*game->brains, slot);
        }
    }
    return true;
}

}  // extern "C"
