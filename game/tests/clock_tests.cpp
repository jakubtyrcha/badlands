// Day/night clock: integer-millisecond sim time at a fixed 30 Hz (components.h),
// advanced deterministically by game_tick.

#include "badlands_game.h"
#include "components.h"
#include "game_state.h"
#include "town_brain.h"

#include <catch_amalgamated.hpp>

#include <entt/entt.hpp>

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

TEST_CASE("game_tick advances the clock by kMillisPerTick") {
    BadlandsGame* g = game_create(nullptr);
    CHECK(g->world_millis == 0);

    game_tick(g, 1.0f / 30.0f);
    CHECK(g->world_millis == kMillisPerTick);

    for (int i = 0; i < 9; ++i) {
        game_tick(g, 1.0f / 30.0f);
    }
    CHECK(g->world_millis == kMillisPerTick * 10);

    game_destroy(g);
}

TEST_CASE("snapshots expose the clock and per-hero needs (observation ABI)") {
    // Inspection is part of the architecture: everything a debug panel needs is
    // readable through the data-only C ABI, never by reaching into the registry.
    BadlandsGame* g = game_create(nullptr);
    GameAction place{GAME_ACTION_PLACE_BUILDING, 0, -20.0f, 20.0f,
                     GAME_BUILDING_FREE_COMPANY_QUARTERS, 0};
    uint32_t guild = static_cast<uint32_t>(game_dispatch(g, &place));
    GameAction recruit{GAME_ACTION_RECRUIT_HERO, guild, 0.0f, 0.0f, 0, 0};
    uint32_t hid = static_cast<uint32_t>(game_dispatch(g, &recruit));
    REQUIRE(hid != UINT32_MAX);

    g->world_millis = 3 * badlands::kMillisPerDay + badlands::kMillisPerDay * 4 / 5;  // day 3, night

    GameWorldState w{};
    game_world(g, &w);
    CHECK(w.day == 3);
    CHECK(w.time_of_day == Catch::Approx(0.8f));
    CHECK(w.is_night == 1);
    CHECK(w.world_millis == g->world_millis);

    g->registry.get<badlands::HeroSimulationState>(g->slots[hid]).fatigue = 0.4f;
    g->registry.get<badlands::HeroSimulationState>(g->slots[hid]).boredom = 0.6f;
    g->registry.get<badlands::HeroSimulationState>(g->slots[hid]).behavior =
        static_cast<int32_t>(badlands::Behavior::VisitTavern);

    GameCharacterState rows[8]{};
    uint32_t n = game_state(g, rows, 8);
    REQUIRE(n >= 1);
    CHECK(rows[0].fatigue == Catch::Approx(0.4f));
    CHECK(rows[0].boredom == Catch::Approx(0.6f));
    CHECK(rows[0].behavior == static_cast<int32_t>(badlands::Behavior::VisitTavern));
    CHECK(rows[0].name[0] != '\0');  // heroes are named for the panel

    game_destroy(g);
}

TEST_CASE("game_command_log exposes the applied trace") {
    // The command log IS the trace of record: everything that mutated the sim,
    // in apply order, readable through the ABI (snapshot truncation idiom).
    BadlandsGame* g = game_create(nullptr);
    GameAction place{GAME_ACTION_PLACE_BUILDING, 0, -20.0f, 20.0f,
                     GAME_BUILDING_FREE_COMPANY_QUARTERS, 0};
    uint32_t guild = static_cast<uint32_t>(game_dispatch(g, &place));
    GameAction recruit{GAME_ACTION_RECRUIT_HERO, guild, 0.0f, 0.0f, 0, 0};
    game_dispatch(g, &recruit);

    GameCommandRecord rows[16]{};
    uint32_t total = game_command_log(g, rows, 16);
    REQUIRE(total == 2);
    CHECK(rows[0].kind == GAME_COMMAND_PLACE_BUILDING);
    CHECK(rows[1].kind == GAME_COMMAND_RECRUIT_HERO);

    // Truncation returns the TOTAL, and yields the most recent rows.
    uint32_t total2 = game_command_log(g, rows, 1);
    CHECK(total2 == 2);
    CHECK(rows[0].kind == GAME_COMMAND_RECRUIT_HERO);

    game_destroy(g);
}
