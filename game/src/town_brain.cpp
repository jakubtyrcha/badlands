#include "town_brain.h"

#include "badlands_game.h"
#include "command.h"
#include "components.h"
#include "game_state.h"
#include "placement.h"

#include <cmath>

#include <entt/entt.hpp>

namespace badlands {

namespace {

// Behaviour thresholds — policy placeholders (see town_brain.h), not the
// architecture. Tuned so heroes sleep when tired / at night and seek the tavern
// when bored during the day.
constexpr float kFatigueGoHome = 0.6f;   // tired enough to head home by day
constexpr float kFatigueNight = 0.2f;    // lower bar to go home once it is night
constexpr float kBoredomTavern = 0.5f;   // bored enough to seek the tavern
constexpr float kRoamRadius = 6.0f;

// xorshift64 step (deterministic; seed must be non-zero).
uint64_t xorshift(uint64_t& s) {
    s ^= s << 13;
    s ^= s >> 7;
    s ^= s << 17;
    return s;
}
float unit(uint64_t& s) {
    return static_cast<float>(xorshift(s) >> 40) * (1.0f / 16777216.0f);  // [0,1)
}

// Approach-tile ("door") of the nearest alive building of `kind` to `pos`.
bool door_of_kind(const BadlandsGame& game, int kind, glm::vec2 pos, glm::vec2& out) {
    uint32_t bid = nearest_building_of(game.placement, kind, pos);
    if (bid == UINT32_MAX) {
        return false;
    }
    return building_approach_tile(game.placement, game.placement.buildings[bid], out);
}

}  // namespace

void town_think(BadlandsGame& game, uint32_t slot) {
    entt::entity e = entity_for_slot(game, static_cast<int32_t>(slot));
    if (e == entt::null) {
        return;
    }
    auto& sim = game.registry.get<HeroSimulationState>(e);
    glm::vec2 pos = game.registry.get<Position>(e).pos;
    const float tod = time_of_day(game.world_millis);
    const bool night = is_night(tod);

    glm::vec2 home_door{};
    bool has_home = false;
    if (sim.home_building_id >= 0 &&
        static_cast<size_t>(sim.home_building_id) < game.placement.buildings.size() &&
        game.placement.buildings[sim.home_building_id].alive) {
        has_home = building_approach_tile(
            game.placement, game.placement.buildings[sim.home_building_id], home_door);
    }
    glm::vec2 apo_door{};
    bool has_apo = door_of_kind(game, GAME_BUILDING_APOTHECARY, pos, apo_door);
    glm::vec2 tavern_door{};
    bool has_tavern = door_of_kind(game, GAME_BUILDING_TAVERN, pos, tavern_door);

    Behavior chosen = Behavior::Idle;
    glm::vec2 target = pos;
    Command follow_up{};
    bool have_follow_up = false;

    if (has_home && (sim.fatigue >= kFatigueGoHome || (night && sim.fatigue >= kFatigueNight))) {
        chosen = Behavior::GoHome;
        target = home_door;
        follow_up = {CommandKind::EnterHome, slot};
        have_follow_up = true;
    } else if (has_apo && sim.inventory < kInventoryCap) {
        chosen = Behavior::Buy;
        target = apo_door;
        follow_up = {CommandKind::Buy, slot};
        have_follow_up = true;
    } else if (has_tavern && !night && sim.boredom >= kBoredomTavern) {
        chosen = Behavior::VisitTavern;
        target = tavern_door;
        follow_up = {CommandKind::EnterBuilding, slot, UINT32_MAX, {0.0f, 0.0f},
                     GAME_BUILDING_TAVERN};
        have_follow_up = true;
    } else {
        chosen = Behavior::Roam;
        // Roam around the home (or origin), re-drawn each ~2 s window so the goal
        // is stable (no per-tick repath jitter) yet deterministic.
        glm::vec2 anchor = has_home ? home_door : glm::vec2{0.0f, 0.0f};
        uint64_t s = (static_cast<uint64_t>(slot) * 2654435761ull) ^
                     (static_cast<uint64_t>(game.world_millis / 2000) + 1ull);
        if (s == 0) {
            s = 1;
        }
        float ang = unit(s) * 6.2831853f;
        float rad = unit(s) * kRoamRadius;
        target = anchor + glm::vec2{std::cos(ang) * rad, std::sin(ang) * rad};
    }

    // Decisions go out as commands like every other mutation (so they are logged
    // + replayable), never as direct writes. Edge-triggered: re-stating an
    // unchanged goal is not a decision and must not enter the trace.
    enqueue_set_behavior(game, slot, static_cast<int32_t>(chosen));
    enqueue_move_to(game, slot, target);
    // The enter/buy handler re-checks proximity authoritatively; emitting only
    // once in range keeps the walk out of the trace (one attempt per arrival,
    // not one per tick of the journey).
    if (have_follow_up && glm::distance(pos, target) <= kEntranceRadius) {
        game.command_queue.push_back(follow_up);
    }
}

}  // namespace badlands
