// badlands::Sim method bodies + the extracted shared free functions
// (make_world / tick_world / *_of snapshots / spawn_into / dispatch_into /
// reload_script) over the UNCHANGED internal world (struct BadlandsGame), plus
// the handle-less helpers (RenderBoxOf / BuildingDefOf / MercenaryDesc /
// GoblinDesc). badlands::Sim and the internal system tests call these free
// functions directly, so there is a single implementation of every operation.

#include "sim_internal.hpp"

#include "brain.h"
#include "combat.h"
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

    // Combat pre-empt for the fighting archetypes: engage the nearest enemy. The
    // movement pipeline walks the MoveTarget; the Attack command resolves the
    // hit authoritatively.
    entt::entity target = select_target(game, self);
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
    // Close to the stance's engagement distance -- a melee unit closes to melee
    // reach, a ranged unit holds at bow distance.
    const Combatant& cb = reg.get<Combatant>(self);
    const Attacks& atk = reg.get<Attacks>(self);
    mt.kind = MoveTarget::Kind::Entity;
    mt.entity = target;
    mt.building = UINT32_MAX;
    mt.stop_distance = engagement_range(cb, atk);

    // If an attack is usable right now, emit an Attack command. Target UINT32_MAX
    // => the handler re-picks the nearest enemy. Off-cooldown gating keeps this to
    // ~one command per swing rather than one per tick.
    if (select_attack(game, self, target) >= 0) {
        game.command_queue.push_back({CommandKind::Attack, slot});
    }
}

}  // namespace

// ---- extracted shared operations over Badlands& ----------------------------

std::unique_ptr<BadlandsGame> make_world(const char* brain_script_source,
                                         const WorldConfig& config) {
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
    game->terrain_blocking = config.terrain_blocking;
    game->arena_half_x = config.arena_half_x;
    game->arena_half_z = config.arena_half_z;
    if (brain_script_source != nullptr) {
        std::string error;
        game->brains = BrainRuntime::create(*game, brain_script_source, error);
        if (!game->brains) {
            report_bug(*game, "compile", error);
        }
    }
    if (config.prebuild_colony) {
        // The colony starts with only the castle, prebuilt on the plains south of
        // the central lake (the map origin is water). Not a player placement: it
        // seeds no urban sprawl. This is the colony seat the game's town forms
        // around; kCastleSpawn is the single source of truth for it. The arena
        // turns this off.
        place_building(
            *game,
            PlacementDesc{static_cast<int32_t>(BuildingKind::Castle), 0, kCastleSpawnX,
                          kCastleSpawnZ},
            /*player=*/false);
    }
    return game;
}

std::unique_ptr<BadlandsGame> make_world(const char* brain_script_source) {
    return make_world(brain_script_source, WorldConfig{});
}

std::unique_ptr<BadlandsGame> make_flat_world() {
    auto game = make_world(nullptr);
    game->terrain_blocking = false;
    return game;
}

void tick_world(BadlandsGame& g, float dt) {
    auto& registry = g.registry;

    // Replay: commands stamped at the CURRENT time were originally applied
    // before this tick (player dispatches between ticks), so they land first.
    apply_replay_commands(g);

    // Day/night clock: integer ms, fixed compile-time increment (deterministic).
    g.world_millis += kMillisPerTick;

    // Per-attack cooldowns tick down (one timer per attack-skill).
    for (auto [e, attacks] : registry.view<Attacks>().each()) {
        for (int i = 0; i < attacks.count && i < kMaxAttacks; ++i) {
            attacks.cooldown_remaining[i] =
                std::max(0.0f, attacks.cooldown_remaining[i] - dt);
        }
    }

    // Needs first: reserves drain (and refill, for anyone inside) before
    // anything looks at them, so a hero whose sleep just topped out is released
    // by advance_inside on the same tick rather than one later.
    advance_needs(g);

    // Release hidden heroes whose reason for being inside is over.
    advance_inside(g);

    // Run conversations and dissolve the finished ones, before think, so a hero
    // whose companion just left decides afresh this very tick rather than
    // standing about for one more.
    advance_chats(g, dt);

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

    // Ranged shots in flight advance and resolve on arrival (melee already
    // resolved in the Attack command during the think pass). A pure system rule:
    // deterministic, runs identically live and on replay.
    advance_projectiles(g, dt);

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

uint32_t spawn_creature_into(BadlandsGame& g, CreatureId id, int32_t team, glm::vec2 pos) {
    const int i = static_cast<int>(id);
    if (i < 0 || i >= kCreatureCount) {
        return UINT32_MAX;
    }
    CharacterDesc desc = g.creatures.defs[i];
    desc.pos_x = pos.x;
    desc.pos_z = pos.y;
    desc.team = team;
    const uint32_t slot = spawn_entity(g, desc, -1);
    // Hero creatures (ids 0..HERO_CLASS_COUNT-1 == HeroClassId) carry their class,
    // which spawn_entity otherwise only derives from a home guild.
    if (i < HERO_CLASS_COUNT) {
        if (auto* hc = g.registry.try_get<HeroCharacter>(g.slots[slot])) {
            hc->hero_class = static_cast<int32_t>(i);
        }
    }
    return slot;
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

void characters_of(const BadlandsGame& g, std::vector<CharacterState>& out) {
    out.clear();
    for (uint32_t slot = 0; slot < g.slots.size(); ++slot) {
        entt::entity e = g.slots[slot];
        if (!g.registry.valid(e)) {
            continue;
        }
        const auto& pos = g.registry.get<Position>(e);
        const auto& health = g.registry.get<Health>(e);
        const auto& shape = g.registry.get<RenderShape>(e);
        const auto* sim = g.registry.try_get<HeroSimulationState>(e);
        const auto* hero = g.registry.try_get<HeroCharacter>(e);
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
        out.push_back(CharacterState{
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
            .content = sim ? sim->content : 0.0f,
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
            .hero_class = hero ? hero->hero_class : -1,
            .facing_x = facing.x,
            .facing_z = facing.y,
            .vision_radius = vis_radius,
            .vision_cone_half_angle_deg = vis_half_deg,
        });
        const char* nm = disp ? disp->name.c_str() : "";
        std::size_t n = std::min(std::strlen(nm), sizeof(out.back().name) - 1);
        std::memcpy(out.back().name, nm, n);
        out.back().name[n] = '\0';
    }
}

std::vector<CharacterState> characters_of(const BadlandsGame& g) {
    std::vector<CharacterState> rows;
    characters_of(g, rows);
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

// The Stage-2 duelists now come straight from the creature catalog (the single
// source of truth), with only position/team stamped on. Kept as named helpers
// because the tests and the Rust app reference them by name.
CharacterDesc MercenaryDesc(float pos_x, float pos_z) {
    CharacterDesc d = DefaultCreatureCatalog().defs[static_cast<int>(CreatureId::Mercenary)];
    d.pos_x = pos_x;
    d.pos_z = pos_z;
    d.team = 0;
    return d;
}

CharacterDesc GoblinDesc(float pos_x, float pos_z) {
    CharacterDesc d = DefaultCreatureCatalog().defs[static_cast<int>(CreatureId::Goblin)];
    d.pos_x = pos_x;
    d.pos_z = pos_z;
    d.team = 1;
    return d;
}

// ---- Sim methods -----------------------------------------------------------

Sim::Sim(const char* brain_script_source) : world_(make_world(brain_script_source)) {}
Sim::Sim(const WorldConfig& config, const char* brain_script_source)
    : world_(make_world(brain_script_source, config)) {}
Sim::~Sim() = default;
Sim::Sim(Sim&&) noexcept = default;
Sim& Sim::operator=(Sim&&) noexcept = default;

uint32_t Sim::Spawn(const CharacterDesc& desc) { return spawn_into(*world_, desc); }
uint32_t Sim::SpawnCreature(CreatureId id, int32_t team, float pos_x, float pos_z) {
    return spawn_creature_into(*world_, id, team, {pos_x, pos_z});
}
void Sim::SetCreatureCatalog(const CreatureCatalog& catalog) { world_->creatures = catalog; }
const CreatureCatalog& Sim::Creatures() const { return world_->creatures; }
void Sim::Tick(float dt) {
    tick_world(*world_, dt);
    // Goal statistics are folded HERE, in the wrapper, from the very rows an
    // observer would read -- never inside tick_world. Counting is an
    // observation of the sim, not a part of it; see ActivityHistogram.
    characters_of(*world_, stats_scratch_);
    activity_stats_.Accumulate(stats_scratch_);
}
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

std::vector<ProjectileState> Sim::Projectiles() const {
    std::vector<ProjectileState> rows;
    const auto& reg = world_->registry;
    for (auto [e, proj] : reg.view<const Projectile>().each()) {
        glm::vec2 tp = proj.pos;
        const entt::entity target =
            entity_for_slot(*world_, static_cast<int32_t>(proj.target_slot));
        if (target != entt::null && reg.all_of<Position>(target)) {
            tp = reg.get<Position>(target).pos;
        }
        rows.push_back({proj.pos.x, proj.pos.y, tp.x, tp.y});
    }
    return rows;
}

std::vector<CharacterState> Sim::Characters() const { return characters_of(*world_); }
void Sim::Characters(std::vector<CharacterState>& out) const { characters_of(*world_, out); }
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
