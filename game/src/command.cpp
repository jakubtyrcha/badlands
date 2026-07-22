#include "command.h"

#include "components.h"
#include "game_state.h"
#include "heroes.h"
#include "placement.h"

#include <entt/entt.hpp>

#include <algorithm>
#include <limits>

namespace badlands {

int64_t apply_command(BadlandsGame& game, const Command& cmd) {
    // Log the command (the trace / replay input) before applying it, stamped
    // with the sim time it took effect.
    game.command_log.push_back(cmd);
    game.command_log.back().at_millis = game.world_millis;

    constexpr auto kNoId = std::numeric_limits<uint32_t>::max();

    switch (cmd.kind) {
        // --- player commands (apply synchronously via game_dispatch) ---------
        case CommandKind::PlaceBuilding: {
            GamePlacementDesc desc{cmd.param_a, cmd.param_b, cmd.point.x, cmd.point.y};
            uint32_t id = place_building(game, desc, /*player=*/true);
            return (id == kNoId) ? -1 : static_cast<int64_t>(id);
        }
        case CommandKind::RecruitHero: {
            uint32_t id = recruit(game, cmd.target_id);
            return (id == kNoId) ? -1 : static_cast<int64_t>(id);
        }
        case CommandKind::DestroyBuilding:
            return destroy_building_impl(game, cmd.target_id);

        // --- unit commands (enqueued by brains, drained in the tick) ---------
        case CommandKind::MoveTo: {
            entt::entity e = entity_for_slot(game, static_cast<int32_t>(cmd.actor));
            if (e == entt::null) {
                return 0;
            }
            MoveTarget& mt = game.registry.get<MoveTarget>(e);
            mt.kind = MoveTarget::Kind::Point;
            mt.point = cmd.point;
            mt.entity = entt::null;
            mt.building = UINT32_MAX;
            mt.stop_distance = 0.0f;
            return 0;
        }
        case CommandKind::EnterBuilding: {
            entt::entity e = entity_for_slot(game, static_cast<int32_t>(cmd.actor));
            if (e != entt::null) {
                hero_enter(game, e, cmd.param_a);
            }
            return 0;
        }
        case CommandKind::EnterHome: {
            entt::entity e = entity_for_slot(game, static_cast<int32_t>(cmd.actor));
            if (e != entt::null) {
                hero_enter_home(game, e);
            }
            return 0;
        }
        case CommandKind::Buy: {
            entt::entity e = entity_for_slot(game, static_cast<int32_t>(cmd.actor));
            if (e != entt::null) {
                hero_buy(game, e);
            }
            return 0;
        }
        case CommandKind::SetBehavior: {
            entt::entity e = entity_for_slot(game, static_cast<int32_t>(cmd.actor));
            if (e != entt::null) {
                if (auto* sim = game.registry.try_get<HeroSimulationState>(e)) {
                    sim->behavior = cmd.param_a;
                }
            }
            return 0;
        }
        case CommandKind::Attack: {
            entt::entity e = entity_for_slot(game, static_cast<int32_t>(cmd.actor));
            if (e != entt::null) {
                game.registry.get<Intent>(e) = {.kind = 2, .dir = {0.0f, 0.0f}};
            }
            return 0;
        }
    }
    return -1;
}

void apply_commands(BadlandsGame& game) {
    // FIFO order; the enqueuing (think) pass iterates entities by slot, so the
    // per-tick apply order is deterministic.
    for (const Command& cmd : game.command_queue) {
        apply_command(game, cmd);
    }
    game.command_queue.clear();
}

void enqueue_move_to(BadlandsGame& game, uint32_t slot, glm::vec2 target) {
    entt::entity e = entity_for_slot(game, static_cast<int32_t>(slot));
    if (e == entt::null) {
        return;
    }
    const MoveTarget& mt = game.registry.get<MoveTarget>(e);
    if (mt.kind == MoveTarget::Kind::Point && mt.point == target) {
        return;  // already walking there — not a new decision
    }
    game.command_queue.push_back({CommandKind::MoveTo, slot, UINT32_MAX, target});
}

void enqueue_set_behavior(BadlandsGame& game, uint32_t slot, int32_t behavior) {
    entt::entity e = entity_for_slot(game, static_cast<int32_t>(slot));
    if (e == entt::null) {
        return;
    }
    const auto* sim = game.registry.try_get<HeroSimulationState>(e);
    if (sim == nullptr || sim->behavior == behavior) {
        return;  // unchanged (or nothing to record it on)
    }
    game.command_queue.push_back(
        {CommandKind::SetBehavior, slot, UINT32_MAX, {0.0f, 0.0f}, behavior});
}

void apply_replay_commands(BadlandsGame& game) {
    if (game.replay_log == nullptr) {
        return;
    }
    const std::vector<Command>& log = *game.replay_log;
    // Stamps are non-decreasing (world_millis only advances), so a monotonic
    // cursor is enough -- no search, and every command lands on exactly the tick
    // boundary it was originally applied at.
    while (game.replay_cursor < log.size() &&
           log[game.replay_cursor].at_millis <= game.world_millis) {
        game.command_queue.push_back(log[game.replay_cursor++]);
    }
    apply_commands(game);
}

}  // namespace badlands

extern "C" {

uint32_t game_command_log(const BadlandsGame* game, GameCommandRecord* out, uint32_t cap) {
    const auto& log = game->command_log;
    const auto total = static_cast<uint32_t>(log.size());
    const uint32_t n = std::min(total, cap);
    // Tail-biased: a panel wants the most recent n, still oldest-first.
    for (uint32_t i = 0; i < n; ++i) {
        const badlands::Command& c = log[total - n + i];
        out[i] = GameCommandRecord{
            .kind = static_cast<int32_t>(c.kind),
            .actor = c.actor,
            .target_id = c.target_id,
            .point_x = c.point.x,
            .point_z = c.point.y,
            .param_a = c.param_a,
            .param_b = c.param_b,
        };
    }
    return total;
}

}  // extern "C"
