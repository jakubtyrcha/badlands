#pragma once

// Teleport mode: a level-8 apprentice standing next to something worth threat
// 20. It should blink away.
//
// The opponent is a TrainingDummy, which has no attacks at all. That is the
// scenario working rather than a compromise: threat approximates what a
// creature is worth in a fight, so a brain deciding whether to run has only
// that number to go on -- and an opponent that never lands a blow makes the
// measurement clean. What is under test is the DECISION, not a race between
// the blink and the apprentice's hit points.
//
// Which is also why the brain's teleport gate keys on threat rather than on
// health: against this dummy, health never moves.

#include <cstdint>
#include <string>
#include <vector>

#include "executables/ai_sandbox/arena.hpp"
#include "executables/ai_sandbox/sandbox_mode.hpp"

namespace badlands {

struct TeleportConfig {
    // Teleport unlocks at 8, so this is the lowest level that can show anything
    // -- and the top of the design doc's early game.
    int32_t hero_level = 8;
    int64_t max_millis = 30000;
    // How close the dummy starts. Inside the apprentice's own bolt range, so it
    // has every reason to be somewhere else.
    float start_gap_m = 4.0f;
};

// One round's observation, folded from the event stream. Pure over what it is
// handed, so the verdict is testable without a Sim.
struct TeleportProgress {
    bool blinked = false;
    int64_t blinked_at_millis = 0;
};

// Folds one tick's events into `p`. Only a SkillUsed naming Teleport by the
// hero counts -- the apprentice also curses and calcifies, and either would
// otherwise look like success.
void observe_teleport(const std::vector<GameEvent>& events, uint32_t hero_slot,
                      TeleportProgress& p);

class TeleportMode : public SandboxMode {
   public:
    explicit TeleportMode(TeleportConfig cfg) : cfg_(cfg) {}

    const char* name() const override { return "teleport"; }
    WorldConfig Configure() override;
    void Stage(Sim& sim) override;
    bool Observe(const std::vector<CharacterState>& rows, const std::vector<GameEvent>& events,
                 int64_t world_millis) override;
    std::string Status() const override;

   private:
    TeleportConfig cfg_;
    ArenaLayout layout_{};
    uint32_t round_ = 0;
    uint32_t hero_slot_ = UINT32_MAX;
    int64_t started_millis_ = 0;
    // Where the apprentice was standing when the round began, so the log can
    // report how far it actually got rather than only that it cast something.
    glm::vec2 start_pos_{};
    TeleportProgress progress_{};
    std::string last_result_ = "no round yet";
};

}  // namespace badlands
