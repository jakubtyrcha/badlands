// The behaviour library, tested WITHOUT a sim: blocks are pure functions of a
// WorldView + SimFactors, so a synthetic view exercises them directly. This is
// the payoff of "WorldView is the only perception path".
//
// These are DEFAULT-POLICY tests: they pin what the shipping weights currently
// produce, and they run against the SHIPPING activity table via
// hero_activities() rather than a copy of it. The mechanism-level guarantees
// that must hold for ANY weights live in activity_selector_tests.cpp -- keep
// the two apart, so retuning a weight touches only this file and swapping the
// implementation touches only that one.
//
// Needs are RESERVES: fatigue/content in [0,1], 1 = satisfied. A hero rests
// because it is TIRED (low fatigue), not because rest is a higher class of
// activity -- there is no such class. What it does falls out of which reserve
// is most depleted relative to its seek threshold.

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

TEST_CASE("a need's score is its urgency, zero until the reserve dips below the bar") {
    // The score of a need-driven activity is how DEPLETED it is past its seek
    // threshold -- 0 above the bar, ramping to 1 at empty. Not a tier, not a
    // flat "applies": the number carries the need's intensity.
    const SimFactors f;
    WorldView v = hero_view();

    // Satisfied -> no urge at all.
    v.fatigue = 1.0f;
    v.content = 1.0f;
    CHECK(score_go_home(v, f) == 0.0f);
    CHECK(score_visit_tavern(v, f) == 0.0f);

    // Below the bar -> a positive, and monotone-increasing, urge.
    v.fatigue = f.hero.fatigue_seek - 0.1f;
    const float mild = score_go_home(v, f);
    CHECK(mild > 0.0f);
    v.fatigue = 0.05f;
    CHECK(score_go_home(v, f) > mild);  // more spent -> more urgent

    // Always-available blocks are flat applicability, gated on preconditions.
    v = hero_view();
    CHECK(score_buy(v, f) > 0.0f);   // room in the pack, apothecary present
    CHECK(score_roam(v, f) > 0.0f);  // the fallback wander
    v.has_apothecary = false;
    CHECK(score_buy(v, f) == 0.0f);

    // Every score stays in [0,1] whatever the reserves.
    v = hero_view();
    v.fatigue = 0.0f;
    v.content = 0.0f;
    for (const ActivityDef& a : hero_activities()) {
        const float s = a.score(v, f);
        INFO("activity " << ActivityName(static_cast<int32_t>(a.id)));
        CHECK(s >= 0.0f);
        CHECK(s <= 1.0f);
    }
}

TEST_CASE("what a hero does falls out of which reserve is most depleted") {
    // No tier orders these: a spent hero rests, a bored-but-rested one drinks,
    // a satisfied one runs errands, an idle one wanders. Retuning the shipping
    // weights is expected to change this test -- that is why it is a policy one.
    const SimFactors f;
    WorldView v = hero_view();

    // Badly tired -> rest wins, whatever else is going on.
    v.fatigue = 0.05f;
    v.content = 0.3f;
    CHECK(decide(v, f) == ActivityId::GoHome);

    // Rested but starved of diversion (daytime) -> the tavern.
    v.fatigue = 1.0f;
    v.content = 0.1f;
    CHECK(decide(v, f) == ActivityId::VisitTavern);

    // Both reserves fine, pack empty -> the errand.
    v.content = 1.0f;
    v.inventory = 0;
    CHECK(decide(v, f) == ActivityId::Buy);

    // Nothing pressing at all -> wander.
    v.inventory = kInventoryCap;
    CHECK(decide(v, f) == ActivityId::Roam);
}

TEST_CASE("a tired hero chooses GoHome, targeting the home door") {
    const SimFactors f;
    WorldView v = hero_view();
    v.fatigue = 0.1f;
    const BehaviourResult r =
        select_banded(hero_activities(), mercenary_weights(f), v, f);
    CHECK(r.id == ActivityId::GoHome);
    CHECK(r.target.x == v.home_door.x);
    CHECK(r.target.y == v.home_door.y);
    REQUIRE(r.follow_up.has_value());
    CHECK(r.follow_up->kind == CommandKind::EnterHome);

    // With nowhere to sleep it does not apply -- and the hero carries on.
    v.has_home = false;
    CHECK(select_banded(hero_activities(), mercenary_weights(f), v, f).id != ActivityId::GoHome);
}

TEST_CASE("a hurt hero wants to lie down whatever its fatigue says") {
    // Rest is urged by low health as well as low fatigue -- the two combine by
    // max(), so a well-rested but wounded hero still heads home rather than
    // bleeding out on its feet.
    const SimFactors f;
    WorldView v = hero_view();
    v.fatigue = 1.0f;                        // fully rested
    v.health_frac = f.hero.low_health_rest - 0.2f;  // but badly hurt
    CHECK(score_go_home(v, f) > 0.0f);

    v.health_frac = 1.0f;  // unhurt AND rested -> no urge
    CHECK(score_go_home(v, f) == 0.0f);
}

TEST_CASE("at night the raised fatigue bar sends a mildly-tired hero home") {
    // The night does not add a tier -- it raises the reserve at which rest
    // starts to appeal, so the SAME fatigue produces a stronger urge to turn in
    // after dark. Errands out of the way (pack full, not bored) so the contest
    // is rest vs the fallback wander.
    const SimFactors f;
    WorldView v = hero_view();
    v.content = 1.0f;
    v.inventory = kInventoryCap;
    // Between the day and night crossover points against Roam.
    v.fatigue = 0.5f;

    v.night = false;
    CHECK(decide(v, f) != ActivityId::GoHome);  // fine by day: wanders
    v.night = true;
    CHECK(decide(v, f) == ActivityId::GoHome);  // ready for bed after dark
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
