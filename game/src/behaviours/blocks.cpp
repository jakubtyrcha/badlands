#include "behaviours/blocks.h"

#include "behaviours/rng.h"

#include <algorithm>
#include <cmath>

#include "components.h"  // kInventoryCap

namespace badlands {

namespace {

// Scores are CONSIDERATIONS in [0,1] -- "how much does the situation call for
// this right now" -- never priorities and never preferences. There is no
// worthiness ranking of activities anywhere: within the Normal band a hero
// rests rather than hunts because it is TIRED, and stops doing so the moment it
// is not.
//
// For a need-driven activity the score is the need's URGENCY (see urgency()).
// For one that is simply available or not, it is kApplies / kNotApplicable, and
// the per-class weight scales it into the same contest.
constexpr float kApplies = 1.0f;
constexpr float kNotApplicable = 0.0f;

// How badly a depleted reserve wants attention: 0 at or above `threshold`,
// ramping linearly to 1 when the reserve is empty. One shape, used by every
// need, with the threshold as the per-need dial.
float urgency(float reserve, float threshold) {
    if (threshold <= 0.0f || reserve >= threshold) {
        return kNotApplicable;
    }
    return std::clamp((threshold - reserve) / threshold, 0.0f, 1.0f);
}

// Townfolk still run select_priority (first applicable in list order), so only
// the sign of their scores matters, not the magnitude.
constexpr float kTierVisitTax = kApplies;
constexpr float kTierDeposit = kApplies;

}  // namespace

// --- GoHome (rest) ----------------------------------------------------------
// Scored on three things, exactly as specified: how spent the hero is, whether
// it is the sleep window, and whether it is hurt. The night raises the bar at
// which turning in starts to appeal (a hero will go to bed merely tired-ish
// after dark, but pushes on through the day); injury urges rest on its own,
// whatever the reserve says -- hence max() rather than a product, which would
// let a well-rested hero bleed out on its feet.
float score_go_home(const WorldView& v, const SimFactors& f) {
    if (!v.has_home) {
        return kNotApplicable;
    }
    const float bar = v.night ? f.hero.fatigue_seek_night : f.hero.fatigue_seek;
    return std::max(urgency(v.fatigue, bar), urgency(v.health_frac, f.hero.low_health_rest));
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
    if (!v.has_tavern || v.night) {
        return kNotApplicable;  // shut after dark
    }
    return urgency(v.content, f.hero.content_seek);
}
BehaviourResult act_visit_tavern(const WorldView& v, const SimFactors&) {
    Command enter{CommandKind::EnterBuilding, v.slot, UINT32_MAX, {0.0f, 0.0f},
                  static_cast<int32_t>(BuildingKind::Tavern)};
    return {Behavior::VisitTavern, v.tavern_door, enter, true};
}

// --- Chat -------------------------------------------------------------------
float score_chat(const WorldView& v, const SimFactors& f) {
    if (v.chatting) {
        return kApplies;  // mid-conversation: see it through
    }
    if (!v.has_chat_partner) {
        return kNotApplicable;
    }
    return urgency(v.content, f.hero.chat_content_seek);
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
    if (v.fatigue <= f.hero.explore_min_fatigue) {
        return kNotApplicable;  // not enough in the tank -- stay near home
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
        r.follow_up = Command{CommandKind::Attack, v.slot, v.prey_slot};
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
