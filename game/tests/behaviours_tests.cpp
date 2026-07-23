// The behaviour library, tested WITHOUT a sim: blocks are pure functions of a
// WorldView + SimFactors, so a synthetic view exercises them directly. This is
// the payoff of "WorldView is the only perception path".
//
// These are DEFAULT-POLICY tests: they pin what the shipping weights currently
// produce (the canonical hero loop), and they run against the SHIPPING activity
// table via hero_activities() rather than a copy of it. The mechanism-level
// guarantees that must hold for ANY weights live in activity_selector_tests.cpp
// -- keep the two apart, so retuning a weight touches only this file and
// swapping the implementation touches only that one.

#include "behaviours/blocks.h"
#include "behaviours/selectors.h"
#include "behaviours/world_view.h"
#include "components.h"  // kInventoryCap
#include "town_brain.h"  // hero_activities()

#include <catch_amalgamated.hpp>

#include <array>

using namespace badlands;

namespace {

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

// A plain (non-hunter) hero's shipping weights.
const ActivityWeights& mercenary_weights(const SimFactors& f) {
    return f.hero.weights[HERO_MERCENARY];
}

ActivityId decide(const WorldView& v, const SimFactors& f) {
    return select_banded(hero_activities(), mercenary_weights(f), v, f).id;
}

}  // namespace

TEST_CASE("blocks report applicability, not priority") {
    // Scores are considerations in [0,1]: "does this apply", never "how
    // important is it". Ordering lives in the weight table, which is exactly
    // what makes it retunable as data and different per class.
    const SimFactors f;
    WorldView v = hero_view();

    // Fresh hero, not tired, not bored, empty pack: only Buy (errand), Roam, Idle.
    CHECK(score_go_home(v, f) == 0.0f);
    CHECK(score_buy(v, f) > 0.0f);            // inventory 0 < cap, apothecary present
    CHECK(score_visit_tavern(v, f) == 0.0f);  // not bored
    CHECK(score_roam(v, f) > 0.0f);           // always applicable
    CHECK(score_idle(v, f) > 0.0f);

    // No apothecary -> Buy drops out.
    v.has_apothecary = false;
    CHECK(score_buy(v, f) == 0.0f);

    // Every score stays inside the consideration range.
    v = hero_view();
    v.fatigue = 1.0f;
    v.boredom = 1.0f;
    for (const ActivityDef& a : hero_activities()) {
        const float s = a.score(v, f);
        INFO("activity " << ActivityName(static_cast<int32_t>(a.id)));
        CHECK(s >= 0.0f);
        CHECK(s <= 1.0f);
    }
}

TEST_CASE("the default weights reproduce the canonical hero loop") {
    // GoHome > Buy > VisitTavern > Roam, expressed now as weights rather than
    // as tiers baked into the scores. Retuning these numbers is expected to
    // change this test -- that is the point of it being a policy test.
    const SimFactors f;
    WorldView v = hero_view();
    v.fatigue = 0.7f;  // tired (but not spent) -> GoHome applies
    v.boredom = 1.0f;  // bored    -> VisitTavern applies
    v.inventory = 0;   // empty    -> Buy applies

    CHECK(decide(v, f) == ActivityId::GoHome);

    v.fatigue = 0.0f;  // no longer tired
    CHECK(decide(v, f) == ActivityId::Buy);

    v.inventory = kInventoryCap;  // pack full
    CHECK(decide(v, f) == ActivityId::VisitTavern);

    v.boredom = 0.0f;  // content
    CHECK(decide(v, f) == ActivityId::Roam);
}

TEST_CASE("a tired hero chooses GoHome, targeting the home door") {
    const SimFactors f;
    WorldView v = hero_view();
    v.fatigue = 0.7f;
    const BehaviourResult r =
        select_banded(hero_activities(), mercenary_weights(f), v, f);
    CHECK(r.id == ActivityId::GoHome);
    CHECK(r.target.x == v.home_door.x);
    CHECK(r.target.y == v.home_door.y);
    REQUIRE(r.follow_up.has_value());
    CHECK(r.follow_up->kind == CommandKind::EnterHome);
}

TEST_CASE("an exhausted hero rests as a Danger-band matter") {
    // Same walk home, different activity: past fatigue_urgent, rest stops being
    // leisure and pre-empts everything -- including a hunt, and including any
    // errand a weight could otherwise favour.
    const SimFactors f;
    WorldView v = hero_view();
    v.fatigue = 1.0f;
    v.boredom = 1.0f;
    v.has_prey = true;  // even with prey right there
    v.prey_dist = 1.0f;

    const BehaviourResult r =
        select_banded(hero_activities(), f.hero.weights[HERO_HUNTER], v, f);
    CHECK(r.id == ActivityId::RestUrgent);
    CHECK(r.target.x == v.home_door.x);
    REQUIRE(r.follow_up.has_value());
    CHECK(r.follow_up->kind == CommandKind::EnterHome);

    // With nowhere to sleep it does not apply -- and the hero carries on.
    v.has_home = false;
    CHECK(select_banded(hero_activities(), f.hero.weights[HERO_HUNTER], v, f).id !=
          ActivityId::RestUrgent);
}

TEST_CASE("at night the lower fatigue bar sends a mildly-tired hero home") {
    const SimFactors f;  // fatigue_go_home 0.6, fatigue_night 0.2
    WorldView v = hero_view();
    v.fatigue = 0.3f;  // below the day bar, above the night bar

    v.night = false;
    CHECK(decide(v, f) != ActivityId::GoHome);
    v.night = true;
    CHECK(decide(v, f) == ActivityId::GoHome);
}

TEST_CASE("only a hunter perceives and acts on prey") {
    // The class-unique activity, achieved with NO class-specific code path: the
    // same table, gated by one weight being zero.
    const SimFactors f;
    WorldView v = hero_view();
    v.has_prey = true;
    v.prey_pos = {5.0f, 5.0f};
    v.prey_dist = 7.0f;
    v.self_attack_range = 6.0f;

    CHECK(select_banded(hero_activities(), f.hero.weights[HERO_HUNTER], v, f).id ==
          ActivityId::Hunt);
    CHECK(select_banded(hero_activities(), f.hero.weights[HERO_MERCENARY], v, f).id !=
          ActivityId::Hunt);
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
    CHECK(r.id == ActivityId::Roam);
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
    CHECK(r.id == ActivityId::Idle);
    CHECK(r.target.x == 3.0f);
    CHECK(r.target.y == 4.0f);
}
