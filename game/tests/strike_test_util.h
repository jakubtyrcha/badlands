// Test helper for the attack-commitment mechanic (game/src/strike.h).
//
// fire_attack (and so the Attack command handler) now COMMITS a swing rather
// than resolving it: the blow lands when the attack's wind-up elapses, in
// advance_strikes. Inside step_world that happens automatically, right after
// apply_commands -- so an attack authored with no wind-up and no recovery
// still resolves within its own tick, exactly as it did before commitment
// existed.
//
// A test that drives apply_commands / fire_attack DIRECTLY, without ticking
// the world, skips that sweep. This runs it for them.

#pragma once

#include "components.h"  // StrikeInProgress, kTicksPerStep
#include "game_state.h"
#include "strike.h"

namespace testfix {

// Runs advance_strikes until no strike is left in progress, advancing the
// world clock by one tick per pass so wind-ups and recoveries actually elapse.
// For a 0/0 attack this is a single pass and the clock does move one tick --
// which is why it is a helper rather than a bare advance_strikes call: the
// deadlines are absolute times, so a caller that forgets the clock would spin.
// Bounded so a never-resolving strike fails the test instead of hanging it.
inline void land_strikes(BadlandsGame& g, int max_ticks = 400) {
    for (int i = 0; i < max_ticks && !g.registry.view<badlands::StrikeInProgress>().empty();
         ++i) {
        g.world_ticks += badlands::kTicksPerStep;
        badlands::advance_strikes(g);
    }
}

}  // namespace testfix
