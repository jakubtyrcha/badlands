// Hero progression: the leveling curve and the XP accrual entry points.
// SYSTEM RULES, deliberately not Commands: XP is a pure function of what the
// tick already did (kills applied, texels discovered), so a replayed log
// reproduces it exactly; logging it would re-state the tick, not a decision.

#pragma once

#include "badlands_sim.hpp"  // ProgressionFactors

#include <glm/glm.hpp>

#include <cstdint>
#include <vector>

struct BadlandsGame;

namespace badlands {

// XP needed to advance FROM `level`: floor(base * level^exponent), never
// below 1 (sanitize keeps base >= 1, exponent >= 0). Saturates to INT32_MAX
// if the curve runs past int range (an effective level cap, not an overflow).
int32_t xp_to_next(const ProgressionFactors& p, int32_t level);

// Adds XP to a hero (no-op for invalid slots, non-heroes, amount <= 0),
// looping level-ups: each crossing subtracts the cost, bumps level, grants
// that level's class skills (SkillGrantTable) and emits HeroLeveledUp.
void award_xp(BadlandsGame& game, uint32_t slot, int32_t amount);

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
