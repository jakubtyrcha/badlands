#include "behaviours/blocks.h"

#include <cmath>

#include "components.h"  // kInventoryCap

namespace badlands {

namespace {

// Behaviour tiers. A block scores its tier when applicable, else 0, so a
// higher-tier applicable block always wins the argmax -- reproducing the old
// town_brain priority chain (GoHome > Buy > VisitTavern > Roam > Idle).
constexpr float kTierGoHome = 5.0f;
constexpr float kTierBuy = 4.0f;
constexpr float kTierVisitTavern = 3.0f;
constexpr float kTierRoam = 2.0f;
constexpr float kTierIdle = 1.0f;

// Critter tiers: bolting always beats grazing/roaming.
constexpr float kTierFlee = 10.0f;
constexpr float kTierGraze = 3.0f;

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
    return tired ? kTierGoHome : 0.0f;
}
BehaviourResult act_go_home(const WorldView& v, const SimFactors&) {
    return {Behavior::GoHome, v.home_door, Command{CommandKind::EnterHome, v.slot}, true};
}

// --- Buy --------------------------------------------------------------------
float score_buy(const WorldView& v, const SimFactors&) {
    return (v.has_apothecary && v.inventory < kInventoryCap) ? kTierBuy : 0.0f;
}
BehaviourResult act_buy(const WorldView& v, const SimFactors&) {
    return {Behavior::Buy, v.apothecary_door, Command{CommandKind::Buy, v.slot}, true};
}

// --- VisitTavern ------------------------------------------------------------
float score_visit_tavern(const WorldView& v, const SimFactors& f) {
    return (v.has_tavern && !v.night && v.boredom >= f.hero.boredom_tavern)
               ? kTierVisitTavern
               : 0.0f;
}
BehaviourResult act_visit_tavern(const WorldView& v, const SimFactors&) {
    Command enter{CommandKind::EnterBuilding, v.slot, UINT32_MAX, {0.0f, 0.0f},
                  static_cast<int32_t>(BuildingKind::Tavern)};
    return {Behavior::VisitTavern, v.tavern_door, enter, true};
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
float score_roam(const WorldView&, const SimFactors&) { return kTierRoam; }
BehaviourResult act_roam(const WorldView& v, const SimFactors&) {
    return {Behavior::Roam, v.roam_goal, std::nullopt, false};
}

// --- Flee (shared) ----------------------------------------------------------
float score_flee(const WorldView& v, const SimFactors& f) {
    return (v.has_threat && v.threat_dist <= f.critter.flee_radius) ? kTierFlee : 0.0f;
}
BehaviourResult act_flee(const WorldView& v, const SimFactors& f) {
    glm::vec2 away = v.pos - v.threat_pos;
    const float len = glm::length(away);
    away = (len > 1e-4f) ? away / len : glm::vec2{1.0f, 0.0f};  // degenerate: pick a dir
    return {Behavior::Roam, v.pos + away * f.critter.flee_distance, std::nullopt, false};
}

// --- Idle -------------------------------------------------------------------
float score_idle(const WorldView&, const SimFactors&) { return kTierIdle; }
BehaviourResult act_idle(const WorldView& v, const SimFactors&) {
    return {Behavior::Idle, v.pos, std::nullopt, false};
}

// --- Graze (critter) --------------------------------------------------------
float score_graze(const WorldView& v, const SimFactors&) {
    return v.grazing ? kTierGraze : 0.0f;
}
BehaviourResult act_graze(const WorldView& v, const SimFactors&) {
    return {Behavior::Graze, v.pos, std::nullopt, false};  // hold and feed
}

}  // namespace badlands
