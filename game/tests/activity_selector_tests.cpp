// THE CONTRACT of the decision core, tested at the level of mechanisms rather
// than of current policy.
//
// Everything here must hold for ANY weights, ANY considerations, and ANY
// activity table -- so retuning the shipping numbers must never touch this
// file, and a reimplementation (the planned noiser brain) is correct exactly
// when it still satisfies these. That is the whole point: this file is the
// specification, game/src/behaviours is one implementation of it.
//
// The model is TWO tiers: Danger (immediate danger, pre-empts everything) and
// Normal (everything else, ordered by weight x need). There is no third
// "worthiness" category, and these tests must never assume one -- the moment a
// test sorts activities into more than danger/not-danger it has smuggled the
// banned taxonomy back in. Policy ("what do the shipping weights do") lives in
// behaviours_tests.cpp instead.

#include "behaviours/blocks.h"
#include "behaviours/selectors.h"
#include "behaviours/world_view.h"

#include <catch_amalgamated.hpp>

#include <array>

using namespace badlands;

namespace {

// --- synthetic activities ---------------------------------------------------
// Deliberately NOT the shipping blocks: the contract is about the selector, so
// the activities under it are stubs whose scores we control exactly.

BehaviourResult act_marker(ActivityId id, const WorldView& v) {
    return {id, v.pos, std::nullopt, false};
}

// Each stub reads one WorldView field as its score, so a test can dial any
// activity's consideration to any value in [0,1] without new machinery.
float score_danger(const WorldView& v, const SimFactors&) { return v.fatigue; }
float score_x(const WorldView& v, const SimFactors&) { return v.content; }
float score_y(const WorldView& v, const SimFactors&) { return v.tod; }

BehaviourResult stub_flee(const WorldView& v, const SimFactors&) {
    return act_marker(ActivityId::Flee, v);
}
BehaviourResult stub_explore(const WorldView& v, const SimFactors&) {
    return act_marker(ActivityId::Explore, v);
}
BehaviourResult stub_roam(const WorldView& v, const SimFactors&) {
    return act_marker(ActivityId::Roam, v);
}

// One Danger activity and two Normal rivals -- enough to observe both the tier
// boundary (Danger vs Normal) and the within-Normal contest (Explore vs Roam),
// which is the whole of the model.
//   Flee    -> Danger  (score = view.fatigue, ab-used as a dial)
//   Explore -> Normal  (score = view.content)
//   Roam    -> Normal  (score = view.tod)
constexpr std::array<ActivityDef, 3> kTable{{
    {ActivityId::Flee, ActivityBand::Danger, score_danger, stub_flee},
    {ActivityId::Explore, ActivityBand::Normal, score_x, stub_explore},
    {ActivityId::Roam, ActivityBand::Normal, score_y, stub_roam},
}};

ActivityWeights uniform_weights(float value) {
    ActivityWeights w;
    for (int32_t i = 0; i < kActivityCount; ++i) {
        w.w[i] = value;
    }
    return w;
}

WorldView view_with(float danger, float x, float y) {
    WorldView v;
    v.pos = {1.0f, 2.0f};
    v.fatigue = danger;
    v.content = x;
    v.tod = y;
    return v;
}

}  // namespace

TEST_CASE("contract: immediate danger cannot be outweighed by anything Normal") {
    // The one structural guarantee: safety is not a tuning outcome. A Danger
    // activity with any positive score beats Normal activities carrying
    // overwhelming weights and maximal scores.
    const SimFactors f;
    const WorldView v = view_with(/*danger=*/0.001f, /*x=*/1.0f, /*y=*/1.0f);

    for (float normal_weight : {1.0f, 10.0f, 1000.0f, 1e6f}) {
        ActivityWeights w = uniform_weights(normal_weight);
        w.set(ActivityId::Flee, 0.001f);  // danger weighted a millionth as much
        INFO("normal-band weight " << normal_weight);
        CHECK(select_banded(kTable, w, v, f).id == ActivityId::Flee);
    }
}

TEST_CASE("contract: Normal is consulted only once Danger is absent") {
    const SimFactors f;
    const ActivityWeights w = uniform_weights(1.0f);

    // Danger present -> Danger, whatever Normal scores.
    CHECK(select_banded(kTable, w, view_with(1.0f, 1.0f, 1.0f), f).id == ActivityId::Flee);
    // Danger vetoed (score 0) -> the Normal contest decides.
    CHECK(select_banded(kTable, w, view_with(0.0f, 1.0f, 0.5f), f).id == ActivityId::Explore);
    CHECK(select_banded(kTable, w, view_with(0.0f, 0.5f, 1.0f), f).id == ActivityId::Roam);
}

TEST_CASE("contract: a zero score is an absolute veto, whatever the weight") {
    // Hard preconditions ("is there a tavern at all") are expressed as a 0
    // factor, so they can never be out-weighted -- no threshold to tune.
    const SimFactors f;
    const WorldView v = view_with(/*danger=*/0.0f, /*x=*/0.0f, /*y=*/1.0f);

    for (float weight : {1.0f, 100.0f, 1e9f}) {
        ActivityWeights w = uniform_weights(1.0f);
        w.set(ActivityId::Flee, weight);
        w.set(ActivityId::Explore, weight);
        INFO("vetoed-activity weight " << weight);
        CHECK(select_banded(kTable, w, v, f).id == ActivityId::Roam);
    }
}

TEST_CASE("contract: a zero weight removes the activity entirely") {
    // This is how a class gets a unique activity set without a separate code
    // path: the table is shared, the weight decides existence.
    const SimFactors f;
    const WorldView v = view_with(1.0f, 1.0f, 1.0f);

    ActivityWeights w = uniform_weights(1.0f);
    w.set(ActivityId::Flee, 0.0f);
    CHECK(select_banded(kTable, w, v, f).id == ActivityId::Explore);

    w.set(ActivityId::Explore, 0.0f);
    CHECK(select_banded(kTable, w, v, f).id == ActivityId::Roam);

    w.set(ActivityId::Roam, 0.0f);
    CHECK(select_banded(kTable, w, v, f).id == ActivityId::Idle);  // nothing left
}

TEST_CASE("contract: within Normal, higher weight x score wins") {
    const SimFactors f;
    ActivityWeights w = uniform_weights(1.0f);
    WorldView v = view_with(0.0f, /*Explore=*/0.5f, /*Roam=*/0.4f);
    CHECK(select_banded(kTable, w, v, f).id == ActivityId::Explore);

    // Same situation, but the actor cares 10x more about the other option.
    w.set(ActivityId::Roam, 10.0f);
    CHECK(select_banded(kTable, w, v, f).id == ActivityId::Roam);

    // ...and a strong enough need flips it back: weight scales the score, it
    // does not replace it.
    v.content = 1.0f;
    w.set(ActivityId::Explore, 5.0f);  // 5*1.0 > 10*0.4
    CHECK(select_banded(kTable, w, v, f).id == ActivityId::Explore);
}

TEST_CASE("contract: raising an activity's weight never loses it a contest it won") {
    // Monotonicity. Without it, "tune this number up to see more of X" would
    // not be a safe thing for a designer to do.
    const SimFactors f;
    for (float x_score : {0.1f, 0.5f, 1.0f}) {
        for (float y_score : {0.1f, 0.5f, 1.0f}) {
            const WorldView v = view_with(0.0f, x_score, y_score);
            ActivityWeights w = uniform_weights(1.0f);
            if (select_banded(kTable, w, v, f).id != ActivityId::Explore) {
                continue;  // only assert about contests Explore already wins
            }
            for (float raised : {1.5f, 4.0f, 50.0f}) {
                w.set(ActivityId::Explore, raised);
                INFO("scores " << x_score << "/" << y_score << " weight " << raised);
                CHECK(select_banded(kTable, w, v, f).id == ActivityId::Explore);
            }
        }
    }
}

TEST_CASE("contract: an empty Danger band falls through to Normal") {
    // What lets a need yield WITHOUT the tier bending: the Danger activity
    // disqualifies itself (score 0) and the loop moves on to Normal.
    const SimFactors f;
    const ActivityWeights w = uniform_weights(1.0f);
    CHECK(select_banded(kTable, w, view_with(0.0f, 1.0f, 0.5f), f).id == ActivityId::Explore);
}

TEST_CASE("contract: no applicable activity yields Idle in place") {
    const SimFactors f;
    const ActivityWeights w = uniform_weights(1.0f);
    const WorldView v = view_with(0.0f, 0.0f, 0.0f);

    const BehaviourResult r = select_banded(kTable, w, v, f);
    CHECK(r.id == ActivityId::Idle);
    CHECK(r.target.x == 0.0f);  // BehaviourResult's default: hold position
    CHECK(r.target.y == 0.0f);
}

TEST_CASE("contract: selection is deterministic and ties keep the earliest") {
    const SimFactors f;
    const ActivityWeights w = uniform_weights(1.0f);
    const WorldView v = view_with(0.0f, 0.5f, 0.5f);  // exact tie in Normal

    constexpr std::array<ActivityDef, 2> tied{{
        {ActivityId::Explore, ActivityBand::Normal, score_x, stub_explore},
        {ActivityId::Roam, ActivityBand::Normal, score_y, stub_roam},
    }};
    constexpr std::array<ActivityDef, 2> tied_reversed{{
        {ActivityId::Roam, ActivityBand::Normal, score_y, stub_roam},
        {ActivityId::Explore, ActivityBand::Normal, score_x, stub_explore},
    }};

    for (int i = 0; i < 8; ++i) {
        CHECK(select_banded(tied, w, v, f).id == ActivityId::Explore);
    }
    // The tie-break is list order, and nothing else.
    CHECK(select_banded(tied_reversed, w, v, f).id == ActivityId::Roam);
}

TEST_CASE("contract: the activity catalog is dense, named, and banded") {
    // Every consumer (UI labels, the statistics histogram) indexes the catalog
    // by id, so a new activity must never be able to leave a hole behind.
    const std::span<const ActivityInfo> catalog = ActivityCatalog();
    REQUIRE(catalog.size() == static_cast<size_t>(kActivityCount));

    for (int32_t i = 0; i < kActivityCount; ++i) {
        INFO("activity id " << i);
        CHECK(static_cast<int32_t>(catalog[i].id) == i);
        REQUIRE(catalog[i].name != nullptr);
        CHECK(catalog[i].name[0] != '\0');
        CHECK(catalog[i].band != ActivityBand::Count);
        CHECK(ActivityName(i) == catalog[i].name);
        CHECK(ActivityInfoOf(i).id == catalog[i].id);
    }

    CHECK(std::string(ActivityName(-1)) == "-");
    CHECK(std::string(ActivityName(kActivityCount)) == "-");
    CHECK(ActivityInfoOf(-1).id == ActivityId::Idle);
}

TEST_CASE("contract: only immediate-danger activities sit in the Danger band") {
    // The two-tier model, pinned: exactly the responses to immediate danger are
    // Danger; everything else is Normal and competes on need. If a future
    // activity is filed under Danger it should be because it pre-empts, not
    // because it feels important.
    for (const ActivityInfo& info : ActivityCatalog()) {
        INFO("activity " << info.name);
        const bool is_danger = info.band == ActivityBand::Danger;
        const bool is_danger_response =
            info.id == ActivityId::Flee || info.id == ActivityId::Combat;
        CHECK(is_danger == is_danger_response);
    }
}
