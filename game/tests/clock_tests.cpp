// Day/night clock: integer-millisecond sim time at a fixed 30 Hz (components.h),
// advanced deterministically by tick_world.

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
    constexpr int64_t kDay = kDefaultMillisPerDay;
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
    // The whole point of the period being a parameter: the SAME world_millis
    // reads as a different time of day under a different day length.
    constexpr int64_t kShort = 60'000;   // half the default
    constexpr int64_t kLong = 240'000;   // twice the default

    CHECK(day_count(120'000, kShort) == 2);
    CHECK(day_count(120'000, kLong) == 0);

    CHECK(time_of_day(30'000, kShort) == Catch::Approx(0.5f));
    CHECK(time_of_day(30'000, kLong) == Catch::Approx(0.125f));

    // An in-game hour is day/24, so hours-authored rates scale with the day.
    CHECK(millis_per_hour(kShort) == 2'500);
    CHECK(millis_per_hour(kLong) == 10'000);
    CHECK(reserve_rate_per_tick(1.0f, kLong) ==
          Catch::Approx(reserve_rate_per_tick(1.0f, kShort) * 0.25f));
    // Non-positive hours still mean "the whole reserve in one tick".
    CHECK(reserve_rate_per_tick(0.0f, kLong) == Catch::Approx(1.0f));
}

TEST_CASE("MillisPerDayForSimSeconds converts through ticks, not seconds*1000") {
    // A tick is 33 ms at 30 Hz, so a sim-second is 990 ms of world time. The
    // seconds*1000 answer (180'000) would leave the sim ~1% fast against the sky.
    CHECK(MillisPerDayForSimSeconds(180.0f) == 178'200);
    CHECK(MillisPerDayForSimSeconds(120.0f) == 118'800);
    // Garbage in -> the default, not a division by zero downstream.
    CHECK(MillisPerDayForSimSeconds(0.0f) == kDefaultMillisPerDay);
    CHECK(MillisPerDayForSimSeconds(-5.0f) == kDefaultMillisPerDay);
}

TEST_CASE("WorldConfig::millis_per_day reaches the world and rescales need drain") {
    // The seam that would otherwise silently no-op: config -> world -> the
    // hours-authored need rates. A doubled day must drain exactly half as fast.
    auto seed = [](int64_t millis_per_day) {
        WorldConfig cfg{};
        cfg.millis_per_day = millis_per_day;
        std::unique_ptr<BadlandsGame> g = make_world(BrainDesc{}, cfg);
        Action place{ActionKind::PlaceBuilding, 0, -20.0f, 20.0f,
                     static_cast<int32_t>(BuildingKind::FreeCompanyQuarters), 0};
        const uint32_t guild = static_cast<uint32_t>(dispatch_into(*g, place));
        Action recruit{ActionKind::RecruitHero, guild, 0.0f, 0.0f, 0, 0};
        const uint32_t hid = static_cast<uint32_t>(dispatch_into(*g, recruit));
        REQUIRE(hid != UINT32_MAX);
        return std::pair{std::move(g), hid};
    };

    auto [normal, normal_hero] = seed(kDefaultMillisPerDay);
    auto [slow, slow_hero] = seed(kDefaultMillisPerDay * 2);
    CHECK(normal->millis_per_day == kDefaultMillisPerDay);
    CHECK(slow->millis_per_day == kDefaultMillisPerDay * 2);

    constexpr int kTicks = 300;
    for (int i = 0; i < kTicks; ++i) {
        tick_world(*normal, 1.0f / 30.0f);
        tick_world(*slow, 1.0f / 30.0f);
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
    cfg.millis_per_day = 0;
    auto g = make_world(BrainDesc{}, cfg);
    CHECK(g->millis_per_day == kDefaultMillisPerDay);
}

TEST_CASE("tick_world advances the clock by kMillisPerTick") {
    auto g_owned = make_world(BrainDesc{});
    BadlandsGame* g = g_owned.get();
    CHECK(g->world_millis == 0);

    tick_world(*g, 1.0f / 30.0f);
    CHECK(g->world_millis == kMillisPerTick);

    for (int i = 0; i < 9; ++i) {
        tick_world(*g, 1.0f / 30.0f);
    }
    CHECK(g->world_millis == kMillisPerTick * 10);

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

    g->world_millis = 3 * g->millis_per_day + g->millis_per_day * 4 / 5;  // day 3, night

    const WorldState w = world_of(*g);
    CHECK(w.day == 3);
    CHECK(w.time_of_day == Catch::Approx(0.8f));
    CHECK(w.is_night == 1);
    CHECK(w.world_millis == g->world_millis);

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
    CHECK(log[0].at_millis == 0);

    }
