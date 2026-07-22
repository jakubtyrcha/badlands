// badlands::Sim method bodies + the extracted shared free functions
// (make_world / tick_world / *_of snapshots / spawn_into / dispatch_into /
// reload_script) over the UNCHANGED internal world (struct BadlandsGame), plus
// the handle-less helpers (RenderBoxOf / BuildingDefOf / MercenaryDesc /
// GoblinDesc). Both badlands::Sim and the game_* C ABI (game.cpp) forward here,
// so there is a single implementation of every operation.

#include "sim_internal.hpp"

#include "brain.h"
#include "components.h"
#include "heroes.h"
#include "movement.h"
#include "placement.h"

#include <entt/entt.hpp>
#include <glm/glm.hpp>

#include <algorithm>
#include <cstdio>
#include <limits>
#include <mutex>
#include <string>
#include <vector>

namespace badlands {

namespace {

// Reference behavior, and the fallback whenever an entity has no (or a
// downgraded) script brain: set a durable MoveTarget on the nearest enemy and
// swing when in range. The movement pipeline walks the MoveTarget; the combat
// pass re-validates the attack Intent authoritatively.
void mock_think(BadlandsGame& game, entt::entity self) {
    auto& reg = game.registry;
    MoveTarget& mt = reg.get<MoveTarget>(self);
    entt::entity target = nearest_enemy(game, self);
    if (target == entt::null) {
        mt.kind = MoveTarget::Kind::None;
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

// ---- extracted shared operations over Badlands& ----------------------------

std::unique_ptr<BadlandsGame> make_world(const char* brain_script_source) {
    // One-time noiser runtime configuration. The profiling switch is
    // thread-local and defaults to ON, and upstream has no public API for it
    // yet (docs/noiser-feedback.md #3) — this is the only detail:: callsite.
    // All make_world/tick_world calls happen on the same (main) thread.
    static std::once_flag noiser_configured;
    std::call_once(noiser_configured,
                   [] { sampo::noiser::detail::SetHostCallProfiling(false); });

    auto game = std::make_unique<BadlandsGame>();
    if (brain_script_source != nullptr) {
        std::string error;
        game->brains = BrainRuntime::create(*game, brain_script_source, error);
        if (!game->brains) {
            report_bug(*game, "compile", error);
        }
    }
    // The colony starts with only the castle, prebuilt at the origin. Not a
    // player placement: it seeds no urban sprawl.
    place_building(*game,
                   PlacementDesc{static_cast<int32_t>(BuildingKind::Castle), 0, 0.0f, 0.0f},
                   /*player=*/false);
    return game;
}

void tick_world(BadlandsGame& g, float dt) {
    auto& registry = g.registry;

    for (auto [e, cooldown] : registry.view<CooldownTimer>().each()) {
        cooldown.remaining = std::max(0.0f, cooldown.remaining - dt);
    }

    // Reappear hidden heroes whose stay has elapsed, before they think again.
    advance_inside_timers(g, dt);

    // Brains: each living entity's coroutine resumes once; intents arrive via
    // host calls. Any failure permanently downgrades that entity to the mock.
    for (auto [e, intent] : registry.view<Intent>().each()) {
        intent = {.kind = 0, .dir = {0.0f, 0.0f}};
    }
    for (size_t slot = 0; slot < g.slots.size(); ++slot) {
        entt::entity e = g.slots[slot];
        if (!registry.valid(e) || registry.all_of<InsideBuilding>(e)) {
            continue;  // hidden heroes don't think
        }
        auto& brain = registry.get<Brain>(e);
        bool scripted = brain.state && !brain.state->downgraded && g.brains;
        if (scripted && !resume_brain(g, static_cast<uint32_t>(slot), *brain.state)) {
            brain.state->downgraded = true;
            scripted = false;
        }
        if (!scripted) {
            mock_think(g, e);
        }
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
    plan_paths(g, dt);
    follow_paths(g, dt);
    update_melee_locks(g);
    separate_units(g);

    // Combat: the engine is authoritative — an attack intent only lands when
    // the nearest enemy is in range and the cooldown has elapsed.
    for (auto [e, intent, pos, stats, cooldown] :
         registry.view<const Intent, const Position, const Stats, CooldownTimer>(
                     entt::exclude<InsideBuilding>)
             .each()) {
        if (intent.kind != 2 || cooldown.remaining > 0.0f) {
            continue;
        }
        entt::entity target = nearest_enemy(g, e);
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

    ++g.ticks;
}

uint32_t spawn_into(BadlandsGame& g, const CharacterDesc& desc) {
    // Plain (home-less) spawn; heroes::spawn_entity emplaces the full component
    // set shared with recruit.
    return spawn_entity(g, desc, -1);
}

int64_t dispatch_into(BadlandsGame& g, const Action& action) {
    switch (action.kind) {
        case ActionKind::PlaceBuilding: {
            PlacementDesc desc{action.param_a, action.param_b, action.world_x, action.world_z};
            uint32_t id = place_building(g, desc, /*player=*/true);
            return (id == std::numeric_limits<uint32_t>::max()) ? -1 : static_cast<int64_t>(id);
        }
        case ActionKind::RecruitHero: {
            uint32_t id = recruit(g, action.target_id);
            return (id == std::numeric_limits<uint32_t>::max()) ? -1 : static_cast<int64_t>(id);
        }
        case ActionKind::DestroyBuilding:
            return destroy_building_impl(g, action.target_id);
        default:
            return -1;
    }
}

bool reload_script(BadlandsGame& g, const std::string& source) {
    std::string error;
    auto fresh = BrainRuntime::create(g, source, error);
    if (!fresh) {
        // Keep-last-good: the running program stays in place.
        std::fprintf(stderr, "[noiser] reload failed, keeping last-good: %s\n", error.c_str());
        return false;
    }
    g.brains = std::move(fresh);
    g.noiser_bugs = 0;
    g.script_intents = 0;
    for (uint32_t slot = 0; slot < g.slots.size(); ++slot) {
        entt::entity e = g.slots[slot];
        if (g.registry.valid(e)) {
            g.registry.get<Brain>(e).state = spawn_brain(*g.brains, slot);
        }
    }
    return true;
}

std::vector<CharacterState> characters_of(const BadlandsGame& g) {
    std::vector<CharacterState> rows;
    for (uint32_t slot = 0; slot < g.slots.size(); ++slot) {
        entt::entity e = g.slots[slot];
        if (!g.registry.valid(e)) {
            continue;
        }
        const auto& pos = g.registry.get<Position>(e);
        const auto& health = g.registry.get<Health>(e);
        const auto& shape = g.registry.get<RenderShape>(e);
        rows.push_back(CharacterState{
            .id = slot,
            .team = g.registry.get<Team>(e).id,
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
            .home_building_id =
                g.registry.all_of<Home>(e) ? g.registry.get<Home>(e).building_id : -1,
            .inside_building_id = g.registry.all_of<InsideBuilding>(e)
                                      ? g.registry.get<InsideBuilding>(e).building_id
                                      : -1,
        });
    }
    return rows;
}

SimStats stats_of(const BadlandsGame& g) {
    return SimStats{
        .ticks = g.ticks,
        .script_intents = g.script_intents,
        .noiser_bugs = g.noiser_bugs,
    };
}

// ---- handle-less helpers ---------------------------------------------------
// BuildingDefOf / RenderBoxOf / buildings_of / world_of / probe_of own their
// logic directly in placement.cpp now (no C-ABI forwarding).

CharacterDesc MercenaryDesc(float pos_x, float pos_z) {
    return CharacterDesc{
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

CharacterDesc GoblinDesc(float pos_x, float pos_z) {
    return CharacterDesc{
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

// ---- Sim methods -----------------------------------------------------------

Sim::Sim(const char* brain_script_source) : world_(make_world(brain_script_source)) {}
Sim::~Sim() = default;
Sim::Sim(Sim&&) noexcept = default;
Sim& Sim::operator=(Sim&&) noexcept = default;

uint32_t Sim::Spawn(const CharacterDesc& desc) { return spawn_into(*world_, desc); }
void Sim::Tick(float dt) { tick_world(*world_, dt); }
bool Sim::ReloadScript(const std::string& source) { return reload_script(*world_, source); }
int64_t Sim::Dispatch(const Action& action) { return dispatch_into(*world_, action); }

void Sim::SetPathfinder(const Pathfinder& pf) {
    // Store the provider by value, then back-fill it with every alive building
    // already placed (the prebuilt castle, and any placed before registration)
    // so its obstacle set matches the world. A default-constructed Pathfinder
    // (null add_obstacle) clears the provider without back-filling.
    BadlandsGame& game = *world_;
    game.pathfinder = pf;
    if (game.pathfinder.add_obstacle == nullptr) {
        return;
    }
    const auto& buildings = game.placement.buildings;
    for (uint32_t id = 0; id < buildings.size(); ++id) {
        if (buildings[id].alive) {
            notify_obstacle_added(game, id);
        }
    }
}

std::vector<CharacterState> Sim::Characters() const { return characters_of(*world_); }
std::vector<BuildingState> Sim::Buildings() const { return buildings_of(*world_); }
WorldState Sim::World() const { return world_of(*world_); }
SimStats Sim::GetStats() const { return stats_of(*world_); }

PlacementProbe Sim::ProbePlacement(const PlacementDesc& desc,
                                   std::vector<GridTriangle>& out_triangles) const {
    return probe_of(*world_, desc, out_triangles);
}

entt::registry& Sim::registry() { return world_->registry; }
const entt::registry& Sim::registry() const { return world_->registry; }

}  // namespace badlands
