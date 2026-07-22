// Needs system (game/src/needs.h) + reset-on-enter satisfaction (heroes.cpp):
// the day/night loop's dynamic hero state. Mechanisms (rates) are policy; these
// tests pin the architecture — growth happens, is clamped, skips hidden heroes,
// and is reset by entering home/tavern.

#include "badlands_game.h"
#include "components.h"
#include "game_state.h"
#include "heroes.h"
#include "placement.h"
#include "town_brain.h"

#include <catch_amalgamated.hpp>

#include <entt/entt.hpp>

using namespace badlands;

namespace {

uint32_t place(BadlandsGame* g, int kind, float x, float z) {
    GameAction a{GAME_ACTION_PLACE_BUILDING, 0, x, z, kind, 0};
    return static_cast<uint32_t>(game_dispatch(g, &a));
}
uint32_t recruit_at(BadlandsGame* g, uint32_t bid) {
    GameAction a{GAME_ACTION_RECRUIT_HERO, bid, 0.0f, 0.0f, 0, 0};
    return static_cast<uint32_t>(game_dispatch(g, &a));
}

}  // namespace

TEST_CASE("advance_needs raises fatigue/boredom each tick and clamps to 1") {
    BadlandsGame* g = game_create(nullptr);
    GameCharacterDesc d = game_desc_mercenary(0.0f, 0.0f);
    uint32_t slot = game_spawn(g, &d);
    entt::entity e = g->slots[slot];

    CHECK(g->registry.get<HeroSimulationState>(e).fatigue == 0.0f);
    game_tick(g, 1.0f / 30.0f);
    CHECK(g->registry.get<HeroSimulationState>(e).fatigue == Catch::Approx(kFatiguePerTick));
    CHECK(g->registry.get<HeroSimulationState>(e).boredom == Catch::Approx(kBoredomPerTick));

    g->registry.get<HeroSimulationState>(e).fatigue = 0.999f;
    for (int i = 0; i < 200; ++i) {
        game_tick(g, 1.0f / 30.0f);
    }
    CHECK(g->registry.get<HeroSimulationState>(e).fatigue <= 1.0f);
    CHECK(g->registry.get<HeroSimulationState>(e).boredom <= 1.0f);

    game_destroy(g);
}

TEST_CASE("hidden heroes do not accrue needs") {
    BadlandsGame* g = game_create(nullptr);
    GameCharacterDesc d = game_desc_mercenary(0.0f, 0.0f);
    uint32_t slot = game_spawn(g, &d);
    entt::entity e = g->slots[slot];
    g->registry.emplace<InsideBuilding>(e, 0, 999.0f);  // hidden long enough

    game_tick(g, 1.0f / 30.0f);
    CHECK(g->registry.get<HeroSimulationState>(e).fatigue == 0.0f);
    CHECK(g->registry.get<HeroSimulationState>(e).boredom == 0.0f);

    game_destroy(g);
}

TEST_CASE("entering home resets fatigue") {
    BadlandsGame* g = game_create(nullptr);
    uint32_t guild = place(g, GAME_BUILDING_FREE_COMPANY_QUARTERS, -20.0f, 20.0f);
    uint32_t hid = recruit_at(g, guild);
    REQUIRE(hid != UINT32_MAX);
    entt::entity e = g->slots[hid];

    glm::vec2 tile;
    REQUIRE(building_approach_tile(g->placement, g->placement.buildings[guild], tile));
    g->registry.get<Position>(e).pos = tile;
    g->registry.get<HeroSimulationState>(e).fatigue = 0.8f;

    REQUIRE(hero_enter_home(*g, e));
    CHECK(g->registry.get<HeroSimulationState>(e).fatigue == 0.0f);

    game_destroy(g);
}

TEST_CASE("entering the tavern resets boredom") {
    BadlandsGame* g = game_create(nullptr);
    uint32_t guild = place(g, GAME_BUILDING_FREE_COMPANY_QUARTERS, -20.0f, 20.0f);
    uint32_t tavern = place(g, GAME_BUILDING_TAVERN, 20.0f, 20.0f);
    uint32_t hid = recruit_at(g, guild);
    REQUIRE(hid != UINT32_MAX);
    entt::entity e = g->slots[hid];

    glm::vec2 tile;
    REQUIRE(building_approach_tile(g->placement, g->placement.buildings[tavern], tile));
    g->registry.get<Position>(e).pos = tile;
    g->registry.get<HeroSimulationState>(e).boredom = 0.7f;

    REQUIRE(hero_enter(*g, e, GAME_BUILDING_TAVERN));
    CHECK(g->registry.get<HeroSimulationState>(e).boredom == 0.0f);

    game_destroy(g);
}

TEST_CASE("C++ town brain sends a tired hero toward home") {
    BadlandsGame* g = game_create(nullptr);  // C++ brain (no noiser)
    uint32_t guild = place(g, GAME_BUILDING_FREE_COMPANY_QUARTERS, -20.0f, 20.0f);
    uint32_t hid = recruit_at(g, guild);
    REQUIRE(hid != UINT32_MAX);
    entt::entity e = g->slots[hid];

    glm::vec2 home_door;
    REQUIRE(building_approach_tile(g->placement, g->placement.buildings[guild], home_door));

    g->registry.get<Position>(e).pos = {40.0f, 40.0f};  // far from home
    g->registry.get<HeroSimulationState>(e).fatigue = 0.9f;

    game_tick(g, 1.0f / 30.0f);

    CHECK(g->registry.get<HeroSimulationState>(e).behavior == static_cast<int32_t>(Behavior::GoHome));
    const MoveTarget& mt = g->registry.get<MoveTarget>(e);
    CHECK(mt.kind == MoveTarget::Kind::Point);
    CHECK(mt.point.x == Catch::Approx(home_door.x));
    CHECK(mt.point.y == Catch::Approx(home_door.y));

    game_destroy(g);
}

TEST_CASE("C++ town brain sends a bored hero to the tavern by day") {
    BadlandsGame* g = game_create(nullptr);
    uint32_t guild = place(g, GAME_BUILDING_FREE_COMPANY_QUARTERS, -20.0f, 20.0f);
    uint32_t tavern = place(g, GAME_BUILDING_TAVERN, 20.0f, 20.0f);
    uint32_t hid = recruit_at(g, guild);
    REQUIRE(hid != UINT32_MAX);
    entt::entity e = g->slots[hid];

    glm::vec2 tavern_door;
    REQUIRE(building_approach_tile(g->placement, g->placement.buildings[tavern], tavern_door));

    g->registry.get<Position>(e).pos = {0.0f, 0.0f};
    g->world_millis = kMillisPerDay / 2;  // midday (not night)
    auto& sim = g->registry.get<HeroSimulationState>(e);
    sim.fatigue = 0.0f;
    sim.boredom = 0.9f;

    game_tick(g, 1.0f / 30.0f);

    CHECK(g->registry.get<HeroSimulationState>(e).behavior ==
          static_cast<int32_t>(Behavior::VisitTavern));
    const MoveTarget& mt = g->registry.get<MoveTarget>(e);
    CHECK(mt.point.x == Catch::Approx(tavern_door.x));
    CHECK(mt.point.y == Catch::Approx(tavern_door.y));

    game_destroy(g);
}
