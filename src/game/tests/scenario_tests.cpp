// Spec of the app-layer sandbox loaders (scenario + creature overrides). Both
// parse JSON into sim data; the sim core never links a JSON parser.

#include "game/creature_manifest.h"
#include "game/scenario.h"

#include <catch_amalgamated.hpp>

#include <fstream>
#include <string>

using namespace badlands;

namespace {

std::string write_temp(const char* name, const std::string& content) {
    const std::string path = std::string("/tmp/badlands_") + name;
    std::ofstream(path) << content;
    return path;
}

}  // namespace

TEST_CASE("LoadScenario parses an arena scenario and its WorldConfig") {
    const std::string p = write_temp("scn_ok.json", R"({
        "name": "duel",
        "arena": { "half_x": 12, "half_z": 8 },
        "spawns": [ { "creature": "Mercenary", "team": 0, "x": -6, "z": 0 },
                    { "creature": "Goblin",    "team": 1, "x":  6, "z": 1 } ] })");
    Scenario s;
    REQUIRE(LoadScenario(p, s));
    CHECK(s.is_arena());
    CHECK(s.arena_half_x == 12.0f);
    REQUIRE(s.spawns.size() == 2);
    CHECK(s.spawns[0].creature == CreatureId::Mercenary);
    CHECK(s.spawns[1].team == 1);
    CHECK(s.spawns[1].z == 1.0f);

    const WorldConfig wc = s.world_config();
    CHECK(wc.prebuild_colony == false);
    CHECK(wc.terrain_blocking == false);
    CHECK(wc.arena_half_z == 8.0f);
}

TEST_CASE("LoadScenario rejects an unknown creature and leaves the target untouched") {
    const std::string p =
        write_temp("scn_bad.json", R"({ "spawns": [ { "creature": "Dragon", "team": 0 } ] })");
    Scenario s;
    s.name = "keep-me";
    CHECK_FALSE(LoadScenario(p, s));
    CHECK(s.name == "keep-me");
}

TEST_CASE("LoadCreatureCatalog overrides scalar stats by name") {
    const std::string p = write_temp(
        "cre_ok.json", R"({ "_note": "docs", "Mercenary": { "hp": 40, "armour": 5 } })");
    CreatureCatalog cat;
    const float goblin_hp0 = cat.defs[static_cast<int>(CreatureId::Goblin)].hp;
    REQUIRE(LoadCreatureCatalog(p, cat));
    CHECK(cat.defs[static_cast<int>(CreatureId::Mercenary)].hp == 40.0f);
    CHECK(cat.defs[static_cast<int>(CreatureId::Mercenary)].armour == 5.0f);
    CHECK(cat.defs[static_cast<int>(CreatureId::Goblin)].hp == goblin_hp0);  // others untouched
}

TEST_CASE("tally_arena ignores neutral critters and finds the winner by team") {
    auto row = [](int32_t team, Archetype a) {
        CharacterState c{};
        c.team = team;
        c.archetype = static_cast<int32_t>(a);
        return c;
    };

    SECTION("two combatant teams -> no winner yet") {
        const std::vector<CharacterState> rows = {row(0, Archetype::Hero),
                                                  row(1, Archetype::Monster)};
        const ArenaTally t = tally_arena(rows);
        CHECK(t.teams.size() == 2);
        CHECK(t.winner == -1);
    }
    SECTION("a combatant on team 2 is counted, not treated as neutral") {
        const std::vector<CharacterState> rows = {row(0, Archetype::Hero),
                                                  row(2, Archetype::Monster)};
        const ArenaTally t = tally_arena(rows);
        CHECK(t.teams.size() == 2);
        CHECK(t.winner == -1);
    }
    SECTION("a neutral deer never keeps its team alive") {
        const std::vector<CharacterState> rows = {row(0, Archetype::Hero),
                                                  row(1, Archetype::Critter)};
        const ArenaTally t = tally_arena(rows);
        REQUIRE(t.teams.size() == 1);
        CHECK(t.winner == 0);
    }
}

TEST_CASE("LoadCreatureCatalog rejects a non-numeric value") {
    const std::string p = write_temp("cre_bad.json", R"({ "Rat": { "hp": "lots" } })");
    CreatureCatalog cat;
    const float rat_hp0 = cat.defs[static_cast<int>(CreatureId::Rat)].hp;
    CHECK_FALSE(LoadCreatureCatalog(p, cat));
    CHECK(cat.defs[static_cast<int>(CreatureId::Rat)].hp == rat_hp0);  // untouched on failure
}
