#pragma once

// Choosing somewhere unexplored to go.
//
// The discovered region of the fog-of-war field has an OUTLINE: texels that are
// known but border something unknown. That frontier is the coherent shape this
// reasons about -- it is the edge of what the colony has seen, and it closes in
// on itself as the map fills. An exploration target is a point on that outline
// pushed OUTWARD, into the dark, so walking to it necessarily reveals ground
// nobody has looked at.
//
// Pure query over the vision grid: no registry, no brains. That is what makes
// it unit-testable against a hand-drawn grid, and what lets the algorithm be
// replaced (contour tracing, region scoring, biome preference) behind an
// unchanged signature.

#include <cstdint>
#include <optional>

#include <glm/glm.hpp>

#include "badlands_sim.hpp"  // HeroFactors

namespace badlands {

struct VisionGrid;

// Picks a point in unexplored territory to walk toward, or nullopt when there
// is nothing to explore -- no vision grid configured, or no frontier within
// explore_search_radius of `origin` (everything nearby is already known).
//
// `seed` makes the choice deterministic: the same seed over the same grid picks
// the same point, which is what keeps a run reproducible and its command log
// replayable. Callers seed from (slot, lease window) so a hero holds one target
// for a while and different heroes strike out in different directions.
std::optional<glm::vec2> pick_exploration_target(const VisionGrid& grid, glm::vec2 origin,
                                                 uint64_t seed, const HeroFactors& factors);

}  // namespace badlands
