#pragma once

// Selectors -- the arbitration half of a brain. Same blocks, different
// arbitration per archetype: heroes/monsters weigh trade-offs (argmax), while
// critters/townfolk take the first applicable behaviour in a fixed priority
// order (cheap, predictable "minimal logic").
//
// Both are deterministic: candidates are scored in list order and ties resolve
// to the earliest candidate, so the same view + factors always yield the same
// choice.

#include <span>

#include "badlands_sim.hpp"
#include "behaviours/blocks.h"
#include "behaviours/world_view.h"

namespace badlands {

// Highest score wins; ties go to the earliest candidate. For the hero list,
// whose scores are strictly-ordered tiers, this equals select_priority -- so
// the town brain's decision is independent of which selector drives it, and
// porting from the old if-chain changes no behaviour.
BehaviourResult select_argmax(std::span<const Candidate> candidates, const WorldView& view,
                              const SimFactors& factors);

// First candidate with a positive score, in list order. The natural fit for a
// short, ordered reaction list (a deer: Flee before Graze before Roam).
BehaviourResult select_priority(std::span<const Candidate> candidates, const WorldView& view,
                                const SimFactors& factors);

// The banded selector: the shared decision core every archetype runs.
//
//   for band in Danger, Productive, Filler, Fallback:
//       best = argmax over that band's activities of weight(id) * score(view)
//       if best exists (utility > 0): return best.act(view)
//   -> Idle in place
//
// Two structural guarantees fall out, and they are the contract worth testing:
//
//   * BAND DOMINANCE -- any applicable Danger activity beats every Productive
//     and Filler one, for ANY weights. Safety is not a tuning problem.
//   * VETO -- an activity scoring 0 is never selected, for any weight. Hard
//     preconditions ("is there a tavern") are expressed as a 0 factor rather
//     than as a threshold to tune.
//
// A band is claimed by any positive utility (the threshold is 0); an activity
// yields to a lower band by disqualifying ITSELF -- e.g. an errand whose
// "am I rested enough" consideration hits 0 lets the loop fall through to rest.
//
// `weights` selects the actor's personality (per class / per archetype); a
// weight of 0 removes that activity entirely. Deterministic: activities are
// scored in list order and ties keep the earliest, so identical inputs always
// yield an identical decision.
BehaviourResult select_banded(std::span<const ActivityDef> activities,
                              const ActivityWeights& weights, const WorldView& view,
                              const SimFactors& factors);

}  // namespace badlands
