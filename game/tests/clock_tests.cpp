// Day/night clock: integer-millisecond sim time at a fixed 30 Hz (components.h),
// advanced deterministically by tick_world.

#include "sim_internal.hpp"
#include "components.h"
#include "game_state.h"
#include "town_brain.h"

#include <catch_amalgamated.hpp>

#include <entt/entt.hpp>

#include <vector>

using namespace badlands;

TEST_CASE("time helpers derive day/night from integer ms") {
    CHECK(day_count(0) == 0);
    CHECK(day_count(kMillisPerDay) == 1);
    CHECK(day_count(kMillisPerDay * 3 + 5) == 3);

    CHECK(time_of_day(0) == Catch::Approx(0.0f));
    CHECK(time_of_day(kMillisPerDay / 4) == Catch::Approx(0.25f));
    CHECK(time_of_day(kMillisPerDay + kMillisPerDay / 2) == Catch::Approx(0.5f));  // wraps

    CHECK(is_night(0.90f));         // past kNightStart (0.75)
    CHECK(is_night(0.10f));         // before kNightEnd (0.25)
    CHECK_FALSE(is_night(0.50f));   // midday
}

TEST_CASE("tick_world advances the clock by kMillisPerTick") {
    auto g_owned = make_world(nullptr);
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
    auto g_owned = make_world(nullptr);
    BadlandsGame* g = g_owned.get();
    Action place{ActionKind::PlaceBuilding, 0, -20.0f, 20.0f,
                     static_cast<int32_t>(BuildingKind::FreeCompanyQuarters), 0};
    uint32_t guild = static_cast<uint32_t>(dispatch_into(*g, place));
    Action recruit{ActionKind::RecruitHero, guild, 0.0f, 0.0f, 0, 0};
    uint32_t hid = static_cast<uint32_t>(dispatch_into(*g, recruit));
    REQUIRE(hid != UINT32_MAX);

    g->world_millis = 3 * badlands::kMillisPerDay + badlands::kMillisPerDay * 4 / 5;  // day 3, night

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
    auto g_owned = make_world(nullptr);
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
