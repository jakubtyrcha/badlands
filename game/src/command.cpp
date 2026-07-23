#include "command.h"

#include "combat.h"
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
            PlacementDesc desc{cmd.param_a, cmd.param_b, cmd.point.x, cmd.point.y};
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
                    // Starting a deliberation pause sets its deadline;
                    // committing to anything else cancels one in progress. Both
                    // derive from the command, so replay reproduces the pause
                    // without re-drawing its length.
                    sim->think_until_millis =
                        (cmd.param_a == static_cast<int32_t>(ActivityId::Think) &&
                         cmd.param_b > 0)
                            ? game.world_millis + cmd.param_b
                            : 0;
                } else if (auto* cs = game.registry.try_get<CritterState>(e)) {
                    cs->behavior = cmd.param_a;
                } else if (auto* tc = game.registry.try_get<TaxCollectorState>(e)) {
                    tc->behavior = cmd.param_a;
                }
            }
            return 0;
        }
        case CommandKind::Attack: {
            // Resolve one attack against the commanded target (UINT32_MAX => the
            // engine picks the nearest enemy). fire_attack is authoritative: it
            // re-validates range/cooldown/lock and chooses the attack. Melee lands
            // now; a ranged attack spawns a projectile that resolves on arrival.
            fire_attack(game, cmd.actor, cmd.target_id);
            return 0;
        }
        case CommandKind::CollectTax: {
            // Bank a building's owed tax into the collector's carry, once. The
            // brain only emits this on arrival; guard against a stale/dead target.
            entt::entity e = entity_for_slot(game, static_cast<int32_t>(cmd.actor));
            if (e == entt::null) {
                return 0;
            }
            auto* tc = game.registry.try_get<TaxCollectorState>(e);
            const uint32_t bid = cmd.target_id;
            if (tc == nullptr || bid >= game.placement.buildings.size() ||
                !game.placement.buildings[bid].alive) {
                return 0;
            }
            PlacedBuilding& b = game.placement.buildings[bid];
            tc->carried_gold += b.taxable_income;
            b.taxable_income = 0;
            tc->visited.push_back(bid);  // do not collect it again this round
            return 0;
        }
        case CommandKind::Deposit: {
            // Bank the carry into the player's gold and despawn. The brain routes
            // to a Castle/Watchtower and gates this on arrival.
            entt::entity e = entity_for_slot(game, static_cast<int32_t>(cmd.actor));
            if (e == entt::null) {
                return 0;
            }
            if (auto* tc = game.registry.try_get<TaxCollectorState>(e)) {
                game.gold += tc->carried_gold;
            }
            game.registry.destroy(e);  // round complete; slot stays invalid
            return 0;
        }
        case CommandKind::AttackBuilding: {
            // A monster swings at a building with its primary (index-0) attack.
            // Authoritative: re-check the target is alive and that attack is off
            // cooldown (the brain gates too, but the handler is the single
            // mutation point). At 0 hp the building is razed through the cascade.
            entt::entity e = entity_for_slot(game, static_cast<int32_t>(cmd.actor));
            const uint32_t bid = cmd.target_id;
            if (e == entt::null || bid >= game.placement.buildings.size() ||
                !game.placement.buildings[bid].alive) {
                return 0;
            }
            auto* attacks = game.registry.try_get<Attacks>(e);
            if (attacks == nullptr || attacks->count == 0 ||
                attacks->cooldown_remaining[0] > 0.0f) {
                return 0;  // unarmed, or still recovering from the last swing
            }
            PlacedBuilding& b = game.placement.buildings[bid];
            const glm::vec2 bc = b.center;
            const float dmg = attacks->defs[0].base_damage;
            b.hp -= dmg;
            attacks->cooldown_remaining[0] = attacks->defs[0].cooldown;
            emit_building_hit(game, cmd.actor, bid, dmg, b.hp, bc);
            if (b.hp <= 0.0f) {
                raze_building(game, bid);
            }
            return 0;
        }
        case CommandKind::Chat: {
            // Start a conversation. Authoritative: the brain gates on distance,
            // but the handler re-checks everything, because it is the single
            // point where the session comes into existence. The session lands
            // on BOTH heroes from this one command -- the partner's own Chat
            // command (if it emits one) then harmlessly no-ops.
            entt::entity a = entity_for_slot(game, static_cast<int32_t>(cmd.actor));
            entt::entity b = entity_for_slot(game, static_cast<int32_t>(cmd.target_id));
            if (a == entt::null || b == entt::null || a == b) {
                return 0;
            }
            if (!game.registry.all_of<HeroSimulationState>(a) ||
                !game.registry.all_of<HeroSimulationState>(b)) {
                return 0;  // only heroes converse
            }
            if (game.registry.any_of<InsideBuilding, ChattingState>(a) ||
                game.registry.any_of<InsideBuilding, ChattingState>(b)) {
                return 0;  // hidden, or already talking to someone else
            }
            const float d = glm::distance(game.registry.get<Position>(a).pos,
                                          game.registry.get<Position>(b).pos);
            if (d > game.factors.hero.chat_radius) {
                return 0;  // drifted apart before the command applied
            }
            const float duration = game.factors.hero.chat_duration;
            game.registry.emplace<ChattingState>(a, cmd.target_id, duration);
            game.registry.emplace<ChattingState>(b, cmd.actor, duration);
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
    // Re-stating a goal that has not meaningfully moved is not a decision. The
    // epsilon (not exact equality) matters for hold-position activities: a
    // character standing still while Idle/Think/Chat is nudged a hair by unit
    // separation each tick, and exact comparison would put one MoveTo per tick
    // in the trace for as long as it stood there. Well below plan_paths'
    // kGoalMovedThreshold, so it never suppresses a real repath.
    constexpr float kGoalEpsilon = 0.05f;
    const MoveTarget& mt = game.registry.get<MoveTarget>(e);
    if (mt.kind == MoveTarget::Kind::Point &&
        glm::distance(mt.point, target) <= kGoalEpsilon) {
        return;  // already walking there — not a new decision
    }
    game.command_queue.push_back({CommandKind::MoveTo, slot, UINT32_MAX, target});
}

void enqueue_set_behavior(BadlandsGame& game, uint32_t slot, int32_t behavior,
                          int64_t duration_millis) {
    entt::entity e = entity_for_slot(game, static_cast<int32_t>(slot));
    if (e == entt::null) {
        return;
    }
    // Behaviour is recorded on whichever inspection carrier the entity has.
    int32_t current = INT32_MIN;
    if (const auto* sim = game.registry.try_get<HeroSimulationState>(e)) {
        current = sim->behavior;
    } else if (const auto* cs = game.registry.try_get<CritterState>(e)) {
        current = cs->behavior;
    } else if (const auto* tc = game.registry.try_get<TaxCollectorState>(e)) {
        current = tc->behavior;
    } else {
        return;  // nothing to record it on
    }
    if (current == behavior) {
        return;  // unchanged -- not a decision
    }
    game.command_queue.push_back({CommandKind::SetBehavior,
                                  slot,
                                  UINT32_MAX,
                                  {0.0f, 0.0f},
                                  behavior,
                                  static_cast<int32_t>(duration_millis)});
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
