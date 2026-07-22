// Archetype spawn recipes: an entity's archetype decides which components and
// which brain it gets. Nothing asks "is this a Hero" at think time -- perception
// is relational -- but the RECIPE is what stops a goblin owning hero state and
// running the townsfolk errand loop.
//
// This file pins the root fix for that regression. Before it, spawn_entity
// emplaced HeroCharacter/HeroSimulationState/HeroDisplayState on EVERY entity,
// so any enemy with no target walked to the player's apothecary and bought
// elixirs; a one-line guard in sim.cpp's mock_think held it back.

#include "brain.h"
#include "command.h"
#include "components.h"
#include "game_state.h"
#include "heroes.h"
#include "placement.h"
#include "sim_internal.hpp"

#include <catch_amalgamated.hpp>

#include <entt/entt.hpp>

#include <set>
#include <string>
#include <vector>

using namespace badlands;

namespace {

uint32_t place(BadlandsGame& g, BuildingKind kind, float x, float z) {
    Action a{ActionKind::PlaceBuilding, 0, x, z, static_cast<int32_t>(kind), 0};
    int64_t r = dispatch_into(g, a);
    return (r < 0) ? UINT32_MAX : static_cast<uint32_t>(r);
}

bool log_has(const BadlandsGame& g, CommandKind kind) {
    for (const Command& c : g.command_log) {
        if (c.kind == kind) {
            return true;
        }
    }
    return false;
}

}  // namespace

TEST_CASE("a monster spawn carries no hero components") {
    auto owned = make_world(nullptr);
    BadlandsGame& g = *owned;
    CharacterDesc goblin = GoblinDesc(5.0f, 5.0f);
    uint32_t slot = spawn_into(g, goblin);
    entt::entity e = g.slots[slot];

    CHECK_FALSE(g.registry.all_of<HeroSimulationState>(e));
    CHECK_FALSE(g.registry.all_of<HeroCharacter>(e));
    CHECK_FALSE(g.registry.all_of<HeroDisplayState>(e));

    // Generic combat/movement components are archetype-independent.
    CHECK(g.registry.all_of<Position>(e));
    CHECK(g.registry.all_of<Health>(e));
    CHECK(g.registry.all_of<MoveTarget>(e));
}

TEST_CASE("a hero spawn carries the full hero recipe") {
    auto owned = make_world(nullptr);
    BadlandsGame& g = *owned;
    CharacterDesc merc = MercenaryDesc(-5.0f, -5.0f);
    uint32_t slot = spawn_into(g, merc);
    entt::entity e = g.slots[slot];

    CHECK(g.registry.all_of<HeroSimulationState>(e));
    CHECK(g.registry.all_of<HeroCharacter>(e));
    CHECK(g.registry.all_of<HeroDisplayState>(e));
}

TEST_CASE("an enemy never runs the townsfolk errand loop") {
    // THE regression, stated directly. An apothecary/tavern exist and the
    // goblin's inventory would be "empty" if it had one -- under the old recipe
    // it walked over and emitted Buy. It must never run a townsfolk errand.
    // (A monster DOES seek a building to attack -- that is its own brain, tested
    // in rat_tests -- so the invariant here is specifically "no shopping", not
    // "no movement".)
    auto owned = make_world(nullptr);
    BadlandsGame& g = *owned;
    REQUIRE(place(g, BuildingKind::Apothecary, 14.0f, 8.0f) != UINT32_MAX);
    REQUIRE(place(g, BuildingKind::Tavern, 14.0f, -8.0f) != UINT32_MAX);

    CharacterDesc goblin = GoblinDesc(-20.0f, -20.0f);  // no enemy unit present
    spawn_into(g, goblin);

    for (int i = 0; i < 60; ++i) {
        tick_world(g, 1.0f / 30.0f);
    }

    CHECK_FALSE(log_has(g, CommandKind::Buy));
    CHECK_FALSE(log_has(g, CommandKind::EnterBuilding));
    CHECK_FALSE(log_has(g, CommandKind::EnterHome));
}

TEST_CASE("recruited heroes still run the town loop") {
    // The other side of the guard: the recipe must not silence actual townsfolk.
    auto owned = make_world(nullptr);
    BadlandsGame& g = *owned;
    uint32_t guild = place(g, BuildingKind::FreeCompanyQuarters, -14.0f, -8.0f);
    REQUIRE(guild != UINT32_MAX);
    REQUIRE(place(g, BuildingKind::Apothecary, 14.0f, 8.0f) != UINT32_MAX);
    Action recruit{ActionKind::RecruitHero, guild, 0.0f, 0.0f, 0, 0};
    REQUIRE(dispatch_into(g, recruit) >= 0);

    for (int i = 0; i < 60; ++i) {
        tick_world(g, 1.0f / 30.0f);
    }

    CHECK(log_has(g, CommandKind::MoveTo));
    CHECK(log_has(g, CommandKind::SetBehavior));
}

TEST_CASE("non-heroes report an empty name through the snapshot") {
    // badlands_sim.hpp documents name as "" for non-heroes; under the old recipe
    // every entity was named, so that contract was unachievable.
    auto owned = make_world(nullptr);
    BadlandsGame& g = *owned;
    CharacterDesc goblin = GoblinDesc(3.0f, 3.0f);
    spawn_into(g, goblin);
    CharacterDesc merc = MercenaryDesc(-3.0f, -3.0f);
    spawn_into(g, merc);

    const std::vector<CharacterState> rows = characters_of(g);
    REQUIRE(rows.size() == 2);
    CHECK(std::string(rows[0].name).empty());   // goblin
    CHECK_FALSE(std::string(rows[1].name).empty());  // mercenary
}

TEST_CASE("the sim owns a biome map and queries it in world coordinates") {
    // Deer roam Forest/Plains and hunters seek Forest, so the sim -- not just
    // the renderer -- needs the biome field. The world<->map offset lives only
    // in biome_at/height_at.
    auto owned = make_world(nullptr);
    BadlandsGame& g = *owned;
    REQUIRE_FALSE(g.map.empty());

    // The symbolic map is 256 m centred on the world origin, with a lake in the
    // middle -- so the origin must NOT read as a land biome, and the map must
    // contain more than one biome across its span.
    CHECK(biome_at(g, {0.0f, 0.0f}) == mapgen::Biome::Lake);

    std::set<mapgen::Biome> seen;
    for (float z = -120.0f; z <= 120.0f; z += 8.0f) {
        for (float x = -120.0f; x <= 120.0f; x += 8.0f) {
            seen.insert(biome_at(g, {x, z}));
        }
    }
    CHECK(seen.size() >= 3);            // lake + at least two land biomes
    CHECK(seen.count(mapgen::Biome::Forest) == 1);
    CHECK(seen.count(mapgen::Biome::Plains) == 1);

    // Out of bounds is clamped, not UB.
    CHECK_NOTHROW(biome_at(g, {-9000.0f, 9000.0f}));
}
