// Hero progression: the leveling curve and the XP accrual entry points.
// SYSTEM RULES, deliberately not Commands: XP is a pure function of what the
// tick already did (kills applied, texels discovered), so a replayed log
// reproduces it exactly; logging it would re-state the tick, not a decision.

#pragma once

#include "badlands_sim.hpp"  // ProgressionFactors

#include <entt/entt.hpp>
#include <glm/glm.hpp>

#include <cstdint>
#include <vector>

struct BadlandsGame;

namespace badlands {

// Recomputes every level-scaled stat for `level`: Health.max_hp, the Combatant
// gates, each attack's base_damage, and the legacy Stats mirror of attack 0.
//
// ALWAYS base + growth * (level - 1) read off the entity's own BaseStats/
// Growth -- never an accumulating +=. That makes it idempotent (calling it
// twice at one level changes nothing) and replay-exact (a recompute lands on
// the identical float rather than drifting by an accumulated epsilon).
//
// Current hp rides max_hp proportionally, so a level-up is not a free heal:
// a hero at half health is at half health one level later, with more of it.
// A level below 1 is clamped to 1 rather than running the growth backwards.
// A no-op on an entity carrying no BaseStats/Growth.
void apply_level_stats(entt::registry& reg, entt::entity e, int32_t level);

// XP needed to advance FROM `level`: floor(base * level^exponent), never
// below 1 (sanitize keeps base >= 1, exponent >= 0). Saturates to INT32_MAX
// if the curve runs past int range (an effective level cap, not an overflow).
int32_t xp_to_next(const ProgressionFactors& p, int32_t level);

// Hard cap on award_xp's level-up loop (Finding 3). xp_to_next saturating
// bounds the COST side, but a legal degenerate config (level_base_xp=1,
// level_exponent=0 -> xp_to_next() == 1 forever) makes the cost side
// saturate at 1 instead of INT32_MAX, so a large-enough single award would
// otherwise loop the level-up while() on the order of 2^31 times. At the
// cap, a hero keeps accruing/saturating xp but gets no further level-ups,
// skill grants, or HeroLeveledUp events.
inline constexpr int32_t kMaxHeroLevel = 100;

// Adds XP to a hero (no-op for invalid slots, non-heroes, amount <= 0),
// looping level-ups: each crossing subtracts the cost, bumps level, grants
// that level's class skills (SkillGrantTable) and emits HeroLeveledUp. amount
// is int64 so a caller (e.g. texels * xp_per_texel) can widen its product
// before it overflows int32; the accumulation into HeroSimulationState::xp
// (an int32) saturates at INT32_MAX rather than wrapping. The loop itself is
// bounded at kMaxHeroLevel, above -- see that constant's own comment for why
// xp_to_next's cost-side saturation is not enough on its own.
void award_xp(BadlandsGame& game, uint32_t slot, int64_t amount);

// One dead entity's XP payout, collected by the death sweep BEFORE the
// destroys (Position and XpReward die with the entity).
struct PendingKillXp {
    glm::vec2 pos;
    int32_t amount;
};

// Splits each payout evenly (round up) over heroes within
// progression.kill_xp_radius of the corpse -- euclidean, obstacles ignored,
// heroes hidden inside buildings excluded. Nobody in range -> the XP
// evaporates. Even split is the v1 rule; per-impact weighting (damage done,
// tanking, support) replaces the `share` computation here later.
void spread_kill_xp(BadlandsGame& game, const std::vector<PendingKillXp>& payouts);

}  // namespace badlands
