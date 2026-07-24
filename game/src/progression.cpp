#include "progression.h"

#include "components.h"
#include "game_state.h"
#include "skills.h"

#include <algorithm>
#include <cmath>

namespace badlands {

int32_t xp_to_next(const ProgressionFactors& p, int32_t level) {
    const double cost = std::floor(static_cast<double>(p.level_base_xp) *
                                   std::pow(static_cast<double>(level),
                                            static_cast<double>(p.level_exponent)));
    if (cost >= 2.0e9) {
        return INT32_MAX;
    }
    return std::max(1, static_cast<int32_t>(cost));
}

void award_xp(BadlandsGame& game, uint32_t slot, int64_t amount) {
    if (amount <= 0) {
        return;
    }
    entt::entity e = entity_for_slot(game, static_cast<int32_t>(slot));
    if (e == entt::null || !game.registry.all_of<HeroSimulationState>(e)) {
        return;
    }
    auto& sim = game.registry.get<HeroSimulationState>(e);
    // Saturate, never wrap: xp_to_next already saturates the cost side, so
    // the level-up loop below stays finite for any non-negative config.
    sim.xp = static_cast<int32_t>(
        std::min<int64_t>(static_cast<int64_t>(sim.xp) + amount, INT32_MAX));
    while (sim.xp >= xp_to_next(game.factors.progression, sim.level)) {
        sim.xp -= xp_to_next(game.factors.progression, sim.level);
        ++sim.level;
        if (const auto* hero = game.registry.try_get<HeroCharacter>(e);
            hero != nullptr && game.registry.all_of<Skills>(e)) {
            grant_skills_for_level(game.registry.get<Skills>(e), hero->hero_class,
                                   sim.level);
        }
        const glm::vec2 pos = game.registry.get<Position>(e).pos;
        emit_event(game, GameEvent{.kind = GameEventKind::HeroLeveledUp,
                                   .actor_id = slot,
                                   .target_id = UINT32_MAX,
                                   .target_kind = kEventTargetCharacter,
                                   .amount = static_cast<float>(sim.level),
                                   .x = pos.x,
                                   .z = pos.y,
                                   .at_millis = game.world_millis});
    }
}

void spread_kill_xp(BadlandsGame& game, const std::vector<PendingKillXp>& payouts) {
    const float radius = game.factors.progression.kill_xp_radius;
    const float r2 = radius * radius;

    // Eligible heroes (outside, not hidden inside a building), collected once:
    // the pool is stable across payouts -- award_xp only touches XP/level/
    // Skills, never Position/InsideBuilding.
    struct Eligible {
        uint32_t slot;
        glm::vec2 pos;
    };
    std::vector<Eligible> eligible;
    for (auto [e, hs, pos] : game.registry
                                 .view<const HeroSimulationState, const Position>(
                                     entt::exclude<InsideBuilding>)
                                 .each()) {
        (void)hs;  // membership only: the view's job is filtering, not reading
        eligible.push_back({slot_for_entity(game, e), pos.pos});
    }

    std::vector<uint32_t> near;
    for (const PendingKillXp& p : payouts) {
        if (p.amount <= 0) {
            continue;
        }
        near.clear();
        for (const Eligible& c : eligible) {
            const glm::vec2 d = c.pos - p.pos;
            if (glm::dot(d, d) <= r2) {
                near.push_back(c.slot);
            }
        }
        if (near.empty()) {
            continue;  // nobody close enough: the XP evaporates
        }
        // Slot order, not view order: award (and event) order is stable.
        std::sort(near.begin(), near.end());
        const int32_t n = static_cast<int32_t>(near.size());
        const int32_t share = (p.amount + n - 1) / n;  // even split, round UP
        for (uint32_t slot : near) {
            award_xp(game, slot, share);
        }
    }
}

}  // namespace badlands
