// THE CONTRACT of the decision core, tested at the level of mechanisms rather
// than of current policy.
//
// Everything here must hold for ANY weights, ANY considerations, and ANY
// activity table -- so retuning the shipping numbers must never touch this
// file, and a reimplementation (the planned noiser brain) is correct exactly
// when it still satisfies these. That is the whole point: this file is the
// specification, game/src/behaviours is one implementation of it.
//
// Policy ("what do the shipping weights currently do") lives in
// behaviours_tests.cpp instead.

#include "behaviours/blocks.h"
#include "behaviours/selectors.h"
#include "behaviours/world_view.h"

#include <catch_amalgamated.hpp>

#include <array>
#include <vector>

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
// (fatigue / boredom / tod are just convenient float carriers here.)
float score_a(const WorldView& v, const SimFactors&) { return v.fatigue; }
float score_b(const WorldView& v, const SimFactors&) { return v.boredom; }
float score_c(const WorldView& v, const SimFactors&) { return v.tod; }

BehaviourResult act_a(const WorldView& v, const SimFactors&) {
    return act_marker(ActivityId::RestUrgent, v);
}
BehaviourResult act_b(const WorldView& v, const SimFactors&) {
    return act_marker(ActivityId::Explore, v);
}
BehaviourResult act_c(const WorldView& v, const SimFactors&) {
    return act_marker(ActivityId::Roam, v);
}

// One activity per band, so band interactions are directly observable.
//   RestUrgent -> Danger     (score = view.fatigue)
//   Explore    -> Productive (score = view.boredom)
//   Roam       -> Filler     (score = view.tod)
constexpr std::array<ActivityDef, 3> kBanded{{
    {ActivityId::RestUrgent, ActivityBand::Danger, score_a, act_a},
    {ActivityId::Explore, ActivityBand::Productive, score_b, act_b},
    {ActivityId::Roam, ActivityBand::Filler, score_c, act_c},
}};

ActivityWeights uniform_weights(float value) {
    ActivityWeights w;
    for (int32_t i = 0; i < kActivityCount; ++i) {
        w.w[i] = value;
    }
    return w;
}

WorldView view_with(float danger, float productive, float filler) {
    WorldView v;
    v.pos = {1.0f, 2.0f};
    v.fatigue = danger;
    v.boredom = productive;
    v.tod = filler;
    return v;
}

}  // namespace

TEST_CASE("contract: band dominance -- no weight can lift a lower band over a higher one") {
    // The load-bearing guarantee. Safety (fleeing, fighting, collapsing) is
    // structural, not a tuning outcome: a Danger activity with ANY positive
    // consideration beats Productive and Filler activities with overwhelming
    // weights and maximal considerations.
    const SimFactors f;
    const WorldView v = view_with(/*danger=*/0.001f, /*productive=*/1.0f, /*filler=*/1.0f);

    for (float lower_weight : {1.0f, 10.0f, 1000.0f, 1e6f}) {
        ActivityWeights w = uniform_weights(lower_weight);
        w.set(ActivityId::RestUrgent, 0.001f);  // danger weighted a millionth as much
        INFO("lower-band weight " << lower_weight);
        CHECK(select_banded(kBanded, w, v, f).id == ActivityId::RestUrgent);
    }
}

TEST_CASE("contract: bands are ordered Danger > Productive > Filler") {
    const SimFactors f;
    const ActivityWeights w = uniform_weights(1.0f);

    // All three applicable -> Danger.
    CHECK(select_banded(kBanded, w, view_with(1.0f, 1.0f, 1.0f), f).id == ActivityId::RestUrgent);
    // Danger vetoed -> Productive.
    CHECK(select_banded(kBanded, w, view_with(0.0f, 1.0f, 1.0f), f).id == ActivityId::Explore);
    // Danger + Productive vetoed -> Filler.
    CHECK(select_banded(kBanded, w, view_with(0.0f, 0.0f, 1.0f), f).id == ActivityId::Roam);
}

TEST_CASE("contract: a zero score is an absolute veto, whatever the weight") {
    // Hard preconditions ("is there a tavern at all") are expressed as a 0
    // factor, so they can never be out-weighted -- no threshold to tune.
    const SimFactors f;
    const WorldView v = view_with(/*danger=*/0.0f, /*productive=*/0.0f, /*filler=*/1.0f);

    for (float weight : {1.0f, 100.0f, 1e9f}) {
        ActivityWeights w = uniform_weights(1.0f);
        w.set(ActivityId::RestUrgent, weight);
        w.set(ActivityId::Explore, weight);
        INFO("vetoed-activity weight " << weight);
        CHECK(select_banded(kBanded, w, v, f).id == ActivityId::Roam);
    }
}

TEST_CASE("contract: a zero weight removes the activity entirely") {
    // This is how a class gets a unique activity set without a separate code
    // path: the table is shared, the weight decides existence.
    const SimFactors f;
    const WorldView v = view_with(1.0f, 1.0f, 1.0f);

    ActivityWeights w = uniform_weights(1.0f);
    w.set(ActivityId::RestUrgent, 0.0f);
    CHECK(select_banded(kBanded, w, v, f).id == ActivityId::Explore);

    w.set(ActivityId::Explore, 0.0f);
    CHECK(select_banded(kBanded, w, v, f).id == ActivityId::Roam);

    w.set(ActivityId::Roam, 0.0f);
    CHECK(select_banded(kBanded, w, v, f).id == ActivityId::Idle);  // nothing left
}

TEST_CASE("contract: within a band, higher weight x consideration wins") {
    // Two rivals in the SAME band, so only the utility product decides.
    const SimFactors f;
    constexpr std::array<ActivityDef, 2> rivals{{
        {ActivityId::Explore, ActivityBand::Productive, score_b, act_b},
        {ActivityId::Roam, ActivityBand::Productive, score_c, act_c},
    }};

    ActivityWeights w = uniform_weights(1.0f);
    WorldView v = view_with(0.0f, /*Explore=*/0.5f, /*Roam=*/0.4f);
    CHECK(select_banded(rivals, w, v, f).id == ActivityId::Explore);

    // Same situation, but the actor cares 10x more about the other option.
    w.set(ActivityId::Roam, 10.0f);
    CHECK(select_banded(rivals, w, v, f).id == ActivityId::Roam);

    // ...and a strong enough situation flips it back: weight scales the
    // consideration, it does not replace it.
    v.boredom = 1.0f;
    w.set(ActivityId::Explore, 5.0f);  // 5*1.0 > 10*0.4
    CHECK(select_banded(rivals, w, v, f).id == ActivityId::Explore);
}

TEST_CASE("contract: raising an activity's weight never loses it a contest it won") {
    // Monotonicity. Without it, "tune this number up to see more of X" would
    // not be a safe thing for a designer to do.
    const SimFactors f;
    constexpr std::array<ActivityDef, 2> rivals{{
        {ActivityId::Explore, ActivityBand::Productive, score_b, act_b},
        {ActivityId::Roam, ActivityBand::Productive, score_c, act_c},
    }};

    for (float explore_score : {0.1f, 0.5f, 1.0f}) {
        for (float roam_score : {0.1f, 0.5f, 1.0f}) {
            const WorldView v = view_with(0.0f, explore_score, roam_score);
            ActivityWeights w = uniform_weights(1.0f);
            if (select_banded(rivals, w, v, f).id != ActivityId::Explore) {
                continue;  // only assert about contests Explore already wins
            }
            for (float raised : {1.5f, 4.0f, 50.0f}) {
                w.set(ActivityId::Explore, raised);
                INFO("scores " << explore_score << "/" << roam_score << " weight " << raised);
                CHECK(select_banded(rivals, w, v, f).id == ActivityId::Explore);
            }
        }
    }
}

TEST_CASE("contract: an empty band falls through to the next") {
    // What lets an activity yield WITHOUT the band hierarchy bending: it
    // disqualifies itself (score 0) and the loop moves on.
    const SimFactors f;
    const ActivityWeights w = uniform_weights(1.0f);

    // A table with a gap: nothing at all in the Productive band.
    constexpr std::array<ActivityDef, 2> gapped{{
        {ActivityId::RestUrgent, ActivityBand::Danger, score_a, act_a},
        {ActivityId::Roam, ActivityBand::Filler, score_c, act_c},
    }};
    CHECK(select_banded(gapped, w, view_with(0.0f, 1.0f, 1.0f), f).id == ActivityId::Roam);
}

TEST_CASE("contract: no applicable activity yields Idle in place") {
    const SimFactors f;
    const ActivityWeights w = uniform_weights(1.0f);
    const WorldView v = view_with(0.0f, 0.0f, 0.0f);

    const BehaviourResult r = select_banded(kBanded, w, v, f);
    CHECK(r.id == ActivityId::Idle);
    CHECK(r.target.x == 0.0f);  // BehaviourResult's default: hold position
    CHECK(r.target.y == 0.0f);
}

TEST_CASE("contract: selection is deterministic and ties keep the earliest") {
    const SimFactors f;
    const ActivityWeights w = uniform_weights(1.0f);
    const WorldView v = view_with(0.0f, 0.5f, 0.5f);  // exact tie in one band

    constexpr std::array<ActivityDef, 2> tied{{
        {ActivityId::Explore, ActivityBand::Productive, score_b, act_b},
        {ActivityId::Roam, ActivityBand::Productive, score_c, act_c},
    }};
    constexpr std::array<ActivityDef, 2> tied_reversed{{
        {ActivityId::Roam, ActivityBand::Productive, score_c, act_c},
        {ActivityId::Explore, ActivityBand::Productive, score_b, act_b},
    }};

    // Same inputs -> same answer, every time.
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

    // Out-of-range ids are answerable, not UB: snapshot rows start at -1.
    CHECK(std::string(ActivityName(-1)) == "-");
    CHECK(std::string(ActivityName(kActivityCount)) == "-");
    CHECK(ActivityInfoOf(-1).id == ActivityId::Idle);
}

TEST_CASE("contract: only Idle occupies the Fallback band") {
    // Fallback is the guaranteed last resort. If anything else were allowed to
    // sit there it could shadow Idle and leave an entity with no decision.
    for (const ActivityInfo& info : ActivityCatalog()) {
        if (info.band == ActivityBand::Fallback) {
            INFO("activity " << info.name);
            CHECK(info.id == ActivityId::Idle);
        }
    }
}
