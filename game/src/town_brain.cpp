#include "town_brain.h"

#include "badlands_sim.hpp"
#include "behaviours/blocks.h"
#include "behaviours/deliberation.h"
#include "behaviours/perception.h"
#include "behaviours/selectors.h"
#include "behaviours/world_view.h"
#include "command.h"
#include "components.h"
#include "heroes.h"  // HERO_HUNTER
#include "game_state.h"
#include "placement.h"

#include <array>

#include <entt/entt.hpp>
#include <glm/glm.hpp>

namespace badlands {

namespace {

// Roam re-draws its goal only when this window rolls over (world_millis / lease),
// so a wanderer holds a stable target for ~2 s instead of jittering each tick.
constexpr int64_t kRoamLeaseMillis = 2000;

// Approach-tile ("door") of the nearest alive building of `kind` to `pos`.
bool door_of_kind(const BadlandsGame& game, int kind, glm::vec2 pos, glm::vec2& out) {
    uint32_t bid = nearest_building_of(game.placement, kind, pos);
    if (bid == UINT32_MAX) {
        return false;
    }
    return building_approach_tile(game.placement, game.placement.buildings[bid], out);
}

// Build the hero's perception. This is the ONLY place town_think reads the
// registry/placement; blocks see only the returned WorldView.
// Nearest deer (critter) within `radius` of `pos`, by slot: what a hunter hunts.
// Perception only -- reads the registry so the Hunt block never does.
bool nearest_prey(const BadlandsGame& game, glm::vec2 pos, float radius, glm::vec2& out_pos,
                  uint32_t& out_slot, float& out_dist) {
    float best = radius;
    bool found = false;
    for (uint32_t slot = 0; slot < game.slots.size(); ++slot) {
        entt::entity e = game.slots[slot];
        if (!game.registry.valid(e) || !game.registry.all_of<CritterState>(e)) {
            continue;
        }
        const glm::vec2 p = game.registry.get<Position>(e).pos;
        const float d = glm::distance(pos, p);
        if (d <= best) {
            best = d;
            out_pos = p;
            out_slot = slot;
            found = true;
        }
    }
    out_dist = found ? best : 0.0f;
    return found;
}

// The actor's preference table: which class this hero is. Everything
// class-specific about a hero's decisions flows from here.
const ActivityWeights& weights_for(const BadlandsGame& game, entt::entity e) {
    const int32_t cls = game.registry.get<HeroCharacter>(e).hero_class;
    const int32_t idx = (cls >= 0 && cls < HERO_CLASS_COUNT) ? cls : HERO_MERCENARY;
    return game.factors.hero.weights[idx];
}

WorldView observe_hero(const BadlandsGame& game, uint32_t slot, entt::entity e,
                       const ActivityWeights& weights) {
    const auto& sim = game.registry.get<HeroSimulationState>(e);
    WorldView v;
    v.slot = slot;
    v.pos = game.registry.get<Position>(e).pos;
    v.fatigue = sim.fatigue;
    v.boredom = sim.boredom;
    v.inventory = sim.inventory;
    v.self_attack_range = game.registry.get<Stats>(e).attack_range;
    v.tod = time_of_day(game.world_millis);
    v.night = is_night(v.tod);
    v.roam_epoch = game.world_millis / kRoamLeaseMillis;
    v.now_millis = game.world_millis;
    v.think_until_millis = sim.think_until_millis;
    v.current_activity = sim.behavior;

    // Threats in proximity. Today this gates deliberation only -- a hero with a
    // hostile anywhere on the map never reaches town_think at all, because the
    // combat pre-empt in mock_think claims it first, so in practice this list
    // is empty here. It is populated regardless because it is the perception
    // the Danger band is built on, and it goes live unchanged the moment combat
    // becomes an activity rather than a pre-empt.
    collect_threats(game, e, v.pos, game.factors.hero.threat_radius, ThreatPolicy::HostileTeam,
                    v);

    // Perception follows the weight table: an activity the class does not have
    // (weight 0) costs nothing to perceive for. Only hunters scan for prey --
    // and that is now a data consequence, not a hardcoded class check.
    if (weights.of(ActivityId::Hunt) > 0.0f) {
        v.has_prey = nearest_prey(game, v.pos, game.factors.hero.hunt_sight_radius,
                                  v.prey_pos, v.prey_slot, v.prey_dist);
    }

    if (sim.home_building_id >= 0 &&
        static_cast<size_t>(sim.home_building_id) < game.placement.buildings.size() &&
        game.placement.buildings[sim.home_building_id].alive) {
        v.has_home = building_approach_tile(
            game.placement, game.placement.buildings[sim.home_building_id], v.home_door);
    }
    v.has_apothecary = door_of_kind(game, static_cast<int32_t>(BuildingKind::Apothecary),
                                    v.pos, v.apothecary_door);
    v.has_tavern =
        door_of_kind(game, static_cast<int32_t>(BuildingKind::Tavern), v.pos, v.tavern_door);

    // Wander anchor: home when there is one, else the origin. The shared Roam
    // block walks to roam_goal; the draw math lives here (perception) so the
    // block never touches the registry.
    const glm::vec2 anchor = v.has_home ? v.home_door : glm::vec2{0.0f, 0.0f};
    v.roam_goal = roam_point(slot, v.roam_epoch, anchor, game.factors.hero.roam_radius);
    return v;
}

// EVERY hero class runs this one table -- there is no per-class list. What a
// class does, how eagerly, and whether it has an activity at all is entirely
// the weight table (SimFactors::hero.weights); a Hunt weight of 0 makes Hunt
// invisible to that class. Adding an activity is a row here plus a weight.
//
// List order is the tie-break only. Priority is the band; preference is the
// weight.
constexpr std::array<ActivityDef, 6> kHeroActivities{{
    {ActivityId::GoHome, ActivityBand::Filler, score_go_home, act_go_home},
    {ActivityId::Hunt, ActivityBand::Filler, score_hunt, act_hunt},
    {ActivityId::Buy, ActivityBand::Filler, score_buy, act_buy},
    {ActivityId::VisitTavern, ActivityBand::Filler, score_visit_tavern, act_visit_tavern},
    {ActivityId::Roam, ActivityBand::Filler, score_roam, act_roam},
    {ActivityId::Idle, ActivityBand::Fallback, score_idle, act_idle},
}};

}  // namespace

std::span<const ActivityDef> hero_activities() { return kHeroActivities; }

void town_think(BadlandsGame& game, uint32_t slot) {
    entt::entity e = entity_for_slot(game, static_cast<int32_t>(slot));
    if (e == entt::null) {
        return;
    }
    const ActivityWeights& weights = weights_for(game, e);
    const WorldView view = observe_hero(game, slot, e, weights);
    const BehaviourResult r = select_banded(kHeroActivities, weights, view, game.factors);

    // Changed its mind? Stand and think about it for a moment first.
    const ThinkDecision think = deliberate(r.id, view, game.factors);
    if (think.pause) {
        if (think.duration_millis > 0) {
            // Starting a pause. Both commands go out exactly once: the hold
            // position is captured here rather than re-stated each tick, so a
            // hero nudged by unit separation mid-pause does not spray MoveTo
            // commands into the trace.
            enqueue_set_behavior(game, slot, static_cast<int32_t>(ActivityId::Think),
                                 think.duration_millis);
            enqueue_move_to(game, slot, view.pos);
        }
        return;  // mid-pause: no decision to make, nothing to log
    }

    // Decisions go out as commands like every other mutation (logged +
    // replayable), never as direct writes. Edge-triggered: re-stating an
    // unchanged goal is not a decision and must not enter the trace.
    // Committing here also clears any pause (the hero was thinking, so the
    // behaviour differs and the command fires).
    enqueue_set_behavior(game, slot, static_cast<int32_t>(r.id));
    enqueue_move_to(game, slot, r.target);
    if (r.follow_up.has_value() &&
        (!r.follow_up_on_arrival || glm::distance(view.pos, r.target) <= kEntranceRadius)) {
        game.command_queue.push_back(*r.follow_up);
    }
}

}  // namespace badlands
