#include "behaviours/blocks.h"

#include <cmath>

#include "components.h"  // kInventoryCap

namespace badlands {

namespace {

// Scores are CONSIDERATIONS in [0,1] -- "how much does the situation call for
// this" -- never priorities and never preferences. Priority is the band
// (ActivityBand) and preference is the weight (ActivityWeights); keeping the
// three apart is what lets weights be retuned, or this whole file be replaced
// by a noiser implementation, without disturbing the band guarantees.
//
// Today most blocks are binary applicability (kApplies / 0). Real curves --
// "how tired am I", "how much unexplored ground is near" -- slot in here
// without any selector or table change, which is the point of the shape.
constexpr float kApplies = 1.0f;
constexpr float kNotApplicable = 0.0f;

// Townfolk still run select_priority (first applicable in list order), so only
// the sign of their scores matters, not the magnitude.
constexpr float kTierVisitTax = kApplies;
constexpr float kTierDeposit = kApplies;

// Deterministic per-entity RNG for the roam goal (xorshift64; seed must be
// non-zero). Identical math to the pre-refactor town_brain so replayed/repeated
// runs stay bit-exact.
uint64_t xorshift(uint64_t& s) {
    s ^= s << 13;
    s ^= s >> 7;
    s ^= s << 17;
    return s;
}
float unit(uint64_t& s) {
    return static_cast<float>(xorshift(s) >> 40) * (1.0f / 16777216.0f);  // [0,1)
}

}  // namespace

// --- GoHome -----------------------------------------------------------------
float score_go_home(const WorldView& v, const SimFactors& f) {
    if (!v.has_home) {
        return 0.0f;
    }
    const bool tired = v.fatigue >= f.hero.fatigue_go_home ||
                       (v.night && v.fatigue >= f.hero.fatigue_night);
    return tired ? kApplies : kNotApplicable;
}
BehaviourResult act_go_home(const WorldView& v, const SimFactors&) {
    return {Behavior::GoHome, v.home_door, Command{CommandKind::EnterHome, v.slot}, true};
}

// --- Buy --------------------------------------------------------------------
float score_buy(const WorldView& v, const SimFactors&) {
    return (v.has_apothecary && v.inventory < kInventoryCap) ? kApplies : kNotApplicable;
}
BehaviourResult act_buy(const WorldView& v, const SimFactors&) {
    return {Behavior::Buy, v.apothecary_door, Command{CommandKind::Buy, v.slot}, true};
}

// --- VisitTavern ------------------------------------------------------------
float score_visit_tavern(const WorldView& v, const SimFactors& f) {
    return (v.has_tavern && !v.night && v.boredom >= f.hero.boredom_tavern) ? kApplies
                                                                            : kNotApplicable;
}
BehaviourResult act_visit_tavern(const WorldView& v, const SimFactors&) {
    Command enter{CommandKind::EnterBuilding, v.slot, UINT32_MAX, {0.0f, 0.0f},
                  static_cast<int32_t>(BuildingKind::Tavern)};
    return {Behavior::VisitTavern, v.tavern_door, enter, true};
}

// --- Hunt (hunter) ----------------------------------------------------------
float score_hunt(const WorldView& v, const SimFactors&) {
    return v.has_prey ? kApplies : kNotApplicable;
}
BehaviourResult act_hunt(const WorldView& v, const SimFactors&) {
    BehaviourResult r{Behavior::Hunt, v.prey_pos, std::nullopt, false};
    // Chase to prey_pos; once within the hunter's own reach, take the shot (the
    // handler re-checks range + cooldown, so the log gets one entry per shot).
    if (v.prey_dist <= v.self_attack_range) {
        r.follow_up = Command{CommandKind::Shoot, v.slot, v.prey_slot};
    }
    return r;
}

// --- Roam (shared) ----------------------------------------------------------
glm::vec2 roam_point(uint32_t slot, int64_t epoch, glm::vec2 anchor, float radius) {
    // Seed off the slot and the roam epoch so the goal is stable within a lease
    // window yet unique per entity -- deterministic (no wall-clock, no unseeded
    // RNG). Math is unchanged from the pre-library town brain, so repeated and
    // replayed runs stay bit-exact.
    uint64_t s = (static_cast<uint64_t>(slot) * 2654435761ull) ^
                 (static_cast<uint64_t>(epoch) + 1ull);
    if (s == 0) {
        s = 1;
    }
    const float ang = unit(s) * 6.2831853f;
    const float rad = unit(s) * radius;
    return anchor + glm::vec2{std::cos(ang) * rad, std::sin(ang) * rad};
}
float score_roam(const WorldView&, const SimFactors&) { return kApplies; }
BehaviourResult act_roam(const WorldView& v, const SimFactors&) {
    return {Behavior::Roam, v.roam_goal, std::nullopt, false};
}

// --- Flee (shared) ----------------------------------------------------------
float score_flee(const WorldView& v, const SimFactors& f) {
    return (v.has_threat && v.threat_dist <= f.critter.flee_radius) ? kApplies : kNotApplicable;
}
BehaviourResult act_flee(const WorldView& v, const SimFactors& f) {
    glm::vec2 away = v.pos - v.threat_pos;
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
