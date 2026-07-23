#include "town_brain.h"

#include "badlands_sim.hpp"
#include "behaviours/blocks.h"
#include "behaviours/deliberation.h"
#include "behaviours/perception.h"
#include "behaviours/rng.h"
#include "behaviours/selectors.h"
#include "behaviours/world_view.h"
#include "command.h"
#include "components.h"
#include "exploration.h"
#include "heroes.h"  // HERO_HUNTER
#include "game_state.h"
#include "placement.h"
#include "vision.h"

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

// Nearest other hero who is also bored enough to want company, and is free to
// give it. Slot order with a strict-less distance test, so if two heroes are
// equidistant both pick the lower slot -- which is what makes the pairing
// agree from both sides without any negotiation.
bool nearest_companion(const BadlandsGame& game, entt::entity self, glm::vec2 pos,
                       float content_bar, float radius, glm::vec2& out_pos, uint32_t& out_slot,
                       float& out_dist) {
    bool found = false;
    for (uint32_t slot = 0; slot < game.slots.size(); ++slot) {
        const entt::entity e = game.slots[slot];
        if (e == self || !game.registry.valid(e)) {
            continue;
        }
        const auto* sim = game.registry.try_get<HeroSimulationState>(e);
        // A companion worth approaching is one who ALSO wants company: its
        // content reserve has dipped below the same bar.
        if (sim == nullptr || sim->content >= content_bar) {
            continue;
        }
        if (game.registry.any_of<InsideBuilding, ChattingState>(e)) {
            continue;  // hidden, or already in someone else's conversation
        }
        const glm::vec2 p = game.registry.get<Position>(e).pos;
        const float d = glm::distance(pos, p);
        if (d > radius || (found && d >= out_dist)) {
            continue;
        }
        out_pos = p;
        out_slot = slot;
        out_dist = d;
        found = true;
    }
    return found;
}

}  // namespace

// The actor's preference table: which class this hero is. Everything
// class-specific about a hero's decisions flows from here. Exposed (see
// town_brain.h) so the wasm path packs the same weights row it sends over the
// wire from the identical lookup, rather than a copy that could drift.
const ActivityWeights& weights_for(const BadlandsGame& game, entt::entity e) {
    const int32_t cls = game.registry.get<HeroCharacter>(e).hero_class;
    const int32_t idx = (cls >= 0 && cls < HERO_CLASS_COUNT) ? cls : HERO_MERCENARY;
    return game.factors.hero.weights[idx];
}

// Build the hero's perception. This is the ONLY place a hero brain (this file
// or the wasm one) reads the registry/placement; blocks -- and a wasm module,
// via the wire -- see only the returned WorldView. Exposed (see town_brain.h)
// so wasm_brain.cpp reuses it verbatim: perception must be identical on both
// paths, or a hero would decide differently for a reason that has nothing to
// do with its decision logic.
WorldView observe_hero(const BadlandsGame& game, uint32_t slot, entt::entity e,
                       const ActivityWeights& weights) {
    const auto& sim = game.registry.get<HeroSimulationState>(e);
    WorldView v;
    v.slot = slot;
    v.pos = game.registry.get<Position>(e).pos;
    v.fatigue = sim.fatigue;
    v.content = sim.content;
    const Health& hp = game.registry.get<Health>(e);
    v.health_frac = hp.max_hp > 0.0f ? hp.hp / hp.max_hp : 1.0f;
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

    // Exploration. Two draws off the same lease window, so a hero commits to
    // one errand into the unknown for a while rather than re-deciding each
    // tick: first whether it feels like going at all (per-class appetite --
    // this lives in perception because only here is the class known), then
    // where. The blocked event stays relevant only for the window it happened
    // in, so a refusal makes the hero try somewhere else next window instead of
    // giving up on exploring forever.
    const int32_t cls = game.registry.get<HeroCharacter>(e).hero_class;
    const int64_t explore_epoch = game.world_millis / game.factors.hero.explore_lease_millis;
    if (weights.of(ActivityId::Explore) > 0.0f) {
        uint64_t rng = seed_of(slot, explore_epoch * 2 + 1);
        const float appetite =
            game.factors.hero
                .explore_chance[(cls >= 0 && cls < HERO_CLASS_COUNT) ? cls : HERO_MERCENARY];
        if (unit_float(rng) < appetite) {
            if (auto goal = pick_exploration_target(game.vision, v.pos,
                                                    seed_of(slot, explore_epoch),
                                                    game.factors.hero)) {
                v.explore_goal = *goal;
                v.has_explore_goal = true;
            }
        }
    }
    if (const auto* blocked = game.registry.try_get<MoveBlocked>(e)) {
        v.move_blocked =
            blocked->at_millis / game.factors.hero.explore_lease_millis == explore_epoch;
        v.blocked_point = blocked->point;
    }

    // Company: an in-progress conversation runs to its own clock, so only look
    // for someone new when not already talking (and only if the class is
    // sociable at all).
    v.chatting = game.registry.all_of<ChattingState>(e);
    if (!v.chatting && weights.of(ActivityId::Chat) > 0.0f &&
        v.content < game.factors.hero.chat_content_seek) {
        v.has_chat_partner =
            nearest_companion(game, e, v.pos, game.factors.hero.chat_content_seek,
                              game.factors.hero.chat_sight, v.partner_pos, v.partner_slot,
                              v.partner_dist);
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

namespace {

// EVERY hero class runs this one table -- there is no per-class list. What a
// class does, how eagerly, and whether it has an activity at all is entirely
// the weight table (SimFactors::hero.weights); a Hunt weight of 0 makes Hunt
// invisible to that class. Adding an activity is a row here plus a weight.
//
// List order is the tie-break only. Priority is the band; preference is the
// weight.
constexpr std::array<ActivityDef, 8> kHeroActivities{{
    {ActivityId::Explore, ActivityBand::Normal, score_explore, act_explore},
    {ActivityId::GoHome, ActivityBand::Normal, score_go_home, act_go_home},
    {ActivityId::Hunt, ActivityBand::Normal, score_hunt, act_hunt},
    {ActivityId::Buy, ActivityBand::Normal, score_buy, act_buy},
    {ActivityId::VisitTavern, ActivityBand::Normal, score_visit_tavern, act_visit_tavern},
    {ActivityId::Chat, ActivityBand::Normal, score_chat, act_chat},
    {ActivityId::Roam, ActivityBand::Normal, score_roam, act_roam},
    {ActivityId::Idle, ActivityBand::Normal, score_idle, act_idle},
}};

}  // namespace

std::span<const ActivityDef> hero_activities() { return kHeroActivities; }

// The shared decision-apply seam (see town_brain.h's BrainDecision doc):
// commits a decision to Commands via the same edge-triggered producers every
// brain uses, or -- if the decision is a pause -- starts/continues one. This
// is byte-for-byte town_think's former tail, generalized to take its inputs
// as a BrainDecision instead of reading (BehaviourResult, ThinkDecision)
// directly, so the wasm path (which has no BehaviourResult/ThinkDecision of
// its own -- only a decoded BlDecisionWire) can drive it too.
void apply_brain_decision(BadlandsGame& game, uint32_t slot, glm::vec2 self_pos,
                          const BrainDecision& decision) {
    if (decision.pause) {
        if (decision.pause_duration_millis > 0) {
            // Starting a pause. Both commands go out exactly once: the hold
            // position is captured here rather than re-stated each tick, so a
            // hero nudged by unit separation mid-pause does not spray MoveTo
            // commands into the trace.
            enqueue_set_behavior(game, slot, static_cast<int32_t>(ActivityId::Think),
                                 decision.pause_duration_millis);
            enqueue_move_to(game, slot, self_pos);
        }
        return;  // mid-pause: no decision to make, nothing to log
    }

    // Decisions go out as commands like every other mutation (logged +
    // replayable), never as direct writes. Edge-triggered: re-stating an
    // unchanged goal is not a decision and must not enter the trace.
    // Committing here also clears any pause (the hero was thinking, so the
    // behaviour differs and the command fires).
    enqueue_set_behavior(game, slot, static_cast<int32_t>(decision.activity));
    enqueue_move_to(game, slot, decision.goal);
    if (decision.follow_up.has_value() &&
        (!decision.follow_up_on_arrival ||
         glm::distance(self_pos, decision.goal) <= kEntranceRadius)) {
        game.command_queue.push_back(*decision.follow_up);
    }
}

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

    BrainDecision decision;
    decision.activity = r.id;
    decision.goal = r.target;
    decision.follow_up = r.follow_up;
    decision.follow_up_on_arrival = r.follow_up_on_arrival;
    decision.pause = think.pause;
    decision.pause_duration_millis = think.duration_millis;
    apply_brain_decision(game, slot, view.pos, decision);
}

}  // namespace badlands
