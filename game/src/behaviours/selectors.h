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

}  // namespace badlands
