// The deliberation ("thinking") contract and the threat-proximity API, both
// tested as pure functions of a synthetic WorldView.
//
// These pin the STATE MACHINE, not the pause lengths: when a character pauses,
// when it refuses to, and when a pause is cancelled. Retuning
// think_min/max_millis must not touch this file; replacing the implementation
// with a noiser one must keep every case here passing.

#include "behaviours/deliberation.h"
#include "behaviours/world_view.h"

#include "command.h"
#include "components.h"
#include "game_state.h"
#include "placement.h"
#include "sim_internal.hpp"

#include <catch_amalgamated.hpp>

#include <entt/entt.hpp>

using namespace badlands;

namespace {

// A hero mid-life: already doing something discretionary, nothing threatening.
WorldView thinking_candidate() {
    WorldView v;
    v.slot = 3;
    v.pos = {5.0f, 5.0f};
    v.now_millis = 10'000;
    v.current_activity = static_cast<int32_t>(ActivityId::Roam);  // Filler
    return v;
}

// Deliberation is on by default; these make the intent of each test explicit.
SimFactors with_thinking() {
    SimFactors f;
    f.hero.think_min_millis = 100;
    f.hero.think_max_millis = 800;
    return f;
}

}  // namespace

TEST_CASE("contract: changing between discretionary goals starts a pause") {
    const SimFactors f = with_thinking();
    const WorldView v = thinking_candidate();

    const ThinkDecision d = deliberate(ActivityId::Buy, v, f);  // Roam -> Buy
    CHECK(d.pause);
    CHECK(d.duration_millis >= f.hero.think_min_millis);
    CHECK(d.duration_millis <= f.hero.think_max_millis);
}

TEST_CASE("contract: an unchanged goal is not a decision to deliberate over") {
    const SimFactors f = with_thinking();
    const WorldView v = thinking_candidate();
    CHECK_FALSE(deliberate(ActivityId::Roam, v, f).pause);  // already roaming
}

TEST_CASE("contract: a threat in proximity forbids deliberation") {
    // The safety rule. Standing still to think while something is closing in is
    // exactly the behaviour this must never produce.
    const SimFactors f = with_thinking();
    WorldView v = thinking_candidate();
    add_threat(v, {6.0f, 5.0f}, 1.0f);

    CHECK_FALSE(deliberate(ActivityId::Buy, v, f).pause);
}

TEST_CASE("contract: a threat CANCELS a pause already in progress") {
    const SimFactors f = with_thinking();
    WorldView v = thinking_candidate();
    v.current_activity = static_cast<int32_t>(ActivityId::Think);
    v.think_until_millis = v.now_millis + 500;  // 500 ms still to run

    CHECK(deliberate(ActivityId::Buy, v, f).pause);  // undisturbed: keeps holding

    add_threat(v, {6.0f, 5.0f}, 1.0f);
    CHECK_FALSE(deliberate(ActivityId::Buy, v, f).pause);  // interrupted
}

TEST_CASE("contract: a Danger-band goal is never deliberated over") {
    const SimFactors f = with_thinking();
    WorldView v = thinking_candidate();
    v.think_until_millis = v.now_millis + 500;  // even mid-pause

    // Flee and Combat are the immediate-danger responses; they commit at once.
    CHECK_FALSE(deliberate(ActivityId::Flee, v, f).pause);
    CHECK_FALSE(deliberate(ActivityId::Combat, v, f).pause);
}

TEST_CASE("contract: a pause in progress continues without re-logging") {
    // duration 0 with pause=true means "still thinking" -- the caller must not
    // emit a second command, or an 800 ms pause would put 24 rows in the trace.
    const SimFactors f = with_thinking();
    WorldView v = thinking_candidate();
    v.current_activity = static_cast<int32_t>(ActivityId::Think);
    v.think_until_millis = v.now_millis + 500;

    const ThinkDecision d = deliberate(ActivityId::Buy, v, f);
    CHECK(d.pause);
    CHECK(d.duration_millis == 0);
}

TEST_CASE("contract: an expired pause commits, and does not pause again") {
    // Without this the character would deliberate about deliberating, forever.
    const SimFactors f = with_thinking();
    WorldView v = thinking_candidate();
    v.current_activity = static_cast<int32_t>(ActivityId::Think);
    v.think_until_millis = v.now_millis;  // just elapsed

    CHECK_FALSE(deliberate(ActivityId::Buy, v, f).pause);
}

TEST_CASE("contract: a character with no decision yet does not deliberate") {
    // Nothing to change its mind ABOUT. Also keeps a freshly spawned character
    // from standing around before it has ever acted.
    const SimFactors f = with_thinking();
    WorldView v = thinking_candidate();
    v.current_activity = -1;

    CHECK_FALSE(deliberate(ActivityId::Buy, v, f).pause);
}

TEST_CASE("contract: leaving or entering a non-goal does not pause") {
    const SimFactors f = with_thinking();
    WorldView v = thinking_candidate();

    // Coming off Idle: idleness is the absence of a goal, so picking one up is
    // not a change of mind worth a pause.
    v.current_activity = static_cast<int32_t>(ActivityId::Idle);
    CHECK_FALSE(deliberate(ActivityId::Buy, v, f).pause);

    // Going TO Idle: you do not stand and think about doing nothing.
    v.current_activity = static_cast<int32_t>(ActivityId::Roam);
    CHECK_FALSE(deliberate(ActivityId::Idle, v, f).pause);

    // Coming off a Danger response: the emergency just ended, get on with it.
    v.current_activity = static_cast<int32_t>(ActivityId::Flee);
    CHECK_FALSE(deliberate(ActivityId::Buy, v, f).pause);
}

TEST_CASE("contract: think_max_millis = 0 disables deliberation entirely") {
    // The documented off switch, relied on by timing-sensitive tests.
    SimFactors f;
    f.hero.think_min_millis = 0;
    f.hero.think_max_millis = 0;
    const WorldView v = thinking_candidate();

    CHECK_FALSE(deliberate(ActivityId::Buy, v, f).pause);
}

TEST_CASE("contract: pause length is deterministic and varies by actor and time") {
    const SimFactors f = with_thinking();
    WorldView v = thinking_candidate();

    // Same (slot, now) -> same draw, every time. This is what makes a run and
    // its replay pause identically.
    const int64_t a = deliberate(ActivityId::Buy, v, f).duration_millis;
    for (int i = 0; i < 8; ++i) {
        CHECK(deliberate(ActivityId::Buy, v, f).duration_millis == a);
    }

    // Different actors, and the same actor at different times, differ -- so a
    // group that decides together does not move as one.
    int distinct = 0;
    int64_t previous = a;
    for (uint32_t slot = 4; slot < 12; ++slot) {
        v.slot = slot;
        const int64_t d = deliberate(ActivityId::Buy, v, f).duration_millis;
        distinct += (d != previous) ? 1 : 0;
        previous = d;
    }
    CHECK(distinct > 0);
}

TEST_CASE("contract: is_discretionary means any Normal-band goal") {
    // A hero deliberates over which errand to run; it does not deliberate over
    // whether to flee. So everything Normal is discretionary and only the
    // immediate-danger responses are not.
    CHECK(is_discretionary(static_cast<int32_t>(ActivityId::Roam)));
    CHECK(is_discretionary(static_cast<int32_t>(ActivityId::Explore)));
    CHECK(is_discretionary(static_cast<int32_t>(ActivityId::GoHome)));
    CHECK(is_discretionary(static_cast<int32_t>(ActivityId::Idle)));
    CHECK_FALSE(is_discretionary(static_cast<int32_t>(ActivityId::Flee)));    // Danger
    CHECK_FALSE(is_discretionary(static_cast<int32_t>(ActivityId::Combat)));  // Danger
    CHECK_FALSE(is_discretionary(-1));                                        // undecided
    CHECK_FALSE(is_discretionary(kActivityCount));                            // out of range
}

// --- the threat-proximity API ----------------------------------------------

TEST_CASE("contract: threats are ordered nearest-first regardless of insertion order") {
    // Blocks read threats[0] as "the closest", so the ordering is contract, not
    // convenience -- and it must not depend on the order perception happened to
    // walk the world in.
    WorldView v;
    add_threat(v, {30.0f, 0.0f}, 30.0f, 2);
    add_threat(v, {10.0f, 0.0f}, 10.0f, 0);
    add_threat(v, {20.0f, 0.0f}, 20.0f, 1);

    REQUIRE(v.threat_count == 3);
    CHECK(v.threats[0].dist == 10.0f);
    CHECK(v.threats[1].dist == 20.0f);
    CHECK(v.threats[2].dist == 30.0f);
    CHECK(has_threat(v));
    CHECK(nearest_threat_dist(v) == 10.0f);
    CHECK(nearest_threat_pos(v).x == 10.0f);
    CHECK(v.threats[0].slot == 0);
}

TEST_CASE("contract: an empty threat list answers safely") {
    const WorldView v;
    CHECK_FALSE(has_threat(v));
    CHECK(nearest_threat_dist(v) == 0.0f);
    CHECK(nearest_threat_pos(v) == glm::vec2{0.0f, 0.0f});
}

TEST_CASE("contract: the threat list saturates without losing the nearest") {
    // Capped attention: past the cap the FAR ones are dropped, never the close
    // ones, so saturation degrades gracefully instead of blinding the entity.
    WorldView v;
    for (int i = WorldView::kMaxThreats * 2; i > 0; --i) {
        add_threat(v, {static_cast<float>(i), 0.0f}, static_cast<float>(i),
                   static_cast<uint32_t>(i));
    }
    CHECK(v.threat_count == WorldView::kMaxThreats);
    CHECK(nearest_threat_dist(v) == 1.0f);
    for (int32_t i = 1; i < v.threat_count; ++i) {
        CHECK(v.threats[i - 1].dist <= v.threats[i].dist);
    }
}

TEST_CASE("contract: equal-distance threats break ties by slot, not arrival") {
    WorldView a;
    add_threat(a, {1.0f, 0.0f}, 5.0f, 7);
    add_threat(a, {0.0f, 1.0f}, 5.0f, 2);

    WorldView b;
    add_threat(b, {0.0f, 1.0f}, 5.0f, 2);
    add_threat(b, {1.0f, 0.0f}, 5.0f, 7);

    CHECK(a.threats[0].slot == 2);
    CHECK(b.threats[0].slot == 2);
}

// --- end to end through the sim ---------------------------------------------
// The pure cases above pin the rules; this one proves they are actually wired
// into the tick loop, that the pause is recorded in the trace (so a replay can
// reproduce it), and that a thinking hero really does stand still.

TEST_CASE("a hero pauses in the sim, holds position, and resumes") {
    auto owned = make_world(nullptr);
    BadlandsGame& g = *owned;

    Action place_guild{ActionKind::PlaceBuilding, 0, -20.0f, 20.0f,
                       static_cast<int32_t>(BuildingKind::FreeCompanyQuarters), 0};
    const uint32_t guild = static_cast<uint32_t>(dispatch_into(g, place_guild));
    Action place_tavern{ActionKind::PlaceBuilding, 0, 20.0f, 20.0f,
                        static_cast<int32_t>(BuildingKind::Tavern), 0};
    REQUIRE(dispatch_into(g, place_tavern) >= 0);
    Action hire{ActionKind::RecruitHero, guild, 0.0f, 0.0f, 0, 0};
    const uint32_t slot = static_cast<uint32_t>(dispatch_into(g, hire));
    REQUIRE(slot != UINT32_MAX);
    const entt::entity e = g.slots[slot];

    // Away from every door, so no arrival handler fires and hides the hero.
    g.registry.get<Position>(e).pos = {0.0f, 0.0f};
    g.world_millis = kMillisPerDay / 2;  // midday: the tavern is an option

    // First decision: spent, so it heads home. Nothing to deliberate over yet
    // (the hero has not decided anything before). Reserves are 0..1 with 1
    // satisfied, so low fatigue = tired.
    {
        auto& sim = g.registry.get<HeroSimulationState>(e);
        sim.fatigue = 0.2f;  // below the seek bar -> wants rest
        sim.content = 1.0f;  // not bored
    }
    tick_world(g, 1.0f / 30.0f);
    REQUIRE(g.registry.get<HeroSimulationState>(e).behavior ==
            static_cast<int32_t>(ActivityId::GoHome));

    // Now rested but starved of diversion: it wants the tavern instead. That IS
    // a change of mind, so it should stop and think rather than pivot mid-stride.
    {
        auto& sim = g.registry.get<HeroSimulationState>(e);
        sim.fatigue = 1.0f;
        sim.content = 0.1f;
    }
    g.registry.get<Position>(e).pos = {0.0f, 0.0f};
    tick_world(g, 1.0f / 30.0f);

    const auto& sim = g.registry.get<HeroSimulationState>(e);
    REQUIRE(sim.behavior == static_cast<int32_t>(ActivityId::Think));
    CHECK(sim.think_until_millis > g.world_millis);

    // The pause is in the trace, with its length -- which is what lets a replay
    // reproduce it instead of re-drawing a different one.
    bool logged = false;
    for (const Command& c : g.command_log) {
        if (c.kind == CommandKind::SetBehavior && c.actor == slot &&
            c.param_a == static_cast<int32_t>(ActivityId::Think)) {
            CHECK(c.param_b > 0);
            logged = true;
        }
    }
    CHECK(logged);

    // While thinking it holds position, and does not re-log the decision.
    const glm::vec2 held = g.registry.get<Position>(e).pos;
    const size_t log_size = g.command_log.size();
    for (int i = 0; i < 5; ++i) {
        tick_world(g, 1.0f / 30.0f);
    }
    CHECK(g.registry.get<HeroSimulationState>(e).behavior ==
          static_cast<int32_t>(ActivityId::Think));
    CHECK(glm::distance(g.registry.get<Position>(e).pos, held) < 0.1f);
    CHECK(g.command_log.size() == log_size);  // a pause is one entry, not one per tick

    // Once it elapses, the hero commits to the goal it was mulling over.
    g.world_millis = sim.think_until_millis;
    {
        auto& s = g.registry.get<HeroSimulationState>(e);
        s.fatigue = 1.0f;
        s.content = 0.1f;
    }
    tick_world(g, 1.0f / 30.0f);
    CHECK(g.registry.get<HeroSimulationState>(e).behavior ==
          static_cast<int32_t>(ActivityId::VisitTavern));
}
