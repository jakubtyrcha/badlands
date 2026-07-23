#include "townfolk_brain.h"

#include "badlands_sim.hpp"
#include "behaviours/blocks.h"
#include "behaviours/selectors.h"
#include "behaviours/world_view.h"
#include "command.h"
#include "components.h"
#include "game_state.h"
#include "nav_world.h"  // nav_cost
#include "placement.h"

#include <array>
#include <cstddef>

#include <entt/entt.hpp>
#include <glm/glm.hpp>

namespace badlands {

namespace {

bool is_visited(const TaxCollectorState& tc, uint32_t bid) {
    for (uint32_t v : tc.visited) {
        if (v == bid) {
            return true;
        }
    }
    return false;
}

// Cheapest-to-reach alive building that still owes tax and has not been
// collected this round -- ranked by TRAVEL cost (nav_cost), so the collector
// routes around the lake/buildings and prefers easy terrain rather than picking
// something that is close as the crow flies but a long walk. Unreachable
// buildings are skipped. Ties break by ascending id (iterate ascending, strict-
// less on cost), so the route is deterministic.
bool nearest_taxable(const BadlandsGame& game, const TaxCollectorState& tc, glm::vec2 pos,
                     glm::vec2& out_door, uint32_t& out_id) {
    float best = 0.0f;
    bool found = false;
    const auto& bs = game.placement.buildings;
    for (uint32_t i = 0; i < bs.size(); ++i) {
        if (!bs[i].alive || bs[i].taxable_income == 0 || is_visited(tc, i)) {
            continue;
        }
        glm::vec2 door;
        if (!building_approach_tile(game.placement, bs[i], door)) {
            continue;
        }
        const float d = nav_cost(game, pos, door);
        if (d >= nav::kImpassable) {
            continue;  // cannot get there: not a candidate
        }
        if (!found || d < best) {
            best = d;
            out_door = door;
            out_id = i;
            found = true;
        }
    }
    return found;
}

// Nearest alive Castle or Watchtower door -- where the carry is banked.
bool nearest_deposit(const BadlandsGame& game, glm::vec2 pos, glm::vec2& out_door) {
    float best = 0.0f;
    bool found = false;
    const auto& bs = game.placement.buildings;
    for (uint32_t i = 0; i < bs.size(); ++i) {
        if (!bs[i].alive) {
            continue;
        }
        if (bs[i].kind != static_cast<int32_t>(BuildingKind::Castle) &&
            bs[i].kind != static_cast<int32_t>(BuildingKind::Watchtower)) {
            continue;
        }
        glm::vec2 door;
        if (!building_approach_tile(game.placement, bs[i], door)) {
            continue;
        }
        const float d = nav_cost(game, pos, door);
        if (d >= nav::kImpassable) {
            continue;  // unreachable deposit point
        }
        if (!found || d < best) {
            best = d;
            out_door = door;
            found = true;
        }
    }
    return found;
}

WorldView observe_townfolk(const BadlandsGame& game, uint32_t slot, entt::entity e) {
    const auto& tc = game.registry.get<TaxCollectorState>(e);
    WorldView v;
    v.slot = slot;
    v.pos = game.registry.get<Position>(e).pos;
    v.has_tax_target = nearest_taxable(game, tc, v.pos, v.tax_target_door, v.tax_target_id);
    v.has_deposit = nearest_deposit(game, v.pos, v.deposit_door);
    return v;
}

// Finish the round (visit every taxable building) before returning to bank it.
constexpr std::array<Candidate, 3> kTownfolkBlocks{{
    {score_visit_taxable, act_visit_taxable},
    {score_deposit, act_deposit},
    {score_idle, act_idle},
}};

}  // namespace

void townfolk_think(BadlandsGame& game, uint32_t slot) {
    entt::entity e = entity_for_slot(game, static_cast<int32_t>(slot));
    if (e == entt::null || !game.registry.all_of<TaxCollectorState>(e)) {
        return;
    }
    const WorldView view = observe_townfolk(game, slot, e);
    const BehaviourResult r = select_priority(kTownfolkBlocks, view, game.factors);

    enqueue_set_behavior(game, slot, static_cast<int32_t>(r.id));
    enqueue_move_to(game, slot, r.target);
    if (r.follow_up.has_value() &&
        (!r.follow_up_on_arrival || glm::distance(view.pos, r.target) <= kEntranceRadius)) {
        game.command_queue.push_back(*r.follow_up);
    }
}

}  // namespace badlands
