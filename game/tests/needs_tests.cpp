// Needs as RESERVES: [0,1] where 1 is satisfied, draining on their own and
// refilling by doing something about them. These pin the mechanism -- drain,
// clamp, fill-over-time, and the fact that a hero LEAVES a building when the
// reserve it went in for is full rather than after a fixed timer. Rates are
// policy (HeroFactors); the architecture is what this fixes in place.

#include "sim_internal.hpp"
#include "components.h"
#include "game_state.h"
#include "heroes.h"
#include "needs.h"
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

TEST_CASE("reserves drain each tick and clamp at 0") {
    auto g_owned = make_world(BrainDesc{});
    BadlandsGame* g = g_owned.get();
    CharacterDesc d = MercenaryDesc(0.0f, 0.0f);
    uint32_t slot = spawn_into(*g, d);
    entt::entity e = g->slots[slot];

    // A fresh hero starts satisfied (1), not spent (0).
    auto& sim = g->registry.get<HeroSimulationState>(e);
    CHECK(sim.fatigue == 1.0f);
    CHECK(sim.content == 1.0f);

    const float before_f = sim.fatigue;
    const float before_c = sim.content;
    advance_needs(*g);
    CHECK(sim.fatigue < before_f);  // drains, not rises
    CHECK(sim.content < before_c);

    // Run it flat and it settles at 0, never below.
    for (int i = 0; i < 100000; ++i) {
        advance_needs(*g);
    }
    CHECK(sim.fatigue == 0.0f);
    CHECK(sim.content == 0.0f);
}

TEST_CASE("the drain rate is expressed in in-game hours and is live") {
    auto g_owned = make_world(BrainDesc{});
    BadlandsGame* g = g_owned.get();
    uint32_t slot = spawn_into(*g, MercenaryDesc(0.0f, 0.0f));
    auto& sim = g->registry.get<HeroSimulationState>(g->slots[slot]);

    // fatigue_drain_hours ticks should take a full reserve to roughly empty.
    const int64_t ticks = g->factors.hero.fatigue_drain_hours *
                          static_cast<float>(kMillisPerGameHour) / kMillisPerTick;
    sim.fatigue = 1.0f;
    for (int64_t i = 0; i < ticks; ++i) {
        advance_needs(*g);
    }
    CHECK(sim.fatigue == Catch::Approx(0.0f).margin(0.02f));

    // Retuning the rate takes effect immediately -- no respawn.
    SimFactors f = g->factors;
    f.hero.fatigue_drain_hours = 1.0f;  // ten-fold faster
    set_factors_of(*g, f);
    sim.fatigue = 1.0f;
    const float slow_step = 1.0f;
    advance_needs(*g);
    CHECK(1.0f - sim.fatigue > slow_step * 0.0f);  // it moved
    const float fast_drop = 1.0f - sim.fatigue;
    CHECK(fast_drop > 1.0f / static_cast<float>(ticks));  // faster than the old rate
}

TEST_CASE("resting at home refills fatigue over time, not instantly") {
    auto g_owned = make_world(BrainDesc{});
    BadlandsGame* g = g_owned.get();
    uint32_t guild = place(g, static_cast<int32_t>(BuildingKind::FreeCompanyQuarters), -20.0f, 20.0f);
    uint32_t hid = recruit_at(g, guild);
    REQUIRE(hid != UINT32_MAX);
    entt::entity e = g->slots[hid];

    glm::vec2 tile;
    REQUIRE(building_approach_tile(g->placement, g->placement.buildings[guild], tile));
    g->registry.get<Position>(e).pos = tile;
    auto& sim = g->registry.get<HeroSimulationState>(e);
    sim.fatigue = 0.2f;

    REQUIRE(hero_enter_home(*g, e));
    // Entering did NOT zero the deficit -- the reserve is unchanged at the door.
    CHECK(sim.fatigue == Catch::Approx(0.2f));
    REQUIRE(g->registry.all_of<InsideBuilding>(e));

    // It fills as time passes inside.
    advance_needs(*g);
    CHECK(sim.fatigue > 0.2f);
    CHECK(sim.fatigue < 1.0f);  // not there yet -- it takes hours
}

TEST_CASE("a hero leaves home exactly when rested, not on a timer") {
    auto g_owned = make_world(BrainDesc{});
    BadlandsGame* g = g_owned.get();
    uint32_t guild = place(g, static_cast<int32_t>(BuildingKind::FreeCompanyQuarters), -20.0f, 20.0f);
    uint32_t hid = recruit_at(g, guild);
    entt::entity e = g->slots[hid];

    glm::vec2 tile;
    REQUIRE(building_approach_tile(g->placement, g->placement.buildings[guild], tile));
    g->registry.get<Position>(e).pos = tile;
    g->registry.get<HeroSimulationState>(e).fatigue = 0.1f;
    REQUIRE(hero_enter_home(*g, e));

    // Sleeps until the reserve tops out, then is released -- however many ticks
    // that takes. A hero that went in nearly empty stays longer than one that
    // ducked in half-rested; nothing hardcodes the duration.
    int ticks = 0;
    while (g->registry.all_of<InsideBuilding>(e) && ticks < 100000) {
        advance_needs(*g);
        advance_inside(*g);
        ++ticks;
    }
    CHECK_FALSE(g->registry.all_of<InsideBuilding>(e));
    CHECK(g->registry.get<HeroSimulationState>(e).fatigue >= 1.0f);
    CHECK(ticks > 1);  // it was NOT a single-tick reset
}

TEST_CASE("the tavern refills content over time and releases when full") {
    auto g_owned = make_world(BrainDesc{});
    BadlandsGame* g = g_owned.get();
    uint32_t guild = place(g, static_cast<int32_t>(BuildingKind::FreeCompanyQuarters), -20.0f, 20.0f);
    uint32_t tavern = place(g, static_cast<int32_t>(BuildingKind::Tavern), 20.0f, 20.0f);
    uint32_t hid = recruit_at(g, guild);
    entt::entity e = g->slots[hid];

    glm::vec2 tile;
    REQUIRE(building_approach_tile(g->placement, g->placement.buildings[tavern], tile));
    g->registry.get<Position>(e).pos = tile;
    auto& sim = g->registry.get<HeroSimulationState>(e);
    sim.content = 0.3f;
    sim.fatigue = 1.0f;  // so rest never pulls it out first

    REQUIRE(hero_enter(*g, e, static_cast<int32_t>(BuildingKind::Tavern)));
    CHECK(sim.content == Catch::Approx(0.3f));  // no instant satisfaction

    int ticks = 0;
    while (g->registry.all_of<InsideBuilding>(e) && ticks < 100000) {
        advance_needs(*g);
        advance_inside(*g);
        ++ticks;
    }
    CHECK(sim.content >= 1.0f);
    CHECK(ticks > 1);
}

TEST_CASE("a hidden hero refills only the reserve it went in for") {
    auto g_owned = make_world(BrainDesc{});
    BadlandsGame* g = g_owned.get();
    CharacterDesc d = MercenaryDesc(0.0f, 0.0f);
    uint32_t slot = spawn_into(*g, d);
    entt::entity e = g->slots[slot];
    auto& sim = g->registry.get<HeroSimulationState>(e);
    sim.fatigue = 0.3f;
    sim.content = 0.3f;

    // Sleeping at home: fatigue rises, content does NOT drain (it is paused, not
    // running down, while inside).
    g->registry.emplace<InsideBuilding>(e, 0, static_cast<int32_t>(Behavior::GoHome));
    advance_needs(*g);
    CHECK(sim.fatigue > 0.3f);
    CHECK(sim.content == Catch::Approx(0.3f));
}

TEST_CASE("a rested hero heads home once fatigue falls past the seek bar") {
    auto g_owned = make_world(BrainDesc{});
    BadlandsGame* g = g_owned.get();  // C++ brain (no noiser)
    uint32_t guild = place(g, static_cast<int32_t>(BuildingKind::FreeCompanyQuarters), -20.0f, 20.0f);
    uint32_t hid = recruit_at(g, guild);
    REQUIRE(hid != UINT32_MAX);
    entt::entity e = g->slots[hid];

    glm::vec2 home_door;
    REQUIRE(building_approach_tile(g->placement, g->placement.buildings[guild], home_door));

    g->registry.get<Position>(e).pos = {40.0f, 40.0f};  // far from home
    g->world_millis = kMillisPerDay / 2;                 // midday, so the night bar doesn't apply
    // Well below the daytime seek bar -> the hero wants to rest.
    g->registry.get<HeroSimulationState>(e).fatigue = 0.2f;
    // Deliberation off: this case is about the need, not the pause.
    SimFactors f = g->factors;
    f.hero.think_max_millis = 0;
    set_factors_of(*g, f);

    tick_world(*g, 1.0f / 30.0f);

    CHECK(g->registry.get<HeroSimulationState>(e).behavior ==
          static_cast<int32_t>(Behavior::GoHome));
    const MoveTarget& mt = g->registry.get<MoveTarget>(e);
    CHECK(mt.kind == MoveTarget::Kind::Point);
    CHECK(mt.point.x == Catch::Approx(home_door.x));
    CHECK(mt.point.y == Catch::Approx(home_door.y));
}

TEST_CASE("an under-entertained hero heads to the tavern by day") {
    auto g_owned = make_world(BrainDesc{});
    BadlandsGame* g = g_owned.get();
    uint32_t guild = place(g, static_cast<int32_t>(BuildingKind::FreeCompanyQuarters), -20.0f, 20.0f);
    uint32_t tavern = place(g, static_cast<int32_t>(BuildingKind::Tavern), 20.0f, 20.0f);
    uint32_t hid = recruit_at(g, guild);
    entt::entity e = g->slots[hid];

    glm::vec2 tavern_door;
    REQUIRE(building_approach_tile(g->placement, g->placement.buildings[tavern], tavern_door));

    g->registry.get<Position>(e).pos = {0.0f, 0.0f};
    g->world_millis = kMillisPerDay / 2;  // midday
    auto& sim = g->registry.get<HeroSimulationState>(e);
    sim.fatigue = 1.0f;   // fully rested, so rest does not compete
    sim.content = 0.1f;   // starved of diversion
    SimFactors f = g->factors;
    f.hero.think_max_millis = 0;
    set_factors_of(*g, f);

    tick_world(*g, 1.0f / 30.0f);

    CHECK(g->registry.get<HeroSimulationState>(e).behavior ==
          static_cast<int32_t>(Behavior::VisitTavern));
    const MoveTarget& mt = g->registry.get<MoveTarget>(e);
    CHECK(mt.point.x == Catch::Approx(tavern_door.x));
    CHECK(mt.point.y == Catch::Approx(tavern_door.y));
}

TEST_CASE("the seek threshold is data, and urgency (not a tier) decides") {
    // Policy is data: the same reserve produces a different decision when the
    // threshold moves. This is what makes assets/creatures/factors.json
    // meaningful.
    auto owned = make_world(BrainDesc{});
    BadlandsGame& g = *owned;
    CHECK(g.factors.hero.fatigue_seek == Catch::Approx(0.55f));

    uint32_t guild = place(&g, static_cast<int32_t>(BuildingKind::FreeCompanyQuarters), -20.0f, 20.0f);
    uint32_t hid = recruit_at(&g, guild);
    entt::entity e = g.slots[hid];
    g.registry.get<Position>(e).pos = {40.0f, 40.0f};
    g.world_millis = kMillisPerDay / 2;  // midday
    SimFactors off = g.factors;
    off.hero.think_max_millis = 0;
    set_factors_of(g, off);

    // fatigue 0.5 near the 0.55 seek bar -> the urge is too mild to beat the
    // fallback wander, so the hero does not head home.
    g.registry.get<HeroSimulationState>(e).fatigue = 0.5f;
    g.registry.get<HeroSimulationState>(e).content = 1.0f;  // and not bored
    tick_world(g, 1.0f / 30.0f);
    CHECK(g.registry.get<HeroSimulationState>(e).behavior !=
          static_cast<int32_t>(Behavior::GoHome));

    // Raise the bar well past the reserve -> the urgency climbs enough to win.
    SimFactors f = g.factors;
    f.hero.fatigue_seek = 0.9f;
    set_factors_of(g, f);
    tick_world(g, 1.0f / 30.0f);
    CHECK(g.registry.get<HeroSimulationState>(e).behavior ==
          static_cast<int32_t>(Behavior::GoHome));
}
