#include "monster_brain.h"

#include "badlands_sim.hpp"
#include "command.h"
#include "components.h"
#include "game_state.h"
#include "placement.h"

#include <cmath>

#include <entt/entt.hpp>
#include <glm/glm.hpp>

namespace badlands {

namespace {

// Nearest alive building a monster may attack (enemy_targettable: Castle/House),
// by distance to its approach tile ("door") -- the tile the rat walks to and
// bites from. Returns its id and door.
bool nearest_targettable(const BadlandsGame& game, glm::vec2 pos, uint32_t& out_id,
                         glm::vec2& out_door) {
    float best = 0.0f;
    bool found = false;
    const auto& bs = game.placement.buildings;
    for (uint32_t i = 0; i < bs.size(); ++i) {
        if (!bs[i].alive ||
            !BuildingDefOf(static_cast<BuildingKind>(bs[i].kind)).enemy_targettable) {
            continue;
        }
        glm::vec2 door;
        if (!building_approach_tile(game.placement, bs[i], door)) {
            continue;
        }
        const float d = glm::distance(pos, door);
        if (!found || d < best) {
            best = d;
            out_id = i;
            out_door = door;
            found = true;
        }
    }
    return found;
}

}  // namespace

void monster_think(BadlandsGame& game, uint32_t slot) {
    entt::entity e = entity_for_slot(game, static_cast<int32_t>(slot));
    if (e == entt::null) {
        return;
    }
    const glm::vec2 pos = game.registry.get<Position>(e).pos;

    uint32_t bid = UINT32_MAX;
    glm::vec2 door{};
    if (!nearest_targettable(game, pos, bid, door)) {
        game.registry.get<MoveTarget>(e).kind = MoveTarget::Kind::None;
        return;  // nothing to gnaw
    }

    // Walk to the door; the shared movement pipeline gets us there.
    enqueue_move_to(game, slot, door);

    // At the door and off cooldown -> swing (command-sourced, so building combat
    // is logged/replayable). Gated on arrival + cooldown, so the log gets one
    // entry per actual hit, not one per tick of the approach.
    const auto& stats = game.registry.get<Stats>(e);
    const bool at_door = glm::distance(pos, door) <= stats.attack_range + kEntranceRadius;
    if (at_door && game.registry.get<CooldownTimer>(e).remaining <= 0.0f) {
        game.command_queue.push_back({CommandKind::AttackBuilding, slot, bid});
    }
}

}  // namespace badlands
