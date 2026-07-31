#include "progression.h"

#include "components.h"
#include "game_state.h"
#include "skills.h"

#include <algorithm>
#include <cmath>

namespace badlands {

void apply_level_stats(entt::registry& reg, entt::entity e, int32_t level) {
    if (e == entt::null || !reg.valid(e) || !reg.all_of<BaseStats, Growth>(e)) {
        return;
    }
    const BaseStats& base = reg.get<BaseStats>(e);
    const StatGrowth& g = reg.get<Growth>(e).rows;
    // Clamp rather than trust: a level below 1 would run the growth backwards
    // and hand out negative hp.
    const float steps = static_cast<float>(std::max(1, level) - 1);

    if (auto* health = reg.try_get<Health>(e); health != nullptr) {
        const float new_max = std::max(1.0f, base.hp + g.hp * steps);
        // Keep the CURRENT fraction, so growth adds capacity without healing.
        // Guard the divide: a max of 0 would have made every hp a NaN.
        const float frac = health->max_hp > 0.0f
                               ? std::clamp(health->hp / health->max_hp, 0.0f, 1.0f)
                               : 1.0f;
        health->max_hp = new_max;
        health->hp = frac * new_max;
    }
    if (auto* c = reg.try_get<Combatant>(e); c != nullptr) {
        // Probabilities stay in [0,1]; armour cannot go negative. Growth is
        // authored non-negative, but a JSON override is not trusted to be.
        c->accuracy = std::clamp(base.accuracy + g.accuracy * steps, 0.0f, 1.0f);
        c->evasion = std::clamp(base.evasion + g.evasion * steps, 0.0f, 1.0f);
        c->defense = std::clamp(base.defense + g.defense * steps, 0.0f, 1.0f);
        c->armour = std::max(0.0f, base.armour + g.armour * steps);
    }
    if (auto* atk = reg.try_get<Attacks>(e); atk != nullptr) {
        const float scale = std::max(0.0f, 1.0f + g.damage_frac * steps);
        for (int i = 0; i < atk->count && i < kMaxAttacks && i < base.attack_count; ++i) {
            atk->defs[i].base_damage = base.attack_damage[i] * scale;
        }
        // Stats is a legacy VIEW of the primary attack (heroes.cpp's spawn
        // comment) -- keep the mirror honest rather than letting perception
        // read a level-1 damage off a level-15 hero.
        if (auto* stats = reg.try_get<Stats>(e); stats != nullptr && atk->count > 0) {
            stats->attack_damage = atk->defs[0].base_damage;
        }
    }
}

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
    // Saturate, never wrap: the accumulation itself stays finite for any
    // amount int64 can hold. The level-up loop below is separately bounded
    // by kMaxHeroLevel (progression.h) -- xp_to_next's cost-side saturation
    // alone is not enough: a degenerate config (level_base_xp=1,
    // level_exponent=0) makes every cost 1, so the loop would otherwise run
    // once per XP point, up to ~2^31 times for a large saturated award.
    sim.xp = static_cast<int32_t>(
        std::min<int64_t>(static_cast<int64_t>(sim.xp) + amount, INT32_MAX));
    while (sim.level < kMaxHeroLevel &&
           sim.xp >= xp_to_next(game.factors.progression, sim.level)) {
        sim.xp -= xp_to_next(game.factors.progression, sim.level);
        ++sim.level;
        // What this level teaches is the entity's OWN grant list (copied off
        // its spawn desc, heroes.cpp) -- there is no class -> skill table to
        // consult, which is what makes acquisition data rather than code.
        if (const auto* grants = game.registry.try_get<SkillGrants>(e);
            grants != nullptr && game.registry.all_of<Skills>(e)) {
            grant_skills_for_level(game.registry.get<Skills>(e), *grants, sim.level);
        }
        // What this level is WORTH is the entity's own growth row, the same
        // way what it TEACHES is its own grant list: both ride the desc, so
        // no consumer re-derives a hero's class to know either one.
        apply_level_stats(game.registry, e, sim.level);
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
