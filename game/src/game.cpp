#include "badlands_game.h"

#include "brain.h"
#include "components.h"
#include "game_state.h"
#include "heroes.h"
#include "movement.h"
#include "needs.h"
#include "placement.h"
#include "town_brain.h"

#include <entt/entt.hpp>
#include <glm/glm.hpp>

#include <cstdio>
#include <cstring>
#include <limits>
#include <mutex>
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

namespace {

// Reference behavior, and the fallback whenever an entity has no (or a
// downgraded) script brain. Combat pre-empt: set a durable MoveTarget on the
// nearest enemy and swing when in range (the movement pipeline walks the
// MoveTarget; the combat pass re-validates the attack Intent authoritatively).
// With no enemy, delegate to the C++ town brain (needs/day-night loop).
void mock_think(BadlandsGame& game, entt::entity self, uint32_t slot) {
    auto& reg = game.registry;
    MoveTarget& mt = reg.get<MoveTarget>(self);
    entt::entity target = nearest_enemy(game, self);
    if (target == entt::null) {
        // STOPGAP (remove with the entity-archetype slice): only townsfolk run
        // the town loop. Without this every enemy/critter spawn shops at the
        // player's apothecary, because spawn_entity emplaces the hero
        // components on EVERY entity. The archetype dispatch replaces this.
        if (reg.all_of<HeroSimulationState>(self)) {
            town_think(game, slot);
        } else {
            mt.kind = MoveTarget::Kind::None;
        }
        return;
    }
    const Stats& stats = reg.get<Stats>(self);
    mt.kind = MoveTarget::Kind::Entity;
    mt.entity = target;
    mt.building = UINT32_MAX;
    mt.stop_distance = stats.attack_range;

    glm::vec2 self_pos = reg.get<Position>(self).pos;
    glm::vec2 target_pos = reg.get<Position>(target).pos;
    if (glm::distance(self_pos, target_pos) <= stats.attack_range &&
        reg.get<CooldownTimer>(self).remaining <= 0.0f) {
        reg.get<Intent>(self).kind = 2;  // swing (Intent was reset to idle this tick)
    }
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
    // Plain (home-less) spawn; heroes::spawn_entity emplaces the full component
    // set shared with recruit.
    return badlands::spawn_entity(*game, *desc, -1);
}

void game_tick(BadlandsGame* game, float dt) {
    auto& registry = game->registry;

    // Replay: commands stamped at the CURRENT time were originally applied
    // before this tick (player dispatches between ticks), so they land first.
    badlands::apply_replay_commands(*game);

    // Day/night clock: integer ms, fixed compile-time increment (deterministic).
    game->world_millis += kMillisPerTick;

    for (auto [e, cooldown] : registry.view<CooldownTimer>().each()) {
        cooldown.remaining = std::max(0.0f, cooldown.remaining - dt);
    }

    // Reappear hidden heroes whose stay has elapsed, before they think again.
    badlands::advance_inside_timers(*game, dt);

    // Needs system: fatigue/boredom rise for active (non-hidden) heroes, so
    // brains this tick see fresh values.
    badlands::advance_needs(*game);

    // Brains: each living entity's coroutine resumes once; intents arrive via
    // host calls. Any failure permanently downgrades that entity to the mock.
    for (auto [e, intent] : registry.view<Intent>().each()) {
        intent = {.kind = 0, .dir = {0.0f, 0.0f}};
    }
    if (game->replay_log != nullptr) {
        // Replaying: this tick's decisions come from the log, not the brains.
        badlands::apply_replay_commands(*game);
    } else {
        for (size_t slot = 0; slot < game->slots.size(); ++slot) {
            entt::entity e = game->slots[slot];
            if (!registry.valid(e) || registry.all_of<InsideBuilding>(e)) {
                continue;  // hidden heroes don't think
            }
            auto& brain = registry.get<Brain>(e);
            bool scripted = brain.state && !brain.state->downgraded && game->brains;
            if (scripted && !resume_brain(*game, static_cast<uint32_t>(slot), *brain.state)) {
                brain.state->downgraded = true;
                scripted = false;
            }
            if (!scripted) {
                mock_think(*game, e, static_cast<uint32_t>(slot));
            }
        }

        // Drain AI commands enqueued during think, in one ordered pass (FIFO;
        // producers iterate by slot). This is the single mutation point for AI
        // decisions and appends each to command_log (the trace).
        apply_commands(*game);
    }

    // Legacy direct movement for scripted brains that still push a per-tick
    // move Intent (intent_move). The shipping brains (hero.noiser,
    // combat_test.noiser) and the mock brain all move via MoveTarget +
    // intent_move_to, so this loop is inert today; it is kept for any brain that
    // still drives a kind-1 move Intent (and for the intent_move host binding the
    // downgrade fixtures may exercise).
    for (auto [e, intent, pos, stats] :
         registry.view<const Intent, Position, const Stats>(entt::exclude<MeleeLock, InsideBuilding>)
             .each()) {
        if (intent.kind != 1) {
            continue;
        }
        float len = glm::length(intent.dir);
        if (len > 0.0f) {
            pos.pos += intent.dir / len * stats.move_speed * dt;
        }
    }

    // Navmesh movement pipeline: plan/follow durable MoveTargets, maintain melee
    // locks, and resolve unit-unit collisions. All exclude hidden (inside) heroes.
    plan_paths(*game, dt);
    follow_paths(*game, dt);
    update_melee_locks(*game);
    separate_units(*game);

    // Combat: the engine is authoritative — an attack intent only lands when
    // the nearest enemy is in range and the cooldown has elapsed.
    for (auto [e, intent, pos, stats, cooldown] :
         registry.view<const Intent, const Position, const Stats, CooldownTimer>(
                     entt::exclude<InsideBuilding>)
             .each()) {
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
            const auto* sim = game->registry.try_get<HeroSimulationState>(e);
            const auto* disp = game->registry.try_get<HeroDisplayState>(e);
            const auto* mt = game->registry.try_get<MoveTarget>(e);
            const auto* path = game->registry.try_get<NavPath>(e);
            // Resolve the goal to a world point so the panel needs no lookup:
            // an Entity/Building goal reports the thing's current position.
            glm::vec2 goal{0.0f, 0.0f};
            if (mt != nullptr) {
                if (mt->kind == MoveTarget::Kind::Point) {
                    goal = mt->point;
                } else if (mt->kind == MoveTarget::Kind::Entity &&
                           game->registry.valid(mt->entity)) {
                    goal = game->registry.get<Position>(mt->entity).pos;
                } else if (mt->kind == MoveTarget::Kind::Building &&
                           mt->building < game->placement.buildings.size()) {
                    goal = game->placement.buildings[mt->building].center;
                }
            }
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
                .home_building_id = sim ? sim->home_building_id : -1,
                .inside_building_id = game->registry.all_of<InsideBuilding>(e)
                                          ? game->registry.get<InsideBuilding>(e).building_id
                                          : -1,
                .fatigue = sim ? sim->fatigue : 0.0f,
                .boredom = sim ? sim->boredom : 0.0f,
                .behavior = sim ? sim->behavior : -1,
                .goal_kind = mt ? static_cast<int32_t>(mt->kind) : 0,
                .goal_x = goal.x,
                .goal_z = goal.y,
                .path_waypoints =
                    path ? static_cast<int32_t>(path->waypoints.size() - std::min<size_t>(
                                                    path->cursor, path->waypoints.size()))
                         : 0,
            };
            const char* name = disp ? disp->name.c_str() : "";
            std::size_t n = std::min(std::strlen(name), sizeof(out[total].name) - 1);
            std::memcpy(out[total].name, name, n);
            out[total].name[n] = '\0';
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

int64_t game_dispatch(BadlandsGame* game, const GameAction* action) {
    if (game == nullptr || action == nullptr) {
        return -1;
    }
    // Player actions become Commands applied synchronously through the one
    // apply_command mutation point (so they land in command_log like AI
    // decisions); the synchronous result (new id / <0) is preserved.
    badlands::Command cmd{};
    switch (action->kind) {
        case GAME_ACTION_PLACE_BUILDING:
            cmd.kind = badlands::CommandKind::PlaceBuilding;
            cmd.point = {action->world_x, action->world_z};
            cmd.param_a = action->param_a;
            cmd.param_b = action->param_b;
            break;
        case GAME_ACTION_RECRUIT_HERO:
            cmd.kind = badlands::CommandKind::RecruitHero;
            cmd.target_id = action->target_id;
            break;
        case GAME_ACTION_DESTROY_BUILDING:
            cmd.kind = badlands::CommandKind::DestroyBuilding;
            cmd.target_id = action->target_id;
            break;
        default:
            return -1;
    }
    return badlands::apply_command(*game, cmd);
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
