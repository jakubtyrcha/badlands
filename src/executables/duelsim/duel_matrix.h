// Stage a 1v1, run it to resolution, tally it. The simulation half of the
// balance report.
//
// This REPORTS, it does not gate. Balance is an approximation and the matrix
// is the instrument for seeing where it sits -- nothing here asserts that a
// pairing ought to come out any particular way. What IS pinned lives in
// game/tests (mechanism) and in the threat table (calibration targets).

#pragma once

#include "badlands_sim.hpp"  // CreatureId

#include <cstdint>
#include <vector>

namespace duelsim {

struct DuelOutcome {
    int32_t winner_index = -1;  // 0 = left, 1 = right, -1 = timeout or mutual
    int32_t ticks = 0;
};

// Stage `left` vs `right` in a flat walled arena `separation` units apart, both
// at `level`, and tick to resolution or `max_ticks`.
//
// Combat rolls are seeded off (attacker, target, world_millis, attack_index),
// so a different separation is a genuinely different roll stream -- that is how
// one pairing yields a distribution rather than the same answer repeated. Fully
// deterministic for a given (left, right, level, separation).
DuelOutcome run_duel(badlands::CreatureId left, badlands::CreatureId right, int32_t level,
                     float separation, int32_t max_ticks = 9000);

struct MatrixCell {
    int32_t wins = 0;    // for the ROW creature
    int32_t losses = 0;
    int32_t draws = 0;   // neither side dead inside max_ticks
    int32_t median_ticks = 0;

    int32_t decided() const { return wins + losses; }
    // Win rate over DECIDED duels; -1 when every sample drew (a pairing that
    // cannot resolve says nothing about who is stronger, and reporting 0%
    // would be a lie about it).
    float win_rate() const {
        return decided() > 0 ? static_cast<float>(wins) / static_cast<float>(decided()) : -1.0f;
    }
};

// Every ordered pairing over `roster`, `samples` separations each. Diagonal
// cells (a creature against itself) are included -- they are the calibration
// sanity check, since identical fighters should sit near 50%.
std::vector<std::vector<MatrixCell>> run_matrix(const std::vector<badlands::CreatureId>& roster,
                                                int32_t level, int32_t samples);

// The stat a creature actually has at `level`, straight through the spawn +
// growth path rather than recomputed here -- so the charts plot what the sim
// would really use, not a second implementation of the same arithmetic.
struct LevelStats {
    float max_hp = 0.0f;
    float damage = 0.0f;  // primary attack's base damage
    float armour = 0.0f;
};
LevelStats stats_at_level(badlands::CreatureId creature, int32_t level);

}  // namespace duelsim
