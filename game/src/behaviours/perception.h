#pragma once

// Shared perception: the registry-reading helpers that FILL a WorldView.
//
// This is the other side of the boundary blocks.h describes. A block may only
// read a WorldView; an observe_*() function may read the world, and it does so
// through helpers collected here so that two archetypes asking the same
// question ("what is near me that I should worry about?") get the same answer
// by the same code rather than by two similar loops.
//
// Fog of war will filter here, at this one seam, when perception becomes
// knowledge-limited.

#include <cstdint>

#include <entt/entt.hpp>
#include <glm/glm.hpp>

#include "behaviours/world_view.h"

struct BadlandsGame;

namespace badlands {

// What the observer treats as a threat. Perception is RELATIONAL, never
// taxonomic -- an entity asks "is that one of mine?", not "is that a Goblin?".
// Adding a species must never require touching this enum.
enum class ThreatPolicy : int32_t {
    // Anything on another team. What a hero or a monster watches for; neutral
    // wildlife is not a threat to either.
    HostileTeam,
    // Anything that is not another critter. What a deer watches for -- it bolts
    // from heroes, tax collectors and rats alike, and ignores the herd.
    NotMyKind,
};

// Fills `out.threats` with everything within `radius` of `pos` that `policy`
// counts as a threat to `self`, nearest first, capped at WorldView::kMaxThreats.
// Returns the number recorded (also written to out.threat_count).
//
// `self` is excluded. Hidden (inside-building) and dead entities are skipped:
// something you cannot see and something that cannot act are not threats.
int32_t collect_threats(const BadlandsGame& game, entt::entity self, glm::vec2 pos, float radius,
                        ThreatPolicy policy, WorldView& out);

}  // namespace badlands
