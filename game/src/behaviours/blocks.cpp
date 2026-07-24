#include "behaviours/blocks.h"

#include "behaviours/rng.h"

#include <cmath>

namespace badlands {

namespace {

// Scores are CONSIDERATIONS in [0,1] -- "how much does the situation call for
// this right now" -- never priorities and never preferences. There is no
// worthiness ranking of activities anywhere.
//
// For one that is simply available or not, it is kApplies / kNotApplicable, and
// the per-class weight scales it into the same contest.
constexpr float kApplies = 1.0f;
constexpr float kNotApplicable = 0.0f;

// Townfolk still run select_priority (first applicable in list order), so only
// the sign of their scores matters, not the magnitude.
constexpr float kTierVisitTax = kApplies;
constexpr float kTierDeposit = kApplies;

}  // namespace

// --- Roam (shared) ----------------------------------------------------------
glm::vec2 roam_point(uint32_t slot, int64_t epoch, glm::vec2 anchor, float radius) {
    // Seeded off the slot and the roam epoch so the goal is stable within a
    // lease window yet unique per entity -- deterministic, no wall-clock, no
    // global RNG state. Shares seed_of with every other draw in the sim, so the
    // same avalanche that fixed the exploration appetite applies here (this
    // used to keep its own weaker copy of the mix).
    uint64_t s = seed_of(slot, epoch);
    const float ang = unit_float(s) * 6.2831853f;
    const float rad = unit_float(s) * radius;
    return anchor + glm::vec2{std::cos(ang) * rad, std::sin(ang) * rad};
}
float score_roam(const WorldView&, const SimFactors&) { return kApplies; }
BehaviourResult act_roam(const WorldView& v, const SimFactors&) {
    return {Behavior::Roam, v.roam_goal, std::nullopt, false};
}

// --- Flee (shared) ----------------------------------------------------------
float score_flee(const WorldView& v, const SimFactors& f) {
    return (has_threat(v) && nearest_threat_dist(v) <= f.critter.flee_radius) ? kApplies
                                                                              : kNotApplicable;
}
BehaviourResult act_flee(const WorldView& v, const SimFactors& f) {
    glm::vec2 away = v.pos - nearest_threat_pos(v);
    const float len = glm::length(away);
    away = (len > 1e-4f) ? away / len : glm::vec2{1.0f, 0.0f};  // degenerate: pick a dir
    // Reports its own id rather than masquerading as Roam: a bolt and a wander
    // are different goals, and the statistics histogram must be able to tell
    // "the herd is panicking" from "the herd is grazing".
    return {Behavior::Flee, v.pos + away * f.critter.flee_distance, std::nullopt, false};
}

// --- Idle -------------------------------------------------------------------
float score_idle(const WorldView&, const SimFactors&) { return kApplies; }
BehaviourResult act_idle(const WorldView& v, const SimFactors&) {
    return {Behavior::Idle, v.pos, std::nullopt, false};
}

// --- Graze (critter) --------------------------------------------------------
float score_graze(const WorldView& v, const SimFactors&) {
    return v.grazing ? kApplies : kNotApplicable;
}
BehaviourResult act_graze(const WorldView& v, const SimFactors&) {
    return {Behavior::Graze, v.pos, std::nullopt, false};  // hold and feed
}

// --- VisitNextTaxable (townfolk) --------------------------------------------
float score_visit_taxable(const WorldView& v, const SimFactors&) {
    return v.has_tax_target ? kTierVisitTax : kNotApplicable;
}
BehaviourResult act_visit_taxable(const WorldView& v, const SimFactors&) {
    Command collect{CommandKind::CollectTax, v.slot, v.tax_target_id};
    return {Behavior::VisitTax, v.tax_target_door, collect, true};
}

// --- Deposit (townfolk) -----------------------------------------------------
float score_deposit(const WorldView& v, const SimFactors&) {
    return v.has_deposit ? kTierDeposit : kNotApplicable;
}
BehaviourResult act_deposit(const WorldView& v, const SimFactors&) {
    return {Behavior::Deposit, v.deposit_door, Command{CommandKind::Deposit, v.slot}, true};
}

}  // namespace badlands
