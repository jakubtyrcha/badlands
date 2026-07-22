// Needs system (game/src/needs.h) + reset-on-enter satisfaction (heroes.cpp):
// the day/night loop's dynamic hero state. Mechanisms (rates) are policy; these
// tests pin the architecture — growth happens, is clamped, skips hidden heroes,
// and is reset by entering home/tavern.

#include "sim_internal.hpp"
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
    Action a{ActionKind::PlaceBuilding, 0, x, z, kind, 0};
    return static_cast<uint32_t>(dispatch_into(*g, a));
}
uint32_t recruit_at(BadlandsGame* g, uint32_t bid) {
    Action a{ActionKind::RecruitHero, bid, 0.0f, 0.0f, 0, 0};
    return static_cast<uint32_t>(dispatch_into(*g, a));
}

}  // namespace

TEST_CASE("advance_needs raises fatigue/boredom each tick and clamps to 1") {
    auto g_owned = make_world(nullptr);
    BadlandsGame* g = g_owned.get();
    CharacterDesc d = MercenaryDesc(0.0f, 0.0f);
    uint32_t slot = spawn_into(*g, d);
    entt::entity e = g->slots[slot];

    CHECK(g->registry.get<HeroSimulationState>(e).fatigue == 0.0f);
    tick_world(*g, 1.0f / 30.0f);
    CHECK(g->registry.get<HeroSimulationState>(e).fatigue == Catch::Approx(kFatiguePerTick));
    CHECK(g->registry.get<HeroSimulationState>(e).boredom == Catch::Approx(kBoredomPerTick));

    g->registry.get<HeroSimulationState>(e).fatigue = 0.999f;
    for (int i = 0; i < 200; ++i) {
        tick_world(*g, 1.0f / 30.0f);
    }
    CHECK(g->registry.get<HeroSimulationState>(e).fatigue <= 1.0f);
    CHECK(g->registry.get<HeroSimulationState>(e).boredom <= 1.0f);

    }

TEST_CASE("hidden heroes do not accrue needs") {
    auto g_owned = make_world(nullptr);
    BadlandsGame* g = g_owned.get();
    CharacterDesc d = MercenaryDesc(0.0f, 0.0f);
    uint32_t slot = spawn_into(*g, d);
    entt::entity e = g->slots[slot];
    g->registry.emplace<InsideBuilding>(e, 0, 999.0f);  // hidden long enough

    tick_world(*g, 1.0f / 30.0f);
    CHECK(g->registry.get<HeroSimulationState>(e).fatigue == 0.0f);
    CHECK(g->registry.get<HeroSimulationState>(e).boredom == 0.0f);

    }

TEST_CASE("entering home resets fatigue") {
    auto g_owned = make_world(nullptr);
    BadlandsGame* g = g_owned.get();
    uint32_t guild = place(g, static_cast<int32_t>(BuildingKind::FreeCompanyQuarters), -20.0f, 20.0f);
    uint32_t hid = recruit_at(g, guild);
    REQUIRE(hid != UINT32_MAX);
    entt::entity e = g->slots[hid];

    glm::vec2 tile;
    REQUIRE(building_approach_tile(g->placement, g->placement.buildings[guild], tile));
    g->registry.get<Position>(e).pos = tile;
    g->registry.get<HeroSimulationState>(e).fatigue = 0.8f;

    REQUIRE(hero_enter_home(*g, e));
    CHECK(g->registry.get<HeroSimulationState>(e).fatigue == 0.0f);

    }

TEST_CASE("entering the tavern resets boredom") {
    auto g_owned = make_world(nullptr);
    BadlandsGame* g = g_owned.get();
    uint32_t guild = place(g, static_cast<int32_t>(BuildingKind::FreeCompanyQuarters), -20.0f, 20.0f);
    uint32_t tavern = place(g, static_cast<int32_t>(BuildingKind::Tavern), 20.0f, 20.0f);
    uint32_t hid = recruit_at(g, guild);
    REQUIRE(hid != UINT32_MAX);
    entt::entity e = g->slots[hid];

    glm::vec2 tile;
    REQUIRE(building_approach_tile(g->placement, g->placement.buildings[tavern], tile));
    g->registry.get<Position>(e).pos = tile;
    g->registry.get<HeroSimulationState>(e).boredom = 0.7f;

    REQUIRE(hero_enter(*g, e, static_cast<int32_t>(BuildingKind::Tavern)));
    CHECK(g->registry.get<HeroSimulationState>(e).boredom == 0.0f);

    }

TEST_CASE("C++ town brain sends a tired hero toward home") {
    auto g_owned = make_world(nullptr);
    BadlandsGame* g = g_owned.get();  // C++ brain (no noiser)
    uint32_t guild = place(g, static_cast<int32_t>(BuildingKind::FreeCompanyQuarters), -20.0f, 20.0f);
    uint32_t hid = recruit_at(g, guild);
    REQUIRE(hid != UINT32_MAX);
    entt::entity e = g->slots[hid];

    glm::vec2 home_door;
    REQUIRE(building_approach_tile(g->placement, g->placement.buildings[guild], home_door));

    g->registry.get<Position>(e).pos = {40.0f, 40.0f};  // far from home
    g->registry.get<HeroSimulationState>(e).fatigue = 0.9f;

    tick_world(*g, 1.0f / 30.0f);

    CHECK(g->registry.get<HeroSimulationState>(e).behavior == static_cast<int32_t>(Behavior::GoHome));
    const MoveTarget& mt = g->registry.get<MoveTarget>(e);
    CHECK(mt.kind == MoveTarget::Kind::Point);
    CHECK(mt.point.x == Catch::Approx(home_door.x));
    CHECK(mt.point.y == Catch::Approx(home_door.y));

    }

TEST_CASE("C++ town brain sends a bored hero to the tavern by day") {
    auto g_owned = make_world(nullptr);
    BadlandsGame* g = g_owned.get();
    uint32_t guild = place(g, static_cast<int32_t>(BuildingKind::FreeCompanyQuarters), -20.0f, 20.0f);
    uint32_t tavern = place(g, static_cast<int32_t>(BuildingKind::Tavern), 20.0f, 20.0f);
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

    tick_world(*g, 1.0f / 30.0f);

    CHECK(g->registry.get<HeroSimulationState>(e).behavior ==
          static_cast<int32_t>(Behavior::VisitTavern));
    const MoveTarget& mt = g->registry.get<MoveTarget>(e);
    CHECK(mt.point.x == Catch::Approx(tavern_door.x));
    CHECK(mt.point.y == Catch::Approx(tavern_door.y));

    }

TEST_CASE("hero thresholds come from SimFactors, not hardcoded constants") {
    // Policy is data: the same world state must produce a different decision
    // when the factors change. This is what makes assets/creatures/factors.json
    // meaningful -- if the brain ignored factors the file would be decoration.
    auto owned = make_world(nullptr);
    BadlandsGame& g = *owned;

    // Defaults are shipped and sane with no file loaded.
    CHECK(g.factors.hero.fatigue_go_home == Catch::Approx(0.6f));

    Action place{ActionKind::PlaceBuilding, 0, -20.0f, 20.0f,
                 static_cast<int32_t>(BuildingKind::FreeCompanyQuarters), 0};
    uint32_t guild = static_cast<uint32_t>(dispatch_into(g, place));
    Action recruit{ActionKind::RecruitHero, guild, 0.0f, 0.0f, 0, 0};
    uint32_t hid = static_cast<uint32_t>(dispatch_into(g, recruit));
    REQUIRE(hid != UINT32_MAX);
    entt::entity e = g.slots[hid];

    glm::vec2 home_door;
    REQUIRE(building_approach_tile(g.placement, g.placement.buildings[guild], home_door));
    g.registry.get<Position>(e).pos = {40.0f, 40.0f};
    // Midday: at t=0 it is NIGHT (tod < kNightEnd), where the lower
    // fatigue_night bar applies and would mask the daytime threshold.
    g.world_millis = kMillisPerDay / 2;

    // Fatigue 0.3 is BELOW the default 0.6 -> the hero must not head home.
    g.registry.get<HeroSimulationState>(e).fatigue = 0.3f;
    tick_world(g, 1.0f / 30.0f);
    CHECK(g.registry.get<HeroSimulationState>(e).behavior !=
          static_cast<int32_t>(Behavior::GoHome));

    // Same fatigue, lower threshold from data -> now it does.
    SimFactors f = g.factors;
    f.hero.fatigue_go_home = 0.2f;
    set_factors_of(g, f);
    tick_world(g, 1.0f / 30.0f);
    CHECK(g.registry.get<HeroSimulationState>(e).behavior ==
          static_cast<int32_t>(Behavior::GoHome));
}
