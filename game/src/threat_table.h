// Threat: a creature's combat potential -- what it is worth in a fight.
//
// ONE NUMBER, TWO ROLES, deliberately (docs/superpowers/specs/
// 2026-07-31-core-classes-combat-design.md §7):
//
//   1. CALIBRATION. These anchors are fixed TARGETS. The duel matrix
//      (src/executables/duelsim) measures how far the stats land from them;
//      balancing moves the STATS toward the anchors, never the anchors toward
//      the stats. Deriving threat from stats would make the whole report
//      circular -- it would validate the numbers against a restatement of
//      themselves.
//
//   2. BRAIN DECISIONS. Because role 1 keeps it a fair approximation, a brain
//      can compare its own threat against a hostile's and be smart about
//      whether a fight is worth taking. Both sides ride the brain wire
//      (BlViewSelf::threat / BlThreat::threat, game/src/brain_abi.h) for
//      exactly this.
//
// The dependency runs one way: role 2 is only as trustworthy as role 1 has
// been done. A brain deciding on a badly-calibrated number makes confident,
// wrong choices.
//
// Threat NEVER feeds combat resolution. It approximates the outcome, so it
// must not become an input to it.
//
// COMPILED, not JSON, and deliberately so: every other number in this sim is a
// tunable, and this one is the fixed post the tunables are measured against.
// The values will change as the design settles -- but by editing this table on
// purpose, never as a side effect of tuning a stat.

#pragma once

#include "badlands_sim.hpp"  // CreatureId

#include <entt/entt.hpp>

#include <cstdint>

namespace badlands {

// One calibration point: "at this level, this creature is worth this much".
struct ThreatAnchor {
    int32_t level;
    float threat;
};

// Linear between anchors, FLAT outside the authored range (never extrapolated
// -- a curve sketched at levels 1..20 says nothing about level 40, and
// guessing would be worse than repeating the last known point). `anchors` must
// be ascending by level; a count of 0 gives 0.
float interpolate_anchors(const ThreatAnchor* anchors, int32_t count, int32_t level);

// The intended threat of `creature` at `level`. 0 for an out-of-range id.
float threat_target(CreatureId creature, int32_t level);

// The threat of a LIVE entity: its creature's anchor at its current level
// (heroes carry one in HeroSimulationState; anything else is level 1). The one
// call the wire packer and every brain-facing consumer uses, so nobody
// re-derives "which creature is this" independently. 0 for an entity with no
// CreatureKind.
//
// Deliberately NOT scaled by current health or statuses: what a wounded enemy
// is worth RIGHT NOW is a judgement, and judgements belong to the brain, which
// already sees health on the wire.
float threat_of(const entt::registry& reg, entt::entity e);

}  // namespace badlands
