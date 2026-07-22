// The behaviour library, tested WITHOUT a sim: blocks are pure functions of a
// WorldView + SimFactors, so a synthetic view exercises them directly. This is
// the payoff of "WorldView is the only perception path".
//
// It also pins the two properties the port relies on: for the hero list, argmax
// and priority agree (so which selector drives the town brain is immaterial and
// the old if-chain behaviour is preserved), and a shared block behaves
// identically whichever archetype's view feeds it.

#include "behaviours/blocks.h"
#include "behaviours/selectors.h"
#include "behaviours/world_view.h"
#include "components.h"  // kInventoryCap

#include <catch_amalgamated.hpp>

#include <array>

using namespace badlands;

namespace {

constexpr std::array<Candidate, 5> kHeroBlocks{{
    {score_go_home, act_go_home},
    {score_buy, act_buy},
    {score_visit_tavern, act_visit_tavern},
    {score_roam, act_roam},
    {score_idle, act_idle},
}};

// A hero standing at the origin with a home/apothecary/tavern all present.
WorldView hero_view() {
    WorldView v;
    v.slot = 0;
    v.pos = {0.0f, 0.0f};
    v.has_home = true;
    v.home_door = {-10.0f, 0.0f};
    v.has_apothecary = true;
    v.apothecary_door = {10.0f, 0.0f};
    v.has_tavern = true;
    v.tavern_door = {0.0f, 10.0f};
    return v;
}

}  // namespace

TEST_CASE("blocks score their tier only when applicable") {
    const SimFactors f;
    WorldView v = hero_view();

    // Fresh hero, not tired, not bored, empty pack: only Buy (errand), Roam, Idle.
    CHECK(score_go_home(v, f) == 0.0f);
    CHECK(score_buy(v, f) > 0.0f);           // inventory 0 < cap, apothecary present
    CHECK(score_visit_tavern(v, f) == 0.0f);  // not bored
    CHECK(score_roam(v, f) > 0.0f);           // always applicable
    CHECK(score_idle(v, f) > 0.0f);

    // No apothecary -> Buy drops out.
    v.has_apothecary = false;
    CHECK(score_buy(v, f) == 0.0f);
}

TEST_CASE("GoHome outranks Buy which outranks VisitTavern (the priority chain)") {
    const SimFactors f;
    WorldView v = hero_view();
    v.fatigue = 1.0f;                          // tired -> GoHome applies
    v.boredom = 1.0f;                          // bored -> VisitTavern applies
    v.inventory = 0;                           // empty -> Buy applies
    CHECK(score_go_home(v, f) > score_buy(v, f));
    CHECK(score_buy(v, f) > score_visit_tavern(v, f));
    CHECK(score_visit_tavern(v, f) > score_roam(v, f));
    CHECK(score_roam(v, f) > score_idle(v, f));
}

TEST_CASE("argmax and priority agree on the hero list") {
    // The property the port leans on: the town brain's decision does not depend
    // on which selector drives it, so replacing the if-chain changes nothing.
    const SimFactors f;
    for (float fatigue : {0.0f, 0.7f}) {
        for (float boredom : {0.0f, 0.7f}) {
            for (int inv : {0, kInventoryCap}) {
                for (bool night : {false, true}) {
                    WorldView v = hero_view();
                    v.fatigue = fatigue;
                    v.boredom = boredom;
                    v.inventory = inv;
                    v.night = night;
                    const BehaviourResult a = select_argmax(kHeroBlocks, v, f);
                    const BehaviourResult p = select_priority(kHeroBlocks, v, f);
                    INFO("fatigue=" << fatigue << " boredom=" << boredom << " inv=" << inv
                                    << " night=" << night);
                    CHECK(a.id == p.id);
                    CHECK(a.target.x == p.target.x);
                    CHECK(a.target.y == p.target.y);
                }
            }
        }
    }
}

TEST_CASE("a tired hero chooses GoHome, targeting the home door") {
    const SimFactors f;
    WorldView v = hero_view();
    v.fatigue = 1.0f;
    const BehaviourResult r = select_argmax(kHeroBlocks, v, f);
    CHECK(r.id == Behavior::GoHome);
    CHECK(r.target.x == v.home_door.x);
    CHECK(r.target.y == v.home_door.y);
    REQUIRE(r.follow_up.has_value());
    CHECK(r.follow_up->kind == CommandKind::EnterHome);
}

TEST_CASE("at night the lower fatigue bar sends a mildly-tired hero home") {
    const SimFactors f;  // fatigue_go_home 0.6, fatigue_night 0.2
    WorldView v = hero_view();
    v.fatigue = 0.3f;  // below the day bar, above the night bar

    v.night = false;
    CHECK(select_argmax(kHeroBlocks, v, f).id != Behavior::GoHome);
    v.night = true;
    CHECK(select_argmax(kHeroBlocks, v, f).id == Behavior::GoHome);
}

TEST_CASE("roam_point is deterministic, anchored and radius-bounded") {
    // The wander goal is drawn in perception (roam_point) and carried in
    // view.roam_goal; act_roam just walks to it.
    const glm::vec2 origin{0.0f, 0.0f};

    const glm::vec2 a = roam_point(7, 42, origin, 6.0f);
    const glm::vec2 b = roam_point(7, 42, origin, 6.0f);
    CHECK(a.x == b.x);  // pure function of (slot, epoch, anchor, radius)
    CHECK(a.y == b.y);
    CHECK(glm::length(a) <= 6.0f + 1e-4f);

    // A different epoch generally moves the goal; a different slot too.
    const glm::vec2 c = roam_point(7, 43, origin, 6.0f);
    CHECK((c.x != a.x || c.y != a.y));
    const glm::vec2 d = roam_point(8, 42, origin, 6.0f);
    CHECK((d.x != a.x || d.y != a.y));

    // A wider radius reaches further; a non-origin anchor shifts the whole draw.
    CHECK(glm::length(roam_point(7, 42, origin, 100.0f)) > 6.0f);
    const glm::vec2 shifted = roam_point(7, 42, {50.0f, 50.0f}, 6.0f);
    CHECK(glm::length(shifted - glm::vec2{50.0f, 50.0f}) <= 6.0f + 1e-4f);
}

TEST_CASE("act_roam walks to the perceived roam goal") {
    const SimFactors f;
    WorldView v;
    v.roam_goal = {12.0f, -3.0f};
    const BehaviourResult r = act_roam(v, f);
    CHECK(r.id == Behavior::Roam);
    CHECK(r.target.x == 12.0f);
    CHECK(r.target.y == -3.0f);
}

TEST_CASE("select returns Idle-in-place when nothing is applicable") {
    const SimFactors f;
    WorldView v;  // no buildings, no needs
    v.pos = {3.0f, 4.0f};
    // Only Roam + Idle score; a priority list of just [Idle] must hold position.
    const std::array<Candidate, 1> idle_only{{{score_idle, act_idle}}};
    const BehaviourResult r = select_priority(idle_only, v, f);
    CHECK(r.id == Behavior::Idle);
    CHECK(r.target.x == 3.0f);
    CHECK(r.target.y == 4.0f);
}
