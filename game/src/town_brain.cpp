#include "town_brain.h"

#include "badlands_sim.hpp"
#include "behaviours/blocks.h"
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

WorldView observe_hero(const BadlandsGame& game, uint32_t slot, entt::entity e) {
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

    // Hunters perceive prey; other classes never populate it (Hunt not in list).
    if (game.registry.get<HeroCharacter>(e).hero_class == HERO_HUNTER) {
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

// The baseline hero block list, highest priority first. Argmax over these
// tiered scores reproduces the old GoHome > Buy > VisitTavern > Roam if-chain.
constexpr std::array<Candidate, 5> kHeroBlocks{{
    {score_go_home, act_go_home},
    {score_buy, act_buy},
    {score_visit_tavern, act_visit_tavern},
    {score_roam, act_roam},
    {score_idle, act_idle},
}};

// The hunter's list: the baseline plus Hunt (tiered below GoHome, above the
// errands) -- so a hunter rests when tired but otherwise hunts before shopping.
constexpr std::array<Candidate, 6> kHunterBlocks{{
    {score_go_home, act_go_home},
    {score_hunt, act_hunt},
    {score_buy, act_buy},
    {score_visit_tavern, act_visit_tavern},
    {score_roam, act_roam},
    {score_idle, act_idle},
}};

}  // namespace

void town_think(BadlandsGame& game, uint32_t slot) {
    entt::entity e = entity_for_slot(game, static_cast<int32_t>(slot));
    if (e == entt::null) {
        return;
    }
    const WorldView view = observe_hero(game, slot, e);
    // Class picks the block list: same shared blocks, hunters get one more.
    const BehaviourResult r =
        (game.registry.get<HeroCharacter>(e).hero_class == HERO_HUNTER)
            ? select_argmax(kHunterBlocks, view, game.factors)
            : select_argmax(kHeroBlocks, view, game.factors);

    // Decisions go out as commands like every other mutation (logged +
    // replayable), never as direct writes. Edge-triggered: re-stating an
    // unchanged goal is not a decision and must not enter the trace.
    enqueue_set_behavior(game, slot, static_cast<int32_t>(r.id));
    enqueue_move_to(game, slot, r.target);
    if (r.follow_up.has_value() &&
        (!r.follow_up_on_arrival || glm::distance(view.pos, r.target) <= kEntranceRadius)) {
        game.command_queue.push_back(*r.follow_up);
    }
}

}  // namespace badlands
