#pragma once

// The puppet master's contract.
//
// A MODE says what world to build and who is in it. The game then runs, and
// cannot tell it is being watched: nothing in game/ or badlands_game_lib knows
// this type exists, no system branches on "am I in a mode", and every world a
// mode produces is an ordinary WorldConfig full of ordinary buildings and
// creatures. That is the whole point -- what the sandbox observes has to be the
// game's real behaviour, not the behaviour of a game that knows it is a test.
//
// VIEW-FREE BY DESIGN. No SceneGraph, Camera, ImGui, SDL or Dawn appears here
// or in any implementation, so a mode is testable without a window and the host
// remains the only thing that draws. badlands_ai_sandbox_tests links the modes
// against badlands_game_lib alone, which makes that a compile-time guarantee
// rather than a convention.

#include <string>
#include <vector>

#include "badlands_sim.hpp"  // WorldConfig, Sim, CharacterState

namespace badlands {

class SandboxMode {
   public:
    virtual ~SandboxMode() = default;

    virtual const char* name() const = 0;

    // Initial config for the next world: which map, what is already built.
    // Called once per staging, so a mode that varies its world between rounds
    // simply returns something different.
    virtual WorldConfig Configure() = 0;

    // Populate the freshly-built world, through the ordinary Sim API.
    virtual void Stage(Sim& sim) = 0;

    // One tick's snapshot, after the tick, plus the events that tick produced.
    // Returns true to ask the host for a FRESH world -- Configure() and Stage()
    // again. Whatever the mode wants to report about the round it just
    // finished, it logs here.
    //
    // The EVENTS are here because the rows cannot answer "did it happen": a
    // CharacterState carries no statuses, and a status that comes and goes
    // between two frames leaves no trace in them at all. The host already
    // drains this stream every tick and discards it, so a mode watching for one
    // particular decision is reading something that already exists rather than
    // asking the sim a new question.
    virtual bool Observe(const std::vector<CharacterState>& rows,
                         const std::vector<GameEvent>& events, int64_t world_ticks) = 0;

    // One line of mode-specific text for the debug panel.
    virtual std::string Status() const = 0;
};

}  // namespace badlands
