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

TEST_CASE("every creature name round-trips through the id table") {
    // The manifest and the scenario loader both key by NAME, so a creature
    // appended to the enum without a kNames entry would silently become
    // unaddressable from data rather than failing here.
    for (int i = 0; i < kCreatureCount; ++i) {
        const CreatureId id = static_cast<CreatureId>(i);
        const char* name = CreatureName(id);
        REQUIRE(name[0] != '\0');
        CHECK(CreatureIdFromName(name) == id);
    }
    CHECK(CreatureIdFromName("Bandit") == CreatureId::Bandit);
    CHECK(CreatureIdFromName("BanditArcher") == CreatureId::BanditArcher);
    CHECK(CreatureIdFromName("BanditLeader") == CreatureId::BanditLeader);
    CHECK(CreatureIdFromName("MudGolem") == CreatureId::MudGolem);
}

// --- level scaling, as data --------------------------------------------------

TEST_CASE("LoadCreatureCatalog reads a creature's growth row") {
    const std::string p = write_temp(
        "cre_growth.json",
        R"({ "Mercenary": { "growth": { "hp": 3, "armour": 0.5, "damage_frac": 0.1 } } })");
    CreatureCatalog cat;
    const float evasion0 =
        cat.defs[static_cast<int>(CreatureId::Mercenary)].growth.evasion;
    REQUIRE(LoadCreatureCatalog(p, cat));
    const StatGrowth& g = cat.defs[static_cast<int>(CreatureId::Mercenary)].growth;
    CHECK(g.hp == 3.0f);
    CHECK(g.armour == 0.5f);
    CHECK(g.damage_frac == 0.1f);
    // Unlike the skill list, growth MERGES: it is a bag of independent
    // scalars, so overriding hp must not silently zero the others.
    CHECK(g.evasion == evasion0);
}

TEST_CASE("a malformed growth row fails the load loudly") {
    CreatureCatalog fresh;
    const float before = fresh.defs[static_cast<int>(CreatureId::Mercenary)].growth.hp;
    for (const char* body : {R"({ "Mercenary": { "growth": { "armor": 0.5 } } })",
                             R"({ "Mercenary": { "growth": { "hp": "fast" } } })",
                             R"({ "Mercenary": { "growth": 3 } })"}) {
        const std::string p = write_temp("cre_growth_bad.json", body);
        CreatureCatalog cat;
        CHECK_FALSE(LoadCreatureCatalog(p, cat));
        CHECK(cat.defs[static_cast<int>(CreatureId::Mercenary)].growth.hp == before);
    }
}

// --- level-gated skill acquisition, as data ---------------------------------
// Which creature learns what, at which level, is catalog data a designer can
// override without a rebuild. Fixtures are test-local: nothing here asserts on
// the shipped assets/creatures/creatures.json.

TEST_CASE("LoadCreatureCatalog reads a creature's level-gated skill list") {
    const std::string p = write_temp(
        "cre_skills.json",
        R"({ "Mercenary": { "skills": [ { "name": "ShieldBash", "level": 4 },
                                        { "name": "Calcify", "level": 7 } ] } })");
    CreatureCatalog cat;
    REQUIRE(LoadCreatureCatalog(p, cat));
    const CharacterDesc& d = cat.defs[static_cast<int>(CreatureId::Mercenary)];
    REQUIRE(d.skill_grant_count == 2);
    CHECK(d.skill_grants[0].skill == static_cast<int32_t>(SkillId::ShieldBash));
    CHECK(d.skill_grants[0].level == 4);   // overrides the compiled 3
    CHECK(d.skill_grants[1].skill == static_cast<int32_t>(SkillId::Calcify));
    CHECK(d.skill_grants[1].level == 7);
}

TEST_CASE("an omitted level defaults to 1, and an empty list takes skills away") {
    {
        const std::string p = write_temp(
            "cre_skills_lvl1.json", R"({ "Mercenary": { "skills": [ { "name": "ShieldBash" } ] } })");
        CreatureCatalog cat;
        REQUIRE(LoadCreatureCatalog(p, cat));
        const CharacterDesc& d = cat.defs[static_cast<int>(CreatureId::Mercenary)];
        REQUIRE(d.skill_grant_count == 1);
        CHECK(d.skill_grants[0].level == 1);
    }
    {
        // The override REPLACES rather than merges, which is the only way to
        // author a class that learns nothing.
        const std::string p =
            write_temp("cre_skills_none.json", R"({ "Mercenary": { "skills": [] } })");
        CreatureCatalog cat;
        REQUIRE(LoadCreatureCatalog(p, cat));
        CHECK(cat.defs[static_cast<int>(CreatureId::Mercenary)].skill_grant_count == 0);
    }
}

TEST_CASE("a malformed skill grant fails the load loudly") {
    CreatureCatalog fresh;
    const int32_t before = fresh.defs[static_cast<int>(CreatureId::Mercenary)].skill_grant_count;
    for (const char* body : {R"({ "Mercenary": { "skills": [ { "name": "Nope" } ] } })",
                             R"({ "Mercenary": { "skills": [ { "level": 3 } ] } })",
                             R"({ "Mercenary": { "skills": [ { "name": "ShieldBash",
                                                               "level": 0 } ] } })",
                             R"({ "Mercenary": { "skills": "ShieldBash" } })"}) {
        const std::string p = write_temp("cre_skills_bad.json", body);
        CreatureCatalog cat;
        CHECK_FALSE(LoadCreatureCatalog(p, cat));
        CHECK(cat.defs[static_cast<int>(CreatureId::Mercenary)].skill_grant_count == before);
    }
}
