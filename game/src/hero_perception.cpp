#include "hero_perception.h"

#include "badlands_sim.hpp"
#include "behaviours/blocks.h"
#include "behaviours/perception.h"
#include "behaviours/rng.h"
#include "behaviours/world_view.h"
#include "command.h"
#include "components.h"
#include "exploration.h"
#include "game_state.h"
#include "placement.h"
#include "vision.h"

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
// hero_perception.h) so the wasm path packs the same weights row it sends
// over the wire from the identical lookup, rather than a copy that could
// drift.
const ActivityWeights& weights_for(const BadlandsGame& game, entt::entity e) {
    const int32_t cls = game.registry.get<HeroCharacter>(e).hero_class;
    const int32_t idx = (cls >= 0 && cls < HERO_CLASS_COUNT) ? cls : HERO_MERCENARY;
    return game.factors.hero.weights[idx];
}

// Build the hero's perception. This is the ONLY place a hero brain (the wasm
// one, via the wire) reads the registry/placement; a wasm module sees only
// the returned WorldView. Exposed (see hero_perception.h) so wasm_brain.cpp
// reuses it verbatim.
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

    // Threats in proximity: the perception the Danger band (and, on the wasm
    // path, a guaranteed-defend suggestion) is built on. Excludes critters
    // (CritterState) the same way nearest_enemy already does (game.cpp) --
    // deer are neutral wildlife, never a team-combat threat, so only a
    // hunter engages them, via has_prey/the Hunt activity below.
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
    if (v.chatting) {
        // Finding B (V6): expose the ACTUAL partner while mid-conversation,
        // not a fresh nearest_companion scan -- that would find nobody
        // (an entity already chatting is excluded as a candidate, see
        // nearest_companion's own ChattingState exclusion above), which is
        // exactly why this used to be skipped outright whenever v.chatting
        // was true. Reading it straight off ChattingState instead is what
        // lets the brain restate BL_INT_CHAT at the right target_slot on a
        // mid-chat wake instead of losing track of the session (see
        // actChat's own comment, scripts/brains/nim/blocks.nim, and
        // is_identical_restatement, intention.cpp, which is what actually
        // makes that restatement a clean resume: this value now matches
        // CurrentIntention::target_slot stamped at adoption).
        v.partner_slot = game.registry.get<ChattingState>(e).partner_slot;
    } else if (weights.of(ActivityId::Chat) > 0.0f &&
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

}  // namespace badlands
