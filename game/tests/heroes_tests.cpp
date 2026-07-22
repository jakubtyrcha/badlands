// Hero recruitment, residency/class, entering/hiding, and the destruction
// cascade. Driven through dispatch_into (recruit/destroy) and the internal
// errand mechanics (enter/exit/buy) -- no UI, no brain.

#include "components.h"
#include "game_state.h"
#include "heroes.h"
#include "placement.h"
#include "sim_internal.hpp"  // make_world / dispatch_into / characters_of / tick_world

#include <catch_amalgamated.hpp>
#include <entt/entt.hpp>
#include <glm/glm.hpp>

#include <cmath>
#include <vector>

using namespace badlands;

namespace {

uint32_t place_at(BadlandsGame* g, BuildingKind kind, float x, float z) {
    Action a{ActionKind::PlaceBuilding, 0, x, z, static_cast<int32_t>(kind), 0};
    int64_t r = dispatch_into(*g, a);
    return (r < 0) ? UINT32_MAX : static_cast<uint32_t>(r);
}

uint32_t recruit_at(BadlandsGame* g, uint32_t bid) {
    Action a{ActionKind::RecruitHero, bid, 0.0f, 0.0f, 0, 0};
    int64_t r = dispatch_into(*g, a);
    return (r < 0) ? UINT32_MAX : static_cast<uint32_t>(r);
}

int64_t destroy_at(BadlandsGame* g, uint32_t bid) {
    Action a{ActionKind::DestroyBuilding, bid, 0.0f, 0.0f, 0, 0};
    return dispatch_into(*g, a);
}

std::vector<CharacterState> chars(BadlandsGame* g) { return characters_of(*g); }

bool tile_free(const BadlandsGame* g, glm::vec2 p) {
    int tx = static_cast<int>(std::floor(p.x));
    int tz = static_cast<int>(std::floor(p.y));
    for (int c = 0; c < 4; ++c) {
        if (g->placement.blocked[tri_index(tx, tz, c)]) {
            return false;
        }
    }
    return true;
}

}  // namespace

TEST_CASE("recruit spawns a class-tinted hero on a free tile at each guild") {
    struct Guild {
        BuildingKind kind;
        int32_t cls;
    };
    Guild guilds[] = {
        {BuildingKind::FreeCompanyQuarters, HERO_MERCENARY},
        {BuildingKind::HuntersCamp, HERO_HUNTER},
        {BuildingKind::ThievesDen, HERO_GRAVE_ROBBER},
        {BuildingKind::Scriptorium, HERO_APPRENTICE},
    };
    for (const Guild& gd : guilds) {
        auto owned = make_world(nullptr);
        BadlandsGame* game = owned.get();
        uint32_t bid = place_at(game, gd.kind, -30.0f, 30.0f);
        REQUIRE(bid != UINT32_MAX);
        uint32_t hid = recruit_at(game, bid);
        REQUIRE(hid != UINT32_MAX);

        entt::entity e = game->slots[hid];
        CHECK(game->registry.get<HeroSimulationState>(e).home_building_id ==
              static_cast<int32_t>(bid));
        CHECK(game->registry.get<HeroCharacter>(e).hero_class == gd.cls);
        CHECK(game->registry.get<HeroSimulationState>(e).inventory == 0);

        // Class-derived color matches hero_desc, and Characters() exposes the home.
        CharacterDesc want = hero_desc(gd.cls, 0.0f, 0.0f);
        CHECK_THAT(game->registry.get<RenderShape>(e).color.x,
                   Catch::Matchers::WithinAbs(want.color_r, 1e-4f));
        for (const auto& row : chars(game)) {
            if (row.id == hid) {
                CHECK(row.home_building_id == static_cast<int32_t>(bid));
                CHECK(row.inside_building_id == -1);
            }
        }
        // Spawned on a free exterior tile.
        CHECK(tile_free(game, game->registry.get<Position>(e).pos));
    }
}

TEST_CASE("recruit rejects non-guilds, missing buildings, and a full roster") {
    auto owned = make_world(nullptr);
    BadlandsGame* game = owned.get();
    uint32_t tavern = place_at(game, BuildingKind::Tavern, -30.0f, 30.0f);
    REQUIRE(tavern != UINT32_MAX);
    CHECK(recruit_at(game, tavern) == UINT32_MAX);  // not a guild
    CHECK(recruit_at(game, 999) == UINT32_MAX);     // no such building

    uint32_t guild = place_at(game, BuildingKind::FreeCompanyQuarters, 30.0f, 30.0f);
    REQUIRE(guild != UINT32_MAX);
    for (int i = 0; i < kGuildRosterCap; ++i) {
        CHECK(recruit_at(game, guild) != UINT32_MAX);
    }
    CHECK(recruit_at(game, guild) == UINT32_MAX);  // 5th over cap
    CHECK(roster_count(*game, guild) == static_cast<uint32_t>(kGuildRosterCap));

}

TEST_CASE("a hero enters a building, hides from Characters()'s drawable set, then reappears") {
    auto owned = make_world(nullptr);
    BadlandsGame* game = owned.get();
    uint32_t guild = place_at(game, BuildingKind::Scriptorium, -30.0f, 30.0f);
    uint32_t apo = place_at(game, BuildingKind::Apothecary, 30.0f, 30.0f);
    uint32_t hid = recruit_at(game, guild);
    REQUIRE(hid != UINT32_MAX);
    entt::entity e = game->slots[hid];

    // Stand at the apothecary's door, then enter.
    glm::vec2 tile;
    REQUIRE(building_approach_tile(game->placement, game->placement.buildings[apo], tile));
    game->registry.get<Position>(e).pos = tile;
    REQUIRE(hero_enter(*game, e, static_cast<int32_t>(BuildingKind::Apothecary)));

    bool seen = false;
    for (const auto& row : chars(game)) {
        if (row.id == hid) {
            seen = true;
            CHECK(row.inside_building_id == static_cast<int32_t>(apo));  // still listed
        }
    }
    CHECK(seen);

    // Tick past the stay: the hero reappears at the approach tile.
    for (int i = 0; i < 300 && game->registry.all_of<InsideBuilding>(e); ++i) {
        tick_world(*game, 1.0f / 30.0f);
    }
    CHECK(!game->registry.all_of<InsideBuilding>(e));
    for (const auto& row : chars(game)) {
        if (row.id == hid) {
            CHECK(row.inside_building_id == -1);
        }
    }
    CHECK(glm::distance(game->registry.get<Position>(e).pos, tile) < 1.5f);

}

TEST_CASE("buying at an apothecary fills the hero's inventory") {
    auto owned = make_world(nullptr);
    BadlandsGame* game = owned.get();
    uint32_t guild = place_at(game, BuildingKind::HuntersCamp, -30.0f, 30.0f);
    uint32_t apo = place_at(game, BuildingKind::Apothecary, 30.0f, 30.0f);
    uint32_t hid = recruit_at(game, guild);
    REQUIRE(hid != UINT32_MAX);
    entt::entity e = game->slots[hid];

    CHECK(game->registry.get<HeroSimulationState>(e).inventory == 0);
    CHECK(!hero_buy(*game, e));  // too far from the apothecary

    glm::vec2 tile;
    REQUIRE(building_approach_tile(game->placement, game->placement.buildings[apo], tile));
    game->registry.get<Position>(e).pos = tile;
    CHECK(hero_buy(*game, e));
    CHECK(game->registry.get<HeroSimulationState>(e).inventory == kInventoryCap);

}

TEST_CASE("DESTROY_BUILDING cascades: expel, reassign or orphan, free the footprint") {
    auto owned = make_world(nullptr);
    BadlandsGame* game = owned.get();
    uint32_t g1 = place_at(game, BuildingKind::FreeCompanyQuarters, -20.0f, 20.0f);
    REQUIRE(g1 != UINT32_MAX);

    SECTION("rejects the non-destructible castle") {
        CHECK(destroy_at(game, 0) < 0);
        CHECK(game->placement.buildings[0].alive);
    }

    SECTION("reassigns residents to another same-class guild with room") {
        uint32_t g2 = place_at(game, BuildingKind::FreeCompanyQuarters, 20.0f, 20.0f);
        std::vector<uint32_t> heroes;
        for (int i = 0; i < 3; ++i) {
            heroes.push_back(recruit_at(game, g1));
        }
        CHECK(destroy_at(game, g1) == 0);
        for (uint32_t hid : heroes) {
            CHECK(game->registry.get<HeroSimulationState>(game->slots[hid]).home_building_id ==
                  static_cast<int32_t>(g2));
        }
        CHECK(roster_count(*game, g2) == 3);
    }

    SECTION("orphans residents when no same-class guild has room") {
        uint32_t h0 = recruit_at(game, g1);
        uint32_t h1 = recruit_at(game, g1);
        CHECK(destroy_at(game, g1) == 0);
        CHECK(game->registry.get<HeroSimulationState>(game->slots[h0]).home_building_id == -1);
        CHECK(game->registry.get<HeroSimulationState>(game->slots[h1]).home_building_id == -1);
    }

    SECTION("expels an inside hero and frees the footprint") {
        uint32_t hid = recruit_at(game, g1);
        entt::entity e = game->slots[hid];
        game->registry.emplace<InsideBuilding>(e, static_cast<int32_t>(g1),
                                               kInsideDurationSeconds);

        Footprint fp = make_footprint(static_cast<int32_t>(BuildingKind::FreeCompanyQuarters), 0,
                                      game->placement.buildings[g1].center);
        std::vector<TriRef> foot;
        footprint_triangles(fp, foot);
        int idx = tri_index(foot[0].tx, foot[0].tz, static_cast<int>(foot[0].corner));
        REQUIRE(game->placement.blocked[idx] == 1);

        CHECK(destroy_at(game, g1) == 0);
        CHECK(game->placement.blocked[idx] == 0);          // footprint freed
        CHECK(!game->registry.all_of<InsideBuilding>(e));  // expelled
        CHECK(!game->placement.buildings[g1].alive);
    }

}
