// The town's periodic world systems: tax accrual and population spawning. Both
// are deterministic functions of the sim clock (world_millis), like advance_needs
// -- they run every tick in both live and replay, so they need not be logged as
// commands. tick_world calls them before the brain-think pass.

#pragma once

struct BadlandsGame;

namespace badlands {

// At each midnight crossing (day_count change), add TownfolkFactors::
// house_income_per_day to every alive House's taxable_income -- the tax a
// collector later banks.
void advance_economy(BadlandsGame& game);

// Periodic entity spawning from buildings. On each TownfolkFactors::
// spawn_interval_millis boundary, spawn a tax collector at each alive Castle,
// capped at TownfolkFactors::max_alive live collectors. (Phase 5 adds the rat at
// the Sewer here too.)
void run_spawners(BadlandsGame& game);

}  // namespace badlands
