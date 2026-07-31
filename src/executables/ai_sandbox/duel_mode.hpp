#pragma once

// Duel mode: sample two creatures and an arena, stage them as enemies, give
// them a minute, log who won, restage.
//
// Everything here is puppet master. The two fighters are spawned through the
// ordinary Sim API onto opposing teams and are then left entirely alone -- they
// find each other, decide, close, and fight through the same brains, intentions
// and combat the real game runs. Nothing tells them this is a duel.

#include <cstdint>
#include <string>
#include <vector>

#include "executables/ai_sandbox/arena.hpp"
#include "executables/ai_sandbox/sandbox_mode.hpp"

namespace badlands {

struct DuelConfig {
    uint64_t seed = 1;
    // Sim-time budget for a round. A pairing that cannot resolve inside it is a
    // draw and says so -- a stalemate is a result, not a hang.
    int64_t max_millis = 60000;
    // How long to keep watching after one side falls, so the end of a fight is
    // visible rather than cutting to the next stage the instant it lands.
    int64_t linger_millis = 5000;
    // Hero level range, inclusive. The design doc's early game is levels 1-8,
    // and 8 is where the last skill unlocks -- so this range is what makes
    // every skill a class has reachable in a duel at all. Ignored for monsters:
    // they do not level.
    int32_t min_level = 1;
    int32_t max_level = 8;
};

// Every creature that can actually fight: not a Critter, and declaring at least
// one attack (either an authored loadout or the legacy single-attack fields the
// spawn path derives one from).
//
// Read from the live catalog rather than a hardcoded list, so a creature added
// to the roster joins the pool the day it is added. Deer is excluded BY THAT
// RULE rather than by name -- and it has to be, since something that never
// fights would never end a round.
std::vector<CreatureId> duel_pool(const CreatureCatalog& catalog);

struct DuelSetup {
    ArenaShape shape = ArenaShape::Tube;
    CreatureId left = CreatureId::Count;
    CreatureId right = CreatureId::Count;
    // Drawn for BOTH sides regardless of what was sampled, and simply ignored
    // by a monster. Drawing unconditionally keeps the sampler a pure function
    // of (seed, round) -- branching the draw on the creature would make the
    // stream depend on what came out of an earlier draw.
    int32_t left_level = 1;
    int32_t right_level = 1;
};

// Pure: (seed, round) -> setup. The same pair always gives the same duel, so a
// session is reproducible without anything being stored.
DuelSetup sample_duel(const DuelConfig& cfg, const std::vector<CreatureId>& pool,
                      uint32_t round);

// Who is still standing. A team is alive while it has a living non-Critter row.
struct DuelTally {
    bool left_alive = false;
    bool right_alive = false;
};
DuelTally tally_duel(const std::vector<CharacterState>& rows);

class DuelMode : public SandboxMode {
   public:
    explicit DuelMode(DuelConfig cfg) : cfg_(cfg) {}

    const char* name() const override { return "duel"; }
    WorldConfig Configure() override;
    void Stage(Sim& sim) override;
    bool Observe(const std::vector<CharacterState>& rows, int64_t world_millis) override;
    std::string Status() const override;

   private:
    DuelConfig cfg_;
    std::vector<CreatureId> pool_;  // filled on the first Stage, from the live catalog
    uint32_t round_ = 0;
    DuelSetup setup_{};
    ArenaLayout layout_{};
    // Sim time this round began. Every deadline is measured against the world
    // clock, not real seconds, so the speed control changes how long you WAIT
    // and never who wins.
    int64_t started_millis_ = 0;
    // Sim time the round is to be reported at: set when one side falls, so the
    // linger is a deadline rather than a countdown to decrement.
    int64_t report_at_millis_ = 0;
    bool decided_ = false;
    std::string last_result_ = "no duel yet";
};

}  // namespace badlands
