// Hero recruitment, residency/class, entering/hiding, and the destruction
// cascade. Driven through game_dispatch (recruit/destroy) and the internal
// errand mechanics (enter/exit/buy) -- no UI, no brain.

#include "badlands_game.h"
#include "components.h"
#include "game_state.h"
#include "heroes.h"
#include "placement.h"

#include <catch_amalgamated.hpp>
#include <entt/entt.hpp>
#include <glm/glm.hpp>

#include <cmath>
#include <vector>

using namespace badlands;

namespace {

uint32_t place_at(BadlandsGame* g, int kind, float x, float z) {
    GameAction a{GAME_ACTION_PLACE_BUILDING, 0, x, z, kind, 0};
    int64_t r = game_dispatch(g, &a);
    return (r < 0) ? UINT32_MAX : static_cast<uint32_t>(r);
}

uint32_t recruit_at(BadlandsGame* g, uint32_t bid) {
    GameAction a{GAME_ACTION_RECRUIT_HERO, bid, 0.0f, 0.0f, 0, 0};
    int64_t r = game_dispatch(g, &a);
    return (r < 0) ? UINT32_MAX : static_cast<uint32_t>(r);
}

int64_t destroy_at(BadlandsGame* g, uint32_t bid) {
    GameAction a{GAME_ACTION_DESTROY_BUILDING, bid, 0.0f, 0.0f, 0, 0};
    return game_dispatch(g, &a);
}

std::vector<GameCharacterState> chars(BadlandsGame* g) {
    std::vector<GameCharacterState> rows(64);
    uint32_t n = game_state(g, rows.data(), static_cast<uint32_t>(rows.size()));
    rows.resize(n);
    return rows;
}

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
        int kind;
        int32_t cls;
    };
    Guild guilds[] = {
        {GAME_BUILDING_FREE_COMPANY_QUARTERS, HERO_WARRIOR},
        {GAME_BUILDING_HUNTERS_CAMP, HERO_RANGER},
        {GAME_BUILDING_THIEVES_DEN, HERO_ROGUE},
        {GAME_BUILDING_SCRIPTORIUM, HERO_WIZARD},
    };
    for (const Guild& gd : guilds) {
        BadlandsGame* game = game_create(nullptr);
        uint32_t bid = place_at(game, gd.kind, -30.0f, 30.0f);
        REQUIRE(bid != UINT32_MAX);
        uint32_t hid = recruit_at(game, bid);
        REQUIRE(hid != UINT32_MAX);

        entt::entity e = game->slots[hid];
        CHECK(game->registry.get<Home>(e).building_id == static_cast<int32_t>(bid));
        CHECK(game->registry.get<HeroClass>(e).value == gd.cls);
        CHECK(game->registry.get<Inventory>(e).count == 0);

        // Class-derived color matches hero_desc, and game_state exposes the home.
        GameCharacterDesc want = hero_desc(gd.cls, 0.0f, 0.0f);
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

        game_destroy(game);
    }
}

TEST_CASE("recruit rejects non-guilds, missing buildings, and a full roster") {
    BadlandsGame* game = game_create(nullptr);
    uint32_t tavern = place_at(game, GAME_BUILDING_TAVERN, -30.0f, 30.0f);
    REQUIRE(tavern != UINT32_MAX);
    CHECK(recruit_at(game, tavern) == UINT32_MAX);  // not a guild
    CHECK(recruit_at(game, 999) == UINT32_MAX);     // no such building

    uint32_t guild = place_at(game, GAME_BUILDING_FREE_COMPANY_QUARTERS, 30.0f, 30.0f);
    REQUIRE(guild != UINT32_MAX);
    for (int i = 0; i < kGuildRosterCap; ++i) {
        CHECK(recruit_at(game, guild) != UINT32_MAX);
    }
    CHECK(recruit_at(game, guild) == UINT32_MAX);  // 5th over cap
    CHECK(roster_count(*game, guild) == static_cast<uint32_t>(kGuildRosterCap));

    game_destroy(game);
}

TEST_CASE("a hero enters a building, hides from game_state's drawable set, then reappears") {
    BadlandsGame* game = game_create(nullptr);
    uint32_t guild = place_at(game, GAME_BUILDING_SCRIPTORIUM, -30.0f, 30.0f);
    uint32_t apo = place_at(game, GAME_BUILDING_APOTHECARY, 30.0f, 30.0f);
    uint32_t hid = recruit_at(game, guild);
    REQUIRE(hid != UINT32_MAX);
    entt::entity e = game->slots[hid];

    // Stand at the apothecary's door, then enter.
    glm::vec2 tile;
    REQUIRE(building_approach_tile(game->placement, game->placement.buildings[apo], tile));
    game->registry.get<Position>(e).pos = tile;
    REQUIRE(hero_enter(*game, e, GAME_BUILDING_APOTHECARY));

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
        game_tick(game, 1.0f / 30.0f);
    }
    CHECK(!game->registry.all_of<InsideBuilding>(e));
    for (const auto& row : chars(game)) {
        if (row.id == hid) {
            CHECK(row.inside_building_id == -1);
        }
    }
    CHECK(glm::distance(game->registry.get<Position>(e).pos, tile) < 1.5f);

    game_destroy(game);
}

TEST_CASE("buying at an apothecary fills the hero's inventory") {
    BadlandsGame* game = game_create(nullptr);
    uint32_t guild = place_at(game, GAME_BUILDING_HUNTERS_CAMP, -30.0f, 30.0f);
    uint32_t apo = place_at(game, GAME_BUILDING_APOTHECARY, 30.0f, 30.0f);
    uint32_t hid = recruit_at(game, guild);
    REQUIRE(hid != UINT32_MAX);
    entt::entity e = game->slots[hid];

    CHECK(game->registry.get<Inventory>(e).count == 0);
    CHECK(!hero_buy(*game, e));  // too far from the apothecary

    glm::vec2 tile;
    REQUIRE(building_approach_tile(game->placement, game->placement.buildings[apo], tile));
    game->registry.get<Position>(e).pos = tile;
    CHECK(hero_buy(*game, e));
    CHECK(game->registry.get<Inventory>(e).count == kInventoryCap);

    game_destroy(game);
}

TEST_CASE("DESTROY_BUILDING cascades: expel, reassign or orphan, free the footprint") {
    BadlandsGame* game = game_create(nullptr);
    uint32_t g1 = place_at(game, GAME_BUILDING_FREE_COMPANY_QUARTERS, -20.0f, 20.0f);
    REQUIRE(g1 != UINT32_MAX);

    SECTION("rejects the non-destructible castle") {
        CHECK(destroy_at(game, 0) < 0);
        CHECK(game->placement.buildings[0].alive);
    }

    SECTION("reassigns residents to another same-class guild with room") {
        uint32_t g2 = place_at(game, GAME_BUILDING_FREE_COMPANY_QUARTERS, 20.0f, 20.0f);
        std::vector<uint32_t> heroes;
        for (int i = 0; i < 3; ++i) {
            heroes.push_back(recruit_at(game, g1));
        }
        CHECK(destroy_at(game, g1) == 0);
        for (uint32_t hid : heroes) {
            CHECK(game->registry.get<Home>(game->slots[hid]).building_id ==
                  static_cast<int32_t>(g2));
        }
        CHECK(roster_count(*game, g2) == 3);
    }

    SECTION("orphans residents when no same-class guild has room") {
        uint32_t h0 = recruit_at(game, g1);
        uint32_t h1 = recruit_at(game, g1);
        CHECK(destroy_at(game, g1) == 0);
        CHECK(game->registry.get<Home>(game->slots[h0]).building_id == -1);
        CHECK(game->registry.get<Home>(game->slots[h1]).building_id == -1);
    }

    SECTION("expels an inside hero and frees the footprint") {
        uint32_t hid = recruit_at(game, g1);
        entt::entity e = game->slots[hid];
        game->registry.emplace<InsideBuilding>(e, static_cast<int32_t>(g1),
                                               kInsideDurationSeconds);

        Footprint fp =
            make_footprint(GAME_BUILDING_FREE_COMPANY_QUARTERS, 0, game->placement.buildings[g1].center);
        std::vector<TriRef> foot;
        footprint_triangles(fp, foot);
        int idx = tri_index(foot[0].tx, foot[0].tz, static_cast<int>(foot[0].corner));
        REQUIRE(game->placement.blocked[idx] == 1);

        CHECK(destroy_at(game, g1) == 0);
        CHECK(game->placement.blocked[idx] == 0);          // footprint freed
        CHECK(!game->registry.all_of<InsideBuilding>(e));  // expelled
        CHECK(!game->placement.buildings[g1].alive);
    }

    game_destroy(game);
}
