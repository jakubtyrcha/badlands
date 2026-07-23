// badlands::Sim method bodies + the extracted shared free functions
// (make_world / tick_world / *_of snapshots / spawn_into / dispatch_into /
// reload_script) over the UNCHANGED internal world (struct BadlandsGame), plus
// the handle-less helpers (RenderBoxOf / BuildingDefOf / MercenaryDesc /
// GoblinDesc). badlands::Sim and the internal system tests call these free
// functions directly, so there is a single implementation of every operation.

#include "sim_internal.hpp"

#include "brain.h"
#include "components.h"
#include "heroes.h"  // spawn_entity, biome_at
#include "command.h"
#include "movement.h"
#include "needs.h"
#include "placement.h"
#include "vision.h"

#include "critter_brain.h"
#include "economy.h"
#include "monster_brain.h"
#include "townfolk_brain.h"

#include "game/map/symbolic_map_generator.hpp"
#include "town_brain.h"

#include <entt/entt.hpp>
#include <glm/glm.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <mutex>
#include <string>
#include <vector>

namespace badlands {

namespace {

// Reference behavior, and the fallback whenever an entity has no (or a
// downgraded) script brain. Combat pre-empt: set a durable MoveTarget on the
// nearest enemy and swing when in range (the movement pipeline walks the
// MoveTarget; the combat pass re-validates the attack Intent authoritatively).
// With no enemy, delegate to the C++ town brain (needs/day-night loop).
void mock_think(BadlandsGame& game, entt::entity self, uint32_t slot) {
    auto& reg = game.registry;
    MoveTarget& mt = reg.get<MoveTarget>(self);
    const BrainKind kind = reg.get<Brain>(self).kind;

    // Critters and townfolk never fight -- their brains own their movement, so
    // they skip the combat pre-empt entirely (otherwise a neutral deer, or a
    // peaceful tax collector, would read an other-team unit as an "enemy" and
    // give chase). Guards (a future townfolk) will opt back into combat.
    if (kind == BrainKind::Critter) {
        critter_think(game, slot);
        return;
    }
    if (kind == BrainKind::Townfolk) {
        townfolk_think(game, slot);
        return;
    }

    // Combat pre-empt for the fighting archetypes: chase and swing at the
    // nearest enemy. The movement pipeline walks the MoveTarget; the combat pass
    // re-validates the attack Intent authoritatively.
    entt::entity target = nearest_enemy(game, self);
    if (target == entt::null) {
        // No enemy: the archetype's own brain decides.
        switch (kind) {
            case BrainKind::Town:
                town_think(game, slot);
                break;
            case BrainKind::Monster:
                monster_think(game, slot);  // no unit enemy -> gnaw a building
                break;
            case BrainKind::None:
                mt.kind = MoveTarget::Kind::None;
                break;
            case BrainKind::Critter:
            case BrainKind::Townfolk:
                break;  // handled above
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
    // Terrain/biomes the sim reasons about. SymbolicMapGenerator is a pure
    // function of its compile-time constants, so every world would generate a
    // byte-identical map -- generate once and copy (the shaping blur passes cost
    // ~0.7 s, which a test suite creating a world per case cannot pay).
    // Determinism is unaffected: the copied data is the same either way.
    // The placement/movement grid must span the whole map (tile == 1 world unit ==
    // 1 map metre). If the map size changes, kGridHalfExtentTiles must track it.
    static_assert(2 * kGridHalfExtentTiles ==
                      static_cast<int>(SymbolicMapGenerator::kMapSizeM),
                  "gameplay grid must span the full map");
    static const MapData kSymbolicMap = SymbolicMapGenerator{}.Generate();
    game->map = kSymbolicMap;
    if (brain_script_source != nullptr) {
        std::string error;
        game->brains = BrainRuntime::create(*game, brain_script_source, error);
        if (!game->brains) {
            report_bug(*game, "compile", error);
        }
    }
    // The colony starts with only the castle, prebuilt on the plains south of
    // the central lake (the map origin is water). Not a player placement: it
    // seeds no urban sprawl. This is the colony seat the game's town forms
    // around; kCastleSpawn is the single source of truth for it.
    place_building(
        *game,
        PlacementDesc{static_cast<int32_t>(BuildingKind::Castle), 0, kCastleSpawnX,
                      kCastleSpawnZ},
        /*player=*/false);
    return game;
}

void tick_world(BadlandsGame& g, float dt) {
    auto& registry = g.registry;

    // Replay: commands stamped at the CURRENT time were originally applied
    // before this tick (player dispatches between ticks), so they land first.
    apply_replay_commands(g);

    // Day/night clock: integer ms, fixed compile-time increment (deterministic).
    g.world_millis += kMillisPerTick;

    for (auto [e, cooldown] : registry.view<CooldownTimer>().each()) {
        cooldown.remaining = std::max(0.0f, cooldown.remaining - dt);
    }

    // Reappear hidden heroes whose stay has elapsed, before they think again.
    advance_inside_timers(g, dt);

    // Needs system: fatigue/boredom rise for active (non-hidden) heroes, so
    // brains this tick see fresh values.
    advance_needs(g);

    // Town economy + population: midnight tax accrual, then periodic spawning
    // (tax collectors from the castle). Deterministic clock-driven systems, so
    // they run identically in live and replay -- before think, so a just-spawned
    // entity thinks and a just-accrued building is visible the same tick.
    advance_economy(g);
    run_spawners(g);

    // Brains: each living entity's coroutine resumes once; intents arrive via
    // host calls. Any failure permanently downgrades that entity to the mock.
    for (auto [e, intent] : registry.view<Intent>().each()) {
        intent = {.kind = 0, .dir = {0.0f, 0.0f}};
    }
    if (g.replay_log != nullptr) {
        // Replaying: this tick's decisions come from the log, not the brains.
        apply_replay_commands(g);
    } else {
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
                mock_think(g, e, static_cast<uint32_t>(slot));
            }
        }

        // Drain AI commands enqueued during think, in one ordered pass (FIFO;
        // producers iterate by slot). This is the single mutation point for AI
        // decisions and appends each to command_log (the trace).
        apply_commands(g);
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

    // Fog-of-war: resolve next visibility from the post-movement world state and
    // publish it (double-buffered). No-op until ConfigureVision.
    resolve_vision(g);

    ++g.ticks;
}

uint32_t spawn_into(BadlandsGame& g, const CharacterDesc& desc) {
    // Plain (home-less) spawn; heroes::spawn_entity emplaces the full component
    // set shared with recruit.
    return spawn_entity(g, desc, -1);
}

int64_t dispatch_into(BadlandsGame& g, const Action& action) {
    // Player actions become Commands applied synchronously through the one
    // apply_command mutation point (so they land in command_log like AI
    // decisions); the synchronous result (new id / <0) is preserved.
    Command cmd{};
    switch (action.kind) {
        case ActionKind::PlaceBuilding:
            cmd.kind = CommandKind::PlaceBuilding;
            cmd.point = {action.world_x, action.world_z};
            cmd.param_a = action.param_a;
            cmd.param_b = action.param_b;
            break;
        case ActionKind::RecruitHero:
            cmd.kind = CommandKind::RecruitHero;
            cmd.target_id = action.target_id;
            break;
        case ActionKind::DestroyBuilding:
            cmd.kind = CommandKind::DestroyBuilding;
            cmd.target_id = action.target_id;
            break;
        default:
            return -1;
    }
    return apply_command(g, cmd);
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
        const auto* sim = g.registry.try_get<HeroSimulationState>(e);
        const auto* disp = g.registry.try_get<HeroDisplayState>(e);
        const auto* crit = g.registry.try_get<CritterState>(e);
        const auto* tax = g.registry.try_get<TaxCollectorState>(e);
        const auto* mt = g.registry.try_get<MoveTarget>(e);
        const auto* path = g.registry.try_get<NavPath>(e);
        // Resolve the goal to a world point so observers need no lookup.
        glm::vec2 goal{0.0f, 0.0f};
        if (mt != nullptr) {
            if (mt->kind == MoveTarget::Kind::Point) {
                goal = mt->point;
            } else if (mt->kind == MoveTarget::Kind::Entity && g.registry.valid(mt->entity)) {
                goal = g.registry.get<Position>(mt->entity).pos;
            } else if (mt->kind == MoveTarget::Kind::Building &&
                       mt->building < g.placement.buildings.size()) {
                goal = g.placement.buildings[mt->building].center;
            }
        }
        const Facing* facing_c = g.registry.try_get<Facing>(e);
        const glm::vec2 facing = facing_c ? facing_c->dir : kCharacterForward;
        const Vision* vis = g.registry.try_get<Vision>(e);
        const float vis_radius = vis ? vis->radius : 0.0f;
        const float vis_half_deg =
            vis ? glm::degrees(std::acos(std::clamp(vis->cone_half_cos, -1.0f, 1.0f)))
                : 180.0f;
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
            .home_building_id = sim ? sim->home_building_id : -1,
            .inside_building_id = g.registry.all_of<InsideBuilding>(e)
                                      ? g.registry.get<InsideBuilding>(e).building_id
                                      : -1,
            .fatigue = sim ? sim->fatigue : 0.0f,
            .boredom = sim ? sim->boredom : 0.0f,
            .behavior = sim ? sim->behavior
                            : (crit ? crit->behavior : (tax ? tax->behavior : -1)),
            .goal_kind = mt ? static_cast<int32_t>(mt->kind) : 0,
            .goal_x = goal.x,
            .goal_z = goal.y,
            .path_waypoints =
                path ? static_cast<int32_t>(path->waypoints.size() -
                                            std::min<size_t>(path->cursor, path->waypoints.size()))
                     : 0,
            .archetype = static_cast<int32_t>(
                sim ? Archetype::Hero
                    : (crit ? Archetype::Critter
                            : (tax ? Archetype::Townfolk : Archetype::Monster))),
            .facing_x = facing.x,
            .facing_z = facing.y,
            .vision_radius = vis_radius,
            .vision_cone_half_angle_deg = vis_half_deg,
        });
        const char* nm = disp ? disp->name.c_str() : "";
        std::size_t n = std::min(std::strlen(nm), sizeof(rows.back().name) - 1);
        std::memcpy(rows.back().name, nm, n);
        rows.back().name[n] = '\0';
    }
    return rows;
}

std::vector<CommandRecord> command_log_of(const BadlandsGame& g) {
    std::vector<CommandRecord> rows;
    rows.reserve(g.command_log.size());
    for (const Command& c : g.command_log) {
        rows.push_back(CommandRecord{
            .kind = static_cast<CommandKindId>(c.kind),
            .actor = c.actor,
            .target_id = c.target_id,
            .point_x = c.point.x,
            .point_z = c.point.y,
            .param_a = c.param_a,
            .param_b = c.param_b,
            .at_millis = c.at_millis,
        });
    }
    return rows;
}

void set_factors_of(BadlandsGame& g, const SimFactors& f) { g.factors = f; }
int32_t biome_at_of(const BadlandsGame& g, float x, float z) {
    return static_cast<int32_t>(biome_at(g, {x, z}));
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
        .vision_radius = 14.0f,
        .vision_cone_half_angle_deg = 60.0f,
    };
}

CharacterDesc GoblinDesc(float pos_x, float pos_z) {
    return CharacterDesc{
        .archetype = Archetype::Monster,
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

void Sim::ConfigureVision(float world_min_x, float world_min_z, float world_size_x,
                          float world_size_z, float texel_m) {
    configure_vision(world_->vision, world_min_x, world_min_z, world_size_x, world_size_z,
                     texel_m);
}
void Sim::ResolveVision() { resolve_vision(*world_); }
VisionField Sim::GetVisionField() const { return vision_field_of(world_->vision); }
VisionLevel Sim::QueryVision(float cx, float cz, float radius) const {
    return query_vision(world_->vision, cx, cz, radius);
}

std::vector<CharacterState> Sim::Characters() const { return characters_of(*world_); }
void Sim::Buildings(std::vector<BuildingState>& out) const { buildings_of(*world_, out); }
std::vector<BuildingState> Sim::Buildings() const {
    std::vector<BuildingState> rows;
    Buildings(rows);
    return rows;
}
WorldState Sim::World() const { return world_of(*world_); }
SimStats Sim::GetStats() const { return stats_of(*world_); }
std::vector<CommandRecord> Sim::CommandLog() const { return command_log_of(*world_); }
void Sim::SetFactors(const SimFactors& f) { set_factors_of(*world_, f); }
const SimFactors& Sim::Factors() const { return world_->factors; }
int32_t Sim::BiomeAt(float x, float z) const { return biome_at_of(*world_, x, z); }

PlacementProbe Sim::ProbePlacement(const PlacementDesc& desc,
                                   std::vector<GridTriangle>& out_triangles) const {
    return probe_of(*world_, desc, out_triangles);
}

entt::registry& Sim::registry() { return world_->registry; }
const entt::registry& Sim::registry() const { return world_->registry; }

}  // namespace badlands
