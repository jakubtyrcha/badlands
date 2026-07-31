// badlands::Sim method bodies + the extracted shared free functions
// (make_world / tick_world / *_of snapshots / spawn_into / dispatch_into) over
// the UNCHANGED internal world (struct BadlandsGame), plus the handle-less
// helpers (RenderBoxOf / BuildingDefOf / MercenaryDesc / GoblinDesc).
// badlands::Sim and the internal system tests call these free functions
// directly, so there is a single implementation of every operation.

#include "sim_internal.hpp"

#include "brain_kind.h"
#include "combat.h"
#include "components.h"
#include "entity_memory.h"  // update_entity_memory
#include "heroes.h"  // spawn_entity, biome_at
#include "command.h"
#include "intention.h"  // advance_intentions, push_inbox_event, should_wake
#include "movement.h"
#include "nav_world.h"
#include "needs.h"
#include "placement.h"
#include "progression.h"
#include "skills.h"
#include "status.h"
#include "vision.h"

#include "critter_brain.h"
#include "economy.h"
#include "monster_brain.h"
#include "townfolk_brain.h"

#include "game/map/symbolic_map_generator.hpp"
#include "wasm_brain.h"

#include <entt/entt.hpp>
#include <glm/glm.hpp>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <vector>

namespace badlands {

namespace {

// Reference behavior for every non-hero archetype (and the Town fallback when
// no wasm brain is loaded). Single-gateway combat (docs/superpowers/specs/
// 2026-07-25-contract-v3-alignment-design.md): there is no host-level combat
// pre-empt anymore -- every swing is a brain action, issued through the same
// intention/action seams a wasm hero uses (game/src/intention.h). Monster is
// the one archetype here with a fighting brain (monster_think, below); a
// brainless Town/None hero issues no intentions/actions at all and simply
// holds position, defending only passively (resolve_attack's defender gates,
// combat.cpp) -- a Town-kind entity with no wasm brain loaded is exactly that
// case (the wasm brain is the sole hero decision-maker now; see
// hero_perception.h's file comment).
void mock_think(BadlandsGame& game, entt::entity self, uint32_t slot) {
    auto& reg = game.registry;
    const BrainKind kind = reg.get<Brain>(self).kind;

    // Critters and townfolk never fight -- their brains own their movement
    // and never call into combat at all (otherwise a neutral deer, or a
    // peaceful tax collector, would read an other-team unit as an "enemy"
    // and give chase). Guards (a future townfolk) will opt back into combat.
    switch (kind) {
        case BrainKind::Critter:
            critter_think(game, slot);
            return;
        case BrainKind::Townfolk:
            townfolk_think(game, slot);
            return;
        case BrainKind::Monster:
            monster_think(game, slot);  // fights a unit target, or gnaws a building
            return;
        case BrainKind::Town:
        case BrainKind::None:
            // Brainless: no intentions, no actions, passive defense only.
            reg.get<MoveTarget>(self).kind = MoveTarget::Kind::None;
            return;
    }
}

}  // namespace

// ---- extracted shared operations over Badlands& ----------------------------

std::unique_ptr<BadlandsGame> make_world(const BrainDesc& desc, const WorldConfig& config) {
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
    // The clock helpers divide by this, so a zero/negative period would be a
    // division by zero rather than merely a strange world -- clamp at the
    // boundary and say so, the same shape as sanitize_factors' adjustments.
    if (config.millis_per_day < 1) {
        spdlog::warn("make_world: millis_per_day {} is not a valid day length -- using {}",
                     config.millis_per_day, kDefaultMillisPerDay);
        game->millis_per_day = kDefaultMillisPerDay;
    } else {
        game->millis_per_day = config.millis_per_day;
    }
    if (desc.wasm_bytes != nullptr) {
        // Wasm bytes were explicitly provided, so a bh_load/bh_instantiate
        // failure here is a brain bug, not a config error to fall back from
        // -- WasmBrainRuntime::create is fatal on failure (brain_fatal,
        // wasm_brain.cpp) and never returns null.
        game->wasm_brains = WasmBrainRuntime::create(desc.wasm_bytes, desc.wasm_len);
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

// Thin forwarder onto the (BrainDesc, WorldConfig) implementation above.
std::unique_ptr<BadlandsGame> make_world(const BrainDesc& desc) {
    return make_world(desc, WorldConfig{});
}

// See the doc comment in badlands_sim.hpp for WHY this goes through ticks
// rather than sim_seconds * 1000. kSimHz/kMillisPerTick are sim internals
// (components.h), which is exactly why this conversion lives here instead of
// being open-coded by every app.
int64_t MillisPerDayForSimSeconds(float sim_seconds) {
    // Not `<= 0` -- this form also rejects NaN, which would otherwise survive
    // the clamp below and land as an arbitrary integer.
    if (!(sim_seconds > 0.0f)) {
        spdlog::warn("MillisPerDayForSimSeconds: {} is not a valid day length -- using {} ms",
                     sim_seconds, kDefaultMillisPerDay);
        return kDefaultMillisPerDay;
    }
    // A day of ~31 years, far past anything useful, but it keeps the double ->
    // int64 conversion in range for any finite input (including +inf).
    constexpr double kMaxMillisPerDay = 1e12;
    const double millis = static_cast<double>(sim_seconds) *
                          static_cast<double>(kSimHz) * static_cast<double>(kMillisPerTick);
    return static_cast<int64_t>(std::llround(std::clamp(millis, 1.0, kMaxMillisPerDay)));
}

std::unique_ptr<BadlandsGame> make_flat_world() {
    auto game = make_world(BrainDesc{});
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

    // Per-skill cooldowns, the same shape (one timer per learned skill). Float
    // seconds like the attack timers above, not int64 ms like a status: a
    // cooldown gates a DECISION (may I cast?) rather than dating a world fact,
    // and it sits alongside SkillSpec::cooldown_seconds, which is authored in
    // seconds.
    for (auto [e, skills] : registry.view<Skills>().each()) {
        for (int i = 0; i < skills.count && i < kMaxSkills; ++i) {
            skills.cooldown_remaining[i] =
                std::max(0.0f, skills.cooldown_remaining[i] - dt);
        }
    }

    // Statuses expire on the integer clock, BEFORE anything reads them: the
    // think dispatch, movement, and combat all gate on has_status further down
    // this same tick, so a status whose last millisecond ran out must be gone
    // by the time they look rather than granting one extra tick of effect.
    advance_statuses(g);

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

    // Navmesh current BEFORE brains think: AI goal selection queries nav_cost
    // (nav_world.h) over it. Gated on terrain_blocking (flat worlds stay
    // obstacle-oblivious). A building the AI places this tick lands after think,
    // so the pre-plan_paths rebuild below picks it up (one-tick lag for the
    // placing brain, which is fine). Cheap no-op when the epoch is unchanged.
    if (g.terrain_blocking) {
        rebuild_navmesh_if_stale(g);
    }

    // EntityMemory: refresh every character's bounded knowledge of who/what
    // it currently sees before anyone thinks, so a just-spawned entity's
    // memory (and everyone else's memory of it) is consistent this very
    // tick. Pure derived state -- reads the world, writes only EntityMemory
    // components -- so it runs unconditionally, live or replaying alike.
    update_entity_memory(g);

    // Intention contract: ThreatSighted inbox writer. Fires on the
    // empty -> nonempty edge of "is there a hostile within threat_radius",
    // not every tick a threat remains in view -- a guaranteed-wake event is a
    // notification, not a per-tick spam channel. Reuses nearest_enemy +
    // factors.hero.threat_radius (game_state.h) rather than
    // collect_threats/observe_hero (behaviours/perception.cpp,
    // hero_perception.cpp): those live INSIDE the per-hero think path and
    // only run on an actual wake (should_wake-gated), but this pass must see
    // every hero every tick regardless of whether it wakes this tick. threat_was_present
    // lives on EventInbox itself (not scratch state here), the same reason
    // MoveBlocked keeps its at_millis on the component, so a replay
    // reproduces the same edges.
    //
    // Iterates EVERY hero, hidden ones included (review fix: excluding them
    // from the view left threat_was_present frozen at whatever it was
    // before they hid, so a stale `true` silently suppressed the real
    // sighting waiting at the door when they re-emerged -- the
    // empty->nonempty edge never fired because the edge was never reset). A
    // hidden hero sees nothing, so its edge is force-reset to false and it
    // writes no event; any threat present once it exits reads as a fresh
    // sighting, correctly.
    //
    // Cleanup: also fills nearest_enemy_scratch (game_state.h), a one-tick
    // cache indexed by slot for every EventInbox-bearing entity (heroes AND,
    // since the single-gateway cutover, monsters too -- heroes.cpp's spawn
    // recipe). monster_think (monster_brain.cpp) consults it directly
    // instead of re-scanning, since this same nearest_enemy result is what
    // it needs moments later in THIS same tick's think pass (nothing moves
    // or dies between here and there) -- the same shortcut the deleted
    // combat_preempt's own cache read relied on. Hidden heroes are SKIPPED
    // (not written) by the `continue` above -- fine because the cache's one
    // safe consumer is never even called for them either: the think loop's
    // own per-slot dispatch already excludes InsideBuilding heroes before
    // mock_think is reached, so a stale/never-written entry is never read.
    // (apply_intention's own Attack-engagement executor and advance_
    // intentions' Attack-abort check, intention.cpp, do NOT read this cache
    // -- they run later in (or after) the tick, once movement/combat may
    // have invalidated it, so they pay for a live select_target scan
    // instead; see that function's own doc comment, combat.cpp.)
    for (auto [e, inbox] : registry.view<EventInbox>().each()) {
        if (registry.all_of<InsideBuilding>(e)) {
            inbox.threat_was_present = false;
            continue;
        }
        entt::entity threat = nearest_enemy(g, e);
        if (const uint32_t slot = slot_for_entity(g, e); slot != UINT32_MAX) {
            if (g.nearest_enemy_scratch.size() <= slot) {
                g.nearest_enemy_scratch.resize(slot + 1, entt::null);
            }
            g.nearest_enemy_scratch[slot] = threat;
        }
        const bool present = threat != entt::null &&
                             glm::distance(registry.get<Position>(e).pos,
                                           registry.get<Position>(threat).pos) <=
                                 g.factors.hero.threat_radius;
        if (present && !inbox.threat_was_present) {
            InboxEvent ev;
            ev.kind = InboxEventKind::ThreatSighted;
            ev.source_slot = slot_for_entity(g, threat);
            push_inbox_event(g, e, ev);
        }
        inbox.threat_was_present = present;
    }

    // Brains: each living entity thinks once (the wasm hero brain on a real
    // wake, everyone else via the mock's per-archetype logic below).
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
            if (g.wasm_brains && brain.kind == BrainKind::Town) {
                // mock_think is never reached for this entity while a wasm
                // program is loaded -- see wasm_brain.h. The intention
                // contract's wake rule gates the think itself (docs/design/
                // intention-contract.html §2): a hero with a running
                // intention and nothing new in its inbox simply keeps doing
                // what it was already doing (the always-running movement/
                // needs/combat systems, not the brain, carry it forward) --
                // the brain is consulted only on a real wake. Single-gateway
                // combat (docs/superpowers/specs/2026-07-25-contract-v3-
                // alignment-design.md): there is no separate combat path to
                // sequence against anymore -- should_wake's own high-stakes
                // clause (threat_was_present/MeleeLock, intention.h) already
                // means "consult every tick a fight is on", so engagement
                // (apply_intention's Attack case, intention.cpp) and swings
                // (BL_ACT_ATTACK -> resolve_action) both arrive through this
                // SAME wake, at the SAME cadence a dedicated pre-empt pass
                // used to run at.
                if (should_wake(g, e)) {
                    tick_wasm_brain(g, static_cast<uint32_t>(slot));
                }
                continue;
            }
            mock_think(g, e, static_cast<uint32_t>(slot));
        }

        // Drain AI commands enqueued during think, in one ordered pass (FIFO;
        // producers iterate by slot). This is the single mutation point for AI
        // decisions and appends each to command_log (the trace).
        apply_commands(g);
    }

    // Rebuild the navmesh if a building was placed/destroyed this tick (bumps
    // placement.nav_epoch). Cheap no-op when unchanged; the whole path/cost layer
    // reads from it, so it must be current before plan_paths.
    //
    // Gated on terrain_blocking, which is the world's "does terrain/obstacles stop
    // anyone" switch: with it off (make_flat_world, movement-mechanics tests that
    // predate terrain) the navmesh is left unbuilt and movement falls back to
    // obstacle-oblivious straight lines -- the documented flat-world contract.
    if (g.terrain_blocking) {
        rebuild_navmesh_if_stale(g);
    }

    // Navmesh movement pipeline: plan/follow durable MoveTargets, maintain melee
    // locks, and resolve unit-unit collisions. All exclude hidden (inside) heroes.
    plan_paths(g, dt);
    follow_paths(g, dt);
    update_melee_locks(g);
    separate_units(g);

    // Ranged shots in flight advance and resolve on arrival (melee already
    // resolved in the Attack command during the think pass). A pure system rule:
    // deterministic, runs identically live and on replay. Both damage sites
    // (fire_attack + advance_projectiles) emit the same DamageDealt/HeroDowned
    // events the old combat pass did.
    advance_projectiles(g, dt);

    // Intention contract: inbox TTL housekeeping + CurrentIntention
    // completion/abort detection (see intention.h). Placed AFTER
    // movement/combat so arrival (MoveTo) and a
    // just-landed-lethal-hit target (the dead-target abort) both see this
    // tick's final positions/hp, and BEFORE the death sweep below so a
    // target that died this very tick is still readable as "dead" via
    // Health<=0 rather than already destroyed -- entity_for_slot would
    // report it gone in the NEXT tick's advance_intentions regardless, but
    // checking hp here rather than validity is what lets the abort fire on
    // the same tick the kill happens.
    advance_intentions(g);

    // Death. Collect each dead entity's XP payout BEFORE the destroys
    // (Position/XpReward die with it), spread AFTER them so a hero that died
    // this tick neither blocks nor receives a share.
    std::vector<entt::entity> dead;
    std::vector<PendingKillXp> kill_xp;
    for (auto [e, health] : registry.view<const Health>().each()) {
        if (health.hp <= 0.0f) {
            dead.push_back(e);
            if (const auto* reward = registry.try_get<XpReward>(e);
                reward != nullptr && registry.all_of<Position>(e)) {
                kill_xp.push_back({registry.get<Position>(e).pos, reward->amount});
            }
        }
    }
    for (entt::entity e : dead) {
        registry.destroy(e);
    }
    spread_kill_xp(g, kill_xp);

    // Fog-of-war: resolve next visibility from the post-movement world state and
    // publish it. Newly-discovered texels credit the stamping CHARACTER with
    // exploration XP -- a system rule, applied here so it lands in the same
    // tick. resolve_vision reports every player-team stamper, hero or not;
    // award_xp is what actually applies the "only heroes gain XP" policy (a
    // no-op for non-heroes), so a townfolk/critter stamp is silently free.
    std::vector<DiscoveryCredit> discoveries;
    resolve_vision(g, &discoveries);
    if (g.factors.progression.xp_per_texel > 0) {
        const int32_t per_texel = g.factors.progression.xp_per_texel;
        for (const DiscoveryCredit& d : discoveries) {
            // Widen to int64 before multiplying: texels * xp_per_texel can
            // exceed int32 range (a wide reveal at a large per-texel reward);
            // award_xp saturates the accumulation from here.
            award_xp(g, d.slot, static_cast<int64_t>(d.texels) * per_texel);
        }
    }

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
    // Hero creatures (ids 0..HERO_CLASS_COUNT-1 == HeroClassId) carry their
    // class via desc.hero_class (the catalog defs author it), so spawn_entity
    // stamps HeroCharacter with the FINAL class at spawn time -- no post-spawn
    // patch needed (nor safe: spawn-time grants would have already run
    // against the stale value).
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
            .archetype = static_cast<int32_t>(archetype_of(g.registry, e)),
            .hero_class = hero ? hero->hero_class : -1,
            .facing_x = facing.x,
            .facing_z = facing.y,
            .vision_radius = vis_radius,
            .vision_cone_half_angle_deg = vis_half_deg,
            .level = sim ? sim->level : 0,
            .xp = sim ? sim->xp : 0,
            .xp_next = sim ? xp_to_next(g.factors.progression, sim->level) : 0,
        });
        const char* nm = disp ? disp->name.c_str() : "";
        std::size_t n = std::min(std::strlen(nm), sizeof(out.back().name) - 1);
        std::memcpy(out.back().name, nm, n);
        out.back().name[n] = '\0';

        const Skills* sk = g.registry.try_get<Skills>(e);
        CharacterState& row = out.back();
        row.skill_count = sk ? sk->count : 0;
        // Designated init above already zeroed row.skills (kMaxSkills entries),
        // so only [0, skill_count) needs writing -- the rest stay 0.
        for (int32_t i = 0; i < row.skill_count; ++i) {
            row.skills[i] = static_cast<int32_t>(sk->ids[i]);
        }
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

namespace {

// sanitize_factors: the single validation boundary for tunable factors.
// set_factors_of (below) is the one choke point every SimFactors write goes
// through -- Sim::SetFactors, and critically the factors.json load path
// (src/game/factors_manifest.cpp's LoadSimFactors is itself deliberately
// unvalidated: it type-checks the JSON shape but not the resulting numbers,
// see its own comments) -- so this is where a hand-edited manifest's mistakes
// get caught before they reach the sim. Compiled defaults (SimFactors'
// constructor) are already sane and pass through unchanged. Not declared in
// sim_internal.hpp: tests exercise it only through Sim::SetFactors/Factors.
//
// Rules:
//  - hero.think_max_millis/think_min_millis: vestigial (badlands_sim.hpp's
//    own comment on the fields has the full account -- the deliberation pause
//    they used to size is deleted, replaced by the intention contract) and
//    unread by decode_suggestion (wasm_brain.cpp), which has no think_max/
//    think_min check of any kind in v2. Clamped here purely to keep the pair
//    internally sane (max >= 0, min in [0, max]) for as long as the fields
//    stay in the manifest schema -- not because anything downstream still
//    relies on the invariant.
//  - hero.memory_ttl_millis: 0 means "remember only the tick you saw them"
//    (see the eviction comment in entity_memory.cpp) -- negative would evict
//    a just-seen entry (age 0) the same tick it was recorded.
//  - hours-rate fields that feed a DIVISION downstream (needs.cpp's
//    advance_needs -> reserve_rate_per_tick, components.h): floored at a
//    small positive epsilon rather than 0, so the field itself stays
//    strictly positive instead of leaning on reserve_rate_per_tick's own
//    <=0 "instantly" guard to stay finite.
//  - hero.explore_lease_millis: also a DIVISOR (hero_perception.cpp's
//    observe_hero computes `world_millis / explore_lease_millis`
//    UNCONDITIONALLY, for every hero, every tick, with no <=0 guard of its
//    own -- unlike the hours-rate fields above) -- floored at a small
//    positive integer rather than 0 for the same reason: 0 is a genuine
//    int64 divide-by-zero (UB/crash), not merely a degenerate rate.
//  - every remaining HeroFactors/CritterFactors/TownfolkFactors/MonsterFactors/
//    ProgressionFactors numeric field (radii, distances, durations, caps;
//    ProgressionFactors::xp_per_texel/kill_xp_radius/level_exponent):
//    negative is never meaningful, clamped to 0 -- this includes
//    MonsterFactors::max_alive, where a negative cap underflows through
//    economy.cpp's `live >= static_cast<uint32_t>(cap)` into a huge unsigned
//    value and silently DISABLES the spawn cap instead of capping at 0.
//    hero.weights[]/critter.weights and hero.explore_chance[] are
//    deliberately EXCLUDED from the clamp-to-0 sweep -- 0 is a meaningful
//    veto/"never" value for both, not a sign error (MonsterFactors has no
//    such field, so it carries no such exclusion).
//    TownfolkFactors::house_income_per_day (unsigned: no sign to sanitize)
//    is the one field left untouched.
//  - progression.level_base_xp: floored like the DIVISOR fields above, but
//    at 1 rather than a divisor's epsilon/1ms -- it scales xp_to_next's
//    leveling-curve threshold (floor(level_base_xp * L^level_exponent)), and
//    a base below 1 collapses every threshold to (near) 0 rather than merely
//    degenerating one rate, so it gets its own floor instead of joining the
//    clamp-to-0 sweep.
//
// A field is only warned about (old value -> new value) when sanitize
// actually moves it.
constexpr float kMinPositiveHours = 1e-3f;
constexpr int64_t kMinPositiveMillis = 1;

template <typename T>
void warn_adjusted(const char* field, T old_value, T new_value) {
    spdlog::warn("sanitize: {} adjusted from {} to {}", field, old_value, new_value);
}

// Sign-invalid scalar (radius/distance/duration/cap -- 0 always meaningful,
// negative never is): clamp to 0.
template <typename T>
void clamp_nonneg(const char* field, T& value) {
    if (value < T{0}) {
        warn_adjusted(field, value, T{0});
        value = T{0};
    }
}

// `value` is a DIVISOR downstream (needs.cpp's advance_needs ->
// reserve_rate_per_tick, components.h): floor at a small positive epsilon
// instead of 0.
void floor_positive_hours(const char* field, float& value) {
    if (value <= 0.0f) {
        warn_adjusted(field, value, kMinPositiveHours);
        value = kMinPositiveHours;
    }
}

// `value` is an integer-millis DIVISOR downstream (hero_perception.cpp's
// observe_hero: world_millis / explore_lease_millis): floor at the smallest
// positive millisecond instead of 0.
void floor_positive_millis(const char* field, int64_t& value) {
    if (value <= 0) {
        warn_adjusted(field, value, kMinPositiveMillis);
        value = kMinPositiveMillis;
    }
}

SimFactors sanitize_factors(SimFactors f) {
    HeroFactors& h = f.hero;

    clamp_nonneg("hero.think_max_millis", h.think_max_millis);
    if (h.think_min_millis < 0 || h.think_min_millis > h.think_max_millis) {
        const int64_t clamped = std::clamp<int64_t>(h.think_min_millis, 0, h.think_max_millis);
        warn_adjusted("hero.think_min_millis", h.think_min_millis, clamped);
        h.think_min_millis = clamped;
    }

    clamp_nonneg("hero.memory_ttl_millis", h.memory_ttl_millis);

    floor_positive_hours("hero.fatigue_drain_hours", h.fatigue_drain_hours);
    floor_positive_hours("hero.content_drain_hours", h.content_drain_hours);
    floor_positive_hours("hero.rest_fill_hours", h.rest_fill_hours);
    floor_positive_hours("hero.tavern_fill_hours", h.tavern_fill_hours);
    floor_positive_hours("hero.chat_fill_hours", h.chat_fill_hours);

    clamp_nonneg("hero.fatigue_seek", h.fatigue_seek);
    clamp_nonneg("hero.fatigue_seek_night", h.fatigue_seek_night);
    clamp_nonneg("hero.content_seek", h.content_seek);
    clamp_nonneg("hero.low_health_rest", h.low_health_rest);
    clamp_nonneg("hero.chat_content_seek", h.chat_content_seek);
    clamp_nonneg("hero.chat_content_ceiling", h.chat_content_ceiling);
    clamp_nonneg("hero.chat_sight", h.chat_sight);
    clamp_nonneg("hero.chat_radius", h.chat_radius);
    clamp_nonneg("hero.chat_duration", h.chat_duration);
    clamp_nonneg("hero.explore_min_fatigue", h.explore_min_fatigue);
    clamp_nonneg("hero.explore_min_distance", h.explore_min_distance);
    clamp_nonneg("hero.explore_max_distance", h.explore_max_distance);
    clamp_nonneg("hero.explore_search_radius", h.explore_search_radius);
    floor_positive_millis("hero.explore_lease_millis", h.explore_lease_millis);
    clamp_nonneg("hero.roam_radius", h.roam_radius);
    clamp_nonneg("hero.hunt_sight_radius", h.hunt_sight_radius);
    clamp_nonneg("hero.threat_radius", h.threat_radius);

    CritterFactors& c = f.critter;
    clamp_nonneg("critter.sight_radius", c.sight_radius);
    clamp_nonneg("critter.flee_radius", c.flee_radius);
    clamp_nonneg("critter.flee_distance", c.flee_distance);
    clamp_nonneg("critter.roam_radius", c.roam_radius);
    clamp_nonneg("critter.graze_fraction", c.graze_fraction);

    TownfolkFactors& t = f.townfolk;
    clamp_nonneg("townfolk.spawn_interval_millis", t.spawn_interval_millis);
    clamp_nonneg("townfolk.max_alive", t.max_alive);
    clamp_nonneg("townfolk.move_speed", t.move_speed);
    // house_income_per_day is unsigned -- no sign to sanitize.

    MonsterFactors& m = f.monster;
    clamp_nonneg("monster.spawn_interval_millis", m.spawn_interval_millis);
    // max_alive: see this function's doc comment -- a negative cap underflows
    // through economy.cpp's `live >= static_cast<uint32_t>(cap)` and silently
    // disables the spawn cap instead of capping at 0.
    clamp_nonneg("monster.max_alive", m.max_alive);

    ProgressionFactors& p = f.progression;
    clamp_nonneg("progression.xp_per_texel", p.xp_per_texel);
    clamp_nonneg("progression.kill_xp_radius", p.kill_xp_radius);
    clamp_nonneg("progression.level_exponent", p.level_exponent);
    // The curve's scale: xp_to_next floors its result at 1 anyway, but a base
    // below 1 collapses every threshold and the warn is the designer's signal.
    if (p.level_base_xp < 1) {
        warn_adjusted("progression.level_base_xp", p.level_base_xp, 1);
        p.level_base_xp = 1;
    }

    return f;
}

// The SetSkillCatalog validation boundary, sanitize_factors' sibling: the
// execution slice divides/waits on these, so negatives are clamped here.
SkillCatalog sanitize_skill_catalog(SkillCatalog c) {
    for (int32_t i = 0; i < kSkillCount; ++i) {
        SkillSpec& s = c.specs[i];
        clamp_nonneg("skill.duration_seconds", s.duration_seconds);
        clamp_nonneg("skill.cooldown_seconds", s.cooldown_seconds);
    }
    return c;
}

}  // namespace

void set_factors_of(BadlandsGame& g, const SimFactors& f) { g.factors = sanitize_factors(f); }
int32_t biome_at_of(const BadlandsGame& g, float x, float z) {
    return static_cast<int32_t>(biome_at(g, {x, z}));
}

SimStats stats_of(const BadlandsGame& g) {
    return SimStats{
        .ticks = g.ticks,
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

Sim::Sim(const BrainDesc& brain_desc) : world_(make_world(brain_desc)) {}
Sim::Sim(const WorldConfig& config, const BrainDesc& brain_desc)
    : world_(make_world(brain_desc, config)) {}
Sim::~Sim() = default;
Sim::Sim(Sim&&) noexcept = default;
Sim& Sim::operator=(Sim&&) noexcept = default;

uint32_t Sim::Spawn(const CharacterDesc& desc) { return spawn_into(*world_, desc); }
uint32_t Sim::SpawnCreature(CreatureId id, int32_t team, float pos_x, float pos_z) {
    return spawn_creature_into(*world_, id, team, {pos_x, pos_z});
}
void Sim::SetCreatureCatalog(const CreatureCatalog& catalog) { world_->creatures = catalog; }
const CreatureCatalog& Sim::Creatures() const { return world_->creatures; }
void Sim::SetSkillCatalog(const SkillCatalog& catalog) {
    world_->skills = sanitize_skill_catalog(catalog);
}
const SkillCatalog& Sim::Skills() const { return world_->skills; }
void Sim::Tick(float dt) {
    tick_world(*world_, dt);
    // Goal statistics are folded HERE, in the wrapper, from the very rows an
    // observer would read -- never inside tick_world. Counting is an
    // observation of the sim, not a part of it; see ActivityHistogram.
    characters_of(*world_, stats_scratch_);
    activity_stats_.Accumulate(stats_scratch_);
}
int64_t Sim::Dispatch(const Action& action) { return dispatch_into(*world_, action); }

std::vector<NavDebugCell> Sim::NavDebugCells() {
    // Ensure a current mesh even in flat/obstacle-oblivious worlds (debug tool).
    // Cheap no-op once built and epoch-current, so it is fine to call per frame.
    rebuild_navmesh_if_stale(*world_);
    std::vector<nav::NavMesh::DebugCell> cells;
    world_->navmesh.DebugCells(cells);
    std::vector<NavDebugCell> out;
    out.reserve(cells.size());
    for (const nav::NavMesh::DebugCell& c : cells) {
        out.push_back({c.min_world.x, c.min_world.y, c.max_world.x, c.max_world.y, c.cost,
                       c.passable});
    }
    return out;
}

NavPathResult Sim::NavQuery(float sx, float sz, float gx, float gz) {
    rebuild_navmesh_if_stale(*world_);
    const nav::NavMesh::PathResult r =
        world_->navmesh.FindPath({sx, sz}, {gx, gz});
    NavPathResult out;
    out.cost = r.cost;
    out.reachable = r.reachable;
    out.waypoints_xz.reserve(r.waypoints.size() * 2);
    for (const glm::vec2& w : r.waypoints) {
        out.waypoints_xz.push_back(w.x);
        out.waypoints_xz.push_back(w.y);
    }
    return out;
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

void Sim::DrainEvents(std::vector<GameEvent>& out) {
    out.clear();
    out.swap(world_->events);  // hand the batch out; the freed buffer refills next tick
}
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
