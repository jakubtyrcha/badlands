#pragma once

// Behaviour blocks -- the reusable unit of a brain. Each block is a (score, act)
// pair over a WorldView + SimFactors, mirroring what a noiser module would
// export (scripts/brains/hero.noiser's score/act), so re-adopting noiser later
// is a port rather than a rewrite.
//
//   score(view, factors) -> f32   how much this entity wants to do this now.
//                                  0 means "not applicable"; a selector
//                                  (selectors.h) picks among the scores.
//   act(view, factors)   -> BehaviourResult   the goal + optional follow-up.
//
// Blocks NEVER read the registry -- only the WorldView. A block shared by more
// than one archetype (Roam, and Flee in a later phase) is written once here.

#include "badlands_sim.hpp"  // badlands::SimFactors
#include "behaviours/world_view.h"

namespace badlands {

// A block is a plain pair of free functions, referenced by a Candidate so a
// selector can score a list without committing to any action.
using ScoreFn = float (*)(const WorldView&, const SimFactors&);
using ActFn = BehaviourResult (*)(const WorldView&, const SimFactors&);

struct Candidate {
    ScoreFn score;
    ActFn act;
};

// --- hero blocks ------------------------------------------------------------
// Scores are TIERS, not soft weights: GoHome > Buy > VisitTavern > Roam > Idle
// when each is applicable. That makes argmax reproduce the old town_brain
// priority chain exactly (and equals select_priority for this list), so porting
// changes no behaviour. Soft, class-weighted scoring is a later refinement.
float score_go_home(const WorldView&, const SimFactors&);
BehaviourResult act_go_home(const WorldView&, const SimFactors&);

float score_buy(const WorldView&, const SimFactors&);
BehaviourResult act_buy(const WorldView&, const SimFactors&);

float score_visit_tavern(const WorldView&, const SimFactors&);
BehaviourResult act_visit_tavern(const WorldView&, const SimFactors&);

// --- shared blocks ----------------------------------------------------------
// Roam walks to view.roam_goal (chosen in perception: hero rng ring, or deer
// biome-filtered). Shared verbatim by the hero and critter brains.
float score_roam(const WorldView&, const SimFactors&);
BehaviourResult act_roam(const WorldView&, const SimFactors&);

// Flee runs directly away from a perceived threat. Reads only the threat fields,
// so it is archetype-agnostic -- the deer uses it now, a hero could later. The
// flee radius/distance come from CritterFactors for now (the only current user).
float score_flee(const WorldView&, const SimFactors&);
BehaviourResult act_flee(const WorldView&, const SimFactors&);

// Idle: the last resort. Always applicable at the lowest tier; holds position.
float score_idle(const WorldView&, const SimFactors&);
BehaviourResult act_idle(const WorldView&, const SimFactors&);

// --- critter blocks ---------------------------------------------------------
// Graze holds position during the graze half of the walk->graze cycle.
float score_graze(const WorldView&, const SimFactors&);
BehaviourResult act_graze(const WorldView&, const SimFactors&);

// --- townfolk (tax collector) blocks ----------------------------------------
// VisitNextTaxable walks to the next unvisited building owing tax and banks it
// on arrival (CollectTax). Deposit walks to a Castle/Watchtower and banks the
// carry into player gold, then despawns (Deposit) -- the round's end. Deposit
// scores below VisitNextTaxable, so the collector finishes its rounds first.
float score_visit_taxable(const WorldView&, const SimFactors&);
BehaviourResult act_visit_taxable(const WorldView&, const SimFactors&);

float score_deposit(const WorldView&, const SimFactors&);
BehaviourResult act_deposit(const WorldView&, const SimFactors&);

// --- shared perception helper -----------------------------------------------
// Deterministic wander point: a per-(slot,epoch) offset within `radius` of
// `anchor`. Used by observe_* to fill WorldView::roam_goal; kept here so the
// hero and deer draw wander goals with identical (bit-exact) math.
glm::vec2 roam_point(uint32_t slot, int64_t epoch, glm::vec2 anchor, float radius);

}  // namespace badlands
