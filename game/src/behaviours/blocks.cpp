#include "behaviours/blocks.h"

#include "behaviours/rng.h"

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

// --- RestUrgent -------------------------------------------------------------
float score_rest_urgent(const WorldView& v, const SimFactors& f) {
    if (!v.has_home) {
        return kNotApplicable;  // nowhere to collapse; GoHome cannot help either
    }
    return v.fatigue >= f.hero.fatigue_urgent ? kApplies : kNotApplicable;
}
BehaviourResult act_rest_urgent(const WorldView& v, const SimFactors&) {
    return {Behavior::RestUrgent, v.home_door, Command{CommandKind::EnterHome, v.slot}, true};
}

// --- Chat -------------------------------------------------------------------
float score_chat(const WorldView& v, const SimFactors& f) {
    if (v.chatting) {
        return kApplies;  // mid-conversation: see it through
    }
    return (v.has_chat_partner && v.boredom >= f.hero.chat_boredom) ? kApplies : kNotApplicable;
}
BehaviourResult act_chat(const WorldView& v, const SimFactors& f) {
    if (v.chatting) {
        return {Behavior::Chat, v.pos, std::nullopt, false};  // stand and talk
    }
    // Walk over, and strike it up once close enough. The handler re-validates
    // (both present, in range, neither already engaged), so emitting on arrival
    // keeps the approach itself out of the command log.
    BehaviourResult r{Behavior::Chat, v.partner_pos, std::nullopt, false};
    if (v.partner_dist <= f.hero.chat_radius) {
        r.follow_up = Command{CommandKind::Chat, v.slot, v.partner_slot};
    }
    return r;
}

// --- Explore ----------------------------------------------------------------
float score_explore(const WorldView& v, const SimFactors& f) {
    if (!v.has_explore_goal) {
        return kNotApplicable;  // nowhere unknown within reach, or not in the mood
    }
    if (v.move_blocked) {
        return kNotApplicable;  // the world said no; try elsewhere next window
    }
    if (v.fatigue >= f.hero.explore_max_fatigue) {
        return kNotApplicable;  // too tired -- rest, an errand, anything nearer
    }
    if (v.has_prey) {
        return kNotApplicable;  // something worth stopping for is right here
    }
    return kApplies;
}
BehaviourResult act_explore(const WorldView& v, const SimFactors&) {
    return {Behavior::Explore, v.explore_goal, std::nullopt, false};
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
