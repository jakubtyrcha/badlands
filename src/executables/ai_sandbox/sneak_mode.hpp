#pragma once

// Sneak mode: one scenario, watched until it either happens or does not.
//
// A grave robber and a bandit at opposite ends of the longest arena. The robber
// should go unseen, cross the gap, and open with the blow that gives it away.
// The mode reports which of those happened and restages.
//
// This exists because a random duel is a bad way to check one behaviour: the
// right pairing may not come up, and when it does the interesting moment is two
// seconds long. Naming a scenario and reporting a verdict turns "watch and hope"
// into a line of text -- which is also the only thing it produces. No files, no
// scores, no accumulator.
//
// Puppet master, like every mode: the robber is spawned through the ordinary
// Sim API and then left completely alone. Nothing tells it that anyone is
// waiting to see it sneak.

#include <cstdint>
#include <string>
#include <vector>

#include "executables/ai_sandbox/arena.hpp"
#include "executables/ai_sandbox/sandbox_mode.hpp"

namespace badlands {

struct SneakConfig {
    // Level 8: Sneak unlocks at 3, so anything past it works -- 8 is the top of
    // the design doc's early game and what the duel mode already samples up to.
    int32_t hero_level = 8;
    // How long the robber gets. Generous: crossing the arena unseen is most of
    // the scenario, and a timeout is a real finding rather than an impatience.
    int64_t max_ticks = 30 * 120;  // 30 s
    // How far the starting separation shifts between rounds, cycling over a few
    // values. Without it every round is byte-identical -- combat rolls are
    // seeded off (attacker, target, world_ticks, attack_index), so an
    // identical scenario replays exactly and running the mode twelve times
    // tells you precisely what running it once did. A different closing time is
    // a genuinely different roll stream.
    float separation_step_m = 3.0f;
    int32_t separation_steps = 5;
};

// What a round is waiting to see, in order. `Struck` is terminal.
enum class SneakStage : int32_t { Waiting = 0, Sneaked, Struck };

// One round's observations, folded from the event stream. Pure over what it is
// handed, so the whole verdict is testable without a Sim.
struct SneakProgress {
    SneakStage stage = SneakStage::Waiting;
    int64_t sneaked_at_ticks = 0;
    int64_t struck_at_ticks = 0;
    float strike_damage = 0.0f;
    // Did the opening blow come out of Backstab, or was it an ordinary swing?
    // Reported rather than required: the pair is what the class is FOR, and a
    // round where the robber arrives unseen and then just swings is a finding
    // about the brain's priorities, not a broken mode.
    bool backstabbed = false;
};

// Folds one tick's events into `p`. `hero_slot` is the robber; everything else
// in the stream is ignored, so a monster's own swing can never be mistaken for
// the payoff. Idempotent per event -- StatusApplied refreshes are not a second
// sneak, and only the FIRST damaging blow after the sneak counts.
void observe_sneak(const std::vector<GameEvent>& events, uint32_t hero_slot,
                   SneakProgress& p);

class SneakMode : public SandboxMode {
   public:
    explicit SneakMode(SneakConfig cfg) : cfg_(cfg) {}

    const char* name() const override { return "sneak"; }
    WorldConfig Configure() override;
    void Stage(Sim& sim) override;
    bool Observe(const std::vector<CharacterState>& rows, const std::vector<GameEvent>& events,
                 int64_t world_ticks) override;
    std::string Status() const override;

   private:
    SneakConfig cfg_;
    ArenaLayout layout_{};
    uint32_t round_ = 0;
    uint32_t hero_slot_ = UINT32_MAX;
    int64_t started_ticks_ = 0;
    SneakProgress progress_{};
    std::string last_result_ = "no round yet";
};

}  // namespace badlands
