// Day/night clock: integer-millisecond sim time at a fixed 30 Hz (components.h),
// advanced deterministically by step_world.

#include "sim_internal.hpp"
#include "behaviours/world_view.h"
#include "components.h"
#include "game_state.h"

#include <catch_amalgamated.hpp>

#include <entt/entt.hpp>

#include <memory>
#include <utility>
#include <vector>

using namespace badlands;

TEST_CASE("time helpers derive day/night from integer ms") {
    constexpr int64_t kDay = kDefaultTicksPerDay;
    CHECK(day_count(0, kDay) == 0);
    CHECK(day_count(kDay, kDay) == 1);
    CHECK(day_count(kDay * 3 + 5, kDay) == 3);

    CHECK(time_of_day(0, kDay) == Catch::Approx(0.0f));
    CHECK(time_of_day(kDay / 4, kDay) == Catch::Approx(0.25f));
    CHECK(time_of_day(kDay + kDay / 2, kDay) == Catch::Approx(0.5f));  // wraps

    CHECK(is_night(0.90f));         // past kNightStart (0.75)
    CHECK(is_night(0.10f));         // before kNightEnd (0.25)
    CHECK_FALSE(is_night(0.50f));   // midday
}

TEST_CASE("time helpers honour a non-default day length") {
    // The whole point of the period being a parameter: the SAME world_ticks
    // reads as a different time of day under a different day length.
    constexpr int64_t kShort = 60'000;   // half the default
    constexpr int64_t kLong = 240'000;   // twice the default

    CHECK(day_count(120'000, kShort) == 2);
    CHECK(day_count(120'000, kLong) == 0);

    CHECK(time_of_day(30'000, kShort) == Catch::Approx(0.5f));
    CHECK(time_of_day(30'000, kLong) == Catch::Approx(0.125f));

    // An in-game hour is day/24, so hours-authored rates scale with the day.
    CHECK(ticks_per_hour(kShort) == 2'500);
    CHECK(ticks_per_hour(kLong) == 10'000);
    CHECK(reserve_rate_per_step(1.0f, kLong) ==
          Catch::Approx(reserve_rate_per_step(1.0f, kShort) * 0.25f));
    // Non-positive hours still mean "the whole reserve in one tick".
    CHECK(reserve_rate_per_step(0.0f, kLong) == Catch::Approx(1.0f));
}

TEST_CASE("TicksPerDayForSimSeconds is exact") {
    // The whole point of counting in ticks: a sim-second IS kTicksPerSecond
    // ticks, so this is a multiplication and not an approximation. The
    // millisecond version of this helper existed only to correct a 1% drift
    // that a 33 ms tick made unavoidable -- there is nothing left to correct.
    CHECK(TicksPerDayForSimSeconds(180.0f) == 180 * badlands::kTicksPerSecond);
    CHECK(TicksPerDayForSimSeconds(120.0f) == 120 * badlands::kTicksPerSecond);
    // Garbage in -> the default, not a division by zero downstream.
    CHECK(TicksPerDayForSimSeconds(0.0f) == kDefaultTicksPerDay);
    CHECK(TicksPerDayForSimSeconds(-5.0f) == kDefaultTicksPerDay);
}

TEST_CASE("WorldConfig::ticks_per_day reaches the world and rescales need drain") {
    // The seam that would otherwise silently no-op: config -> world -> the
    // hours-authored need rates. A doubled day must drain exactly half as fast.
    auto seed = [](int64_t ticks_per_day) {
        WorldConfig cfg{};
        cfg.ticks_per_day = ticks_per_day;
        std::unique_ptr<BadlandsGame> g = make_world(BrainDesc{}, cfg);
        Action place{ActionKind::PlaceBuilding, 0, -20.0f, 20.0f,
                     static_cast<int32_t>(BuildingKind::FreeCompanyQuarters), 0};
        const uint32_t guild = static_cast<uint32_t>(dispatch_into(*g, place));
        Action recruit{ActionKind::RecruitHero, guild, 0.0f, 0.0f, 0, 0};
        const uint32_t hid = static_cast<uint32_t>(dispatch_into(*g, recruit));
        REQUIRE(hid != UINT32_MAX);
        return std::pair{std::move(g), hid};
    };

    auto [normal, normal_hero] = seed(kDefaultTicksPerDay);
    auto [slow, slow_hero] = seed(kDefaultTicksPerDay * 2);
    CHECK(normal->ticks_per_day == kDefaultTicksPerDay);
    CHECK(slow->ticks_per_day == kDefaultTicksPerDay * 2);

    constexpr int kTicks = 300;
    for (int i = 0; i < kTicks; ++i) {
        step_world(*normal);
        step_world(*slow);
    }

    const float normal_drop =
        1.0f - normal->registry.get<HeroSimulationState>(normal->slots[normal_hero]).fatigue;
    const float slow_drop =
        1.0f - slow->registry.get<HeroSimulationState>(slow->slots[slow_hero]).fatigue;
    REQUIRE(normal_drop > 0.0f);  // it drained at all, so the ratio below means something
    CHECK(slow_drop == Catch::Approx(normal_drop * 0.5f).epsilon(0.001));
}

TEST_CASE("an invalid day length falls back to the default instead of dividing by zero") {
    WorldConfig cfg{};
    cfg.ticks_per_day = 0;
    auto g = make_world(BrainDesc{}, cfg);
    CHECK(g->ticks_per_day == kDefaultTicksPerDay);
}

TEST_CASE("step_world advances the clock by kTicksPerStep") {
    auto g_owned = make_world(BrainDesc{});
    BadlandsGame* g = g_owned.get();
    CHECK(g->world_ticks == 0);

    step_world(*g);
    CHECK(g->world_ticks == kTicksPerStep);

    for (int i = 0; i < 9; ++i) {
        step_world(*g);
    }
    CHECK(g->world_ticks == kTicksPerStep * 10);

    }

TEST_CASE("snapshots expose the clock and per-hero needs (observation ABI)") {
    // Inspection is part of the architecture: everything a debug panel needs is
    // readable through the snapshot API, never by reaching into the registry.
    auto g_owned = make_world(BrainDesc{});
    BadlandsGame* g = g_owned.get();
    Action place{ActionKind::PlaceBuilding, 0, -20.0f, 20.0f,
                     static_cast<int32_t>(BuildingKind::FreeCompanyQuarters), 0};
    uint32_t guild = static_cast<uint32_t>(dispatch_into(*g, place));
    Action recruit{ActionKind::RecruitHero, guild, 0.0f, 0.0f, 0, 0};
    uint32_t hid = static_cast<uint32_t>(dispatch_into(*g, recruit));
    REQUIRE(hid != UINT32_MAX);

    g->world_ticks = 3 * g->ticks_per_day + g->ticks_per_day * 4 / 5;  // day 3, night

    const WorldState w = world_of(*g);
    CHECK(w.day == 3);
    CHECK(w.time_of_day == Catch::Approx(0.8f));
    CHECK(w.is_night == 1);
    CHECK(w.world_ticks == g->world_ticks);

    g->registry.get<badlands::HeroSimulationState>(g->slots[hid]).fatigue = 0.4f;
    g->registry.get<badlands::HeroSimulationState>(g->slots[hid]).content = 0.6f;
    g->registry.get<badlands::HeroSimulationState>(g->slots[hid]).behavior =
        static_cast<int32_t>(badlands::Behavior::VisitTavern);

    const std::vector<CharacterState> rows = characters_of(*g);
    REQUIRE(rows.size() >= 1);
    CHECK(rows[0].fatigue == Catch::Approx(0.4f));
    CHECK(rows[0].content == Catch::Approx(0.6f));
    CHECK(rows[0].behavior == static_cast<int32_t>(badlands::Behavior::VisitTavern));
    CHECK(rows[0].name[0] != '\0');  // heroes are named for the panel

    }

TEST_CASE("command_log_of exposes the applied trace") {
    // The command log IS the trace of record: everything that mutated the sim,
    // in apply order, readable through the snapshot API.
    auto g_owned = make_world(BrainDesc{});
    BadlandsGame* g = g_owned.get();
    Action place{ActionKind::PlaceBuilding, 0, -20.0f, 20.0f,
                     static_cast<int32_t>(BuildingKind::FreeCompanyQuarters), 0};
    uint32_t guild = static_cast<uint32_t>(dispatch_into(*g, place));
    Action recruit{ActionKind::RecruitHero, guild, 0.0f, 0.0f, 0, 0};
    dispatch_into(*g, recruit);

    const std::vector<CommandRecord> log = command_log_of(*g);
    REQUIRE(log.size() == 2);
    CHECK(log[0].kind == CommandKindId::PlaceBuilding);
    CHECK(log[1].kind == CommandKindId::RecruitHero);
    // Stamped with the sim time each took effect (pre-tick dispatches -> 0).
    CHECK(log[0].at_ticks == 0);

    }

TEST_CASE("a second is exactly kTicksPerSecond ticks, and a step divides it") {
    // THE invariant this whole unit change exists for, and the one the
    // millisecond clock could not state: 1000 does not divide by 30, so a
    // 33 ms tick made thirty steps 990 ms against a SimClock second of 1000 --
    // 1% slow, permanently and cumulatively, which is why the day-length helper
    // above needed a correction factor at all.
    CHECK(badlands::kTicksPerSecond % badlands::kStepsPerSecond == 0);
    CHECK(badlands::kStepsPerSecond * badlands::kTicksPerStep ==
          badlands::kTicksPerSecond);

    // ...and the world clock agrees with it, measured rather than asserted:
    // exactly kStepsPerSecond steps advance world_ticks by exactly one second.
    auto owned = badlands::make_flat_world();
    BadlandsGame& g = *owned;
    const int64_t before = g.world_ticks;
    for (int64_t i = 0; i < badlands::kStepsPerSecond; ++i) {
        badlands::step_world(g);
    }
    CHECK(g.world_ticks - before == badlands::kTicksPerSecond);

    // The authoring conversion round-trips through whole seconds.
    CHECK(badlands::ticks_of(3.0f) == 3 * badlands::kTicksPerSecond);
    CHECK(badlands::seconds_of_ticks(badlands::ticks_of(2.5f)) == Catch::Approx(2.5f));
    // A positive duration never rounds away to nothing, which downstream would
    // read as "no duration at all".
    CHECK(badlands::ticks_of(0.001f) > 0);
}
