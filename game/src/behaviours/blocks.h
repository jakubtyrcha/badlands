#pragma once

// Behaviour blocks -- the reusable unit of a brain. Each block is a (score, act)
// pair over a WorldView + SimFactors.
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

// An ACTIVITY is a block plus its identity in the shared goal vocabulary: which
// ActivityId it reports (so it is inspectable and countable) and which band it
// competes in. An archetype's brain is then just a table of these plus a
// weight table -- adding a behaviour is a row, not a new code path.
//
// `score` returns a CONSIDERATION PRODUCT in [0,1]: "how much does the
// situation call for this", with 0 an outright veto. It must NOT encode
// priority (that is the band) or preference (that is the weight) -- keeping
// those three concerns in separate places is what lets weights be retuned
// without disturbing the guarantees the band hierarchy provides.
struct ActivityDef {
    ActivityId id;
    ActivityBand band;
    ScoreFn score;
    ActFn act;
};

// --- shared blocks ----------------------------------------------------------
// (The hero-only blocks that used to live here -- GoHome, Buy, VisitTavern,
// Chat, Hunt, Explore -- died with the C++ hero decision layer; the Nim/wasm
// brain is the sole implementation now, scripts/brains/nim/blocks.nim.)
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
