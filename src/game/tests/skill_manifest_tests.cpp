// Skill template manifest: designer-authored skill data as JSON. Mirrors
// factors_manifest_tests -- pure CPU, all cases load a test-local inline
// fixture (never the shipped assets/skills/skills.json -- tests do not
// assert on shipped data files), except "missing file", whose path is
// relative, so WORKING_DIRECTORY is still the repo root (CMakeLists.txt).

#include "game/skill_manifest.hpp"

#include <catch_amalgamated.hpp>

#include <cstdio>
#include <fstream>
#include <string>

using badlands::SkillAttackTest;
using badlands::SkillCatalog;
using badlands::SkillId;
using badlands::SkillTargetMode;
using badlands::SkillTrigger;

namespace {

// Writes a temp manifest and removes it on scope exit (same mechanism as
// factors_manifest_tests' TempManifest).
struct TempManifest {
    std::string path;
    explicit TempManifest(const std::string& body)
        : path(std::string(std::tmpnam(nullptr)) + ".json") {
        std::ofstream(path) << body;
    }
    ~TempManifest() { std::remove(path.c_str()); }
};

}  // namespace

TEST_CASE("partial override keeps unspecified fields at compiled defaults") {
    TempManifest m(R"({"Calcify": {"cooldown": 5, "effect": "New text."}})");
    SkillCatalog cat;
    REQUIRE(badlands::LoadSkillCatalog(m.path, cat));
    const auto& c = cat.specs[static_cast<size_t>(SkillId::Calcify)];
    CHECK(c.cooldown_seconds == 5.0f);
    CHECK(c.effect == "New text.");
    CHECK(c.trigger == SkillTrigger::Action);         // untouched default
    CHECK(c.target == SkillTargetMode::SelfOnly);     // untouched default
    CHECK(c.intention_duration_seconds == 0.0f);      // untouched default
}

TEST_CASE("the whole engine-checked vocabulary parses") {
    // Every field the engine validates a cast against, including the ones it
    // does not execute yet (passive/multi) -- those are DECLARED, so they must
    // load; refusing them is the cast path's job, not the loader's.
    TempManifest m(R"({"ShieldBash": {"trigger": "intention", "target": "multi",
                                      "target_limit": 3, "cooldown": 9,
                                      "intention_duration": 2.5,
                                      "attack_test": "ranged",
                                      "constants": {"stun_seconds": 4.5, "radius": 2}}})");
    SkillCatalog cat;
    REQUIRE(badlands::LoadSkillCatalog(m.path, cat));
    const auto& c = cat.specs[static_cast<size_t>(SkillId::ShieldBash)];
    CHECK(c.trigger == SkillTrigger::Intention);
    CHECK(c.target == SkillTargetMode::Multi);
    CHECK(c.target_limit == 3);
    CHECK(c.cooldown_seconds == 9.0f);
    CHECK(c.intention_duration_seconds == 2.5f);
    CHECK(c.attack_test == SkillAttackTest::Ranged);
    CHECK(c.constant("stun_seconds") == 4.5f);   // overrides the compiled 3
    CHECK(c.constant("radius") == 2.0f);         // a name the defaults never had
}

TEST_CASE("a bad constant fails the whole load") {
    SkillCatalog cat;
    const float before = cat.specs[static_cast<size_t>(SkillId::ShieldBash)]
                             .constant("stun_seconds");
    {
        TempManifest m(R"({"ShieldBash": {"constants": {"stun_seconds": "soon"}}})");
        CHECK_FALSE(badlands::LoadSkillCatalog(m.path, cat));
    }
    {
        TempManifest m(R"({"ShieldBash": {"constants": 3}})");
        CHECK_FALSE(badlands::LoadSkillCatalog(m.path, cat));
    }
    CHECK(cat.specs[static_cast<size_t>(SkillId::ShieldBash)].constant("stun_seconds") ==
          before);
}

TEST_CASE("unknown skill names, bad choices, and wrong types fail loudly") {
    SkillCatalog cat;
    const float default_cd = cat.specs[0].cooldown_seconds;
    {
        TempManifest m(R"({"NotASkill": {"cooldown": 5}})");
        CHECK_FALSE(badlands::LoadSkillCatalog(m.path, cat));
    }
    {
        TempManifest m(R"({"Calcify": {"trigger": "sometimes"}})");
        CHECK_FALSE(badlands::LoadSkillCatalog(m.path, cat));
    }
    {
        TempManifest m(R"({"Calcify": {"cooldown": "fast"}})");
        CHECK_FALSE(badlands::LoadSkillCatalog(m.path, cat));
    }
    CHECK(cat.specs[0].cooldown_seconds == default_cd);  // out untouched on failure
}

TEST_CASE("missing file returns false and leaves the catalog untouched") {
    SkillCatalog cat;
    CHECK_FALSE(badlands::LoadSkillCatalog("build/definitely_absent.json", cat));
    CHECK(cat.specs[0].cooldown_seconds == 45.0f);
}

TEST_CASE("a full record (cooldown + effect text together) parses correctly") {
    // Finding 6 review fix: tests never assert on shipped data files (this
    // case used to load the real assets/skills/skills.json and pin its
    // content) -- an inline fixture exercises the same "cooldown and effect
    // set together" shape without coupling the test to designer-authored
    // data.
    TempManifest m(
        R"({"Calcify": {"cooldown": 20, "effect": "Absorbs the next physical strike, then shatters."}})");
    SkillCatalog cat;
    REQUIRE(badlands::LoadSkillCatalog(m.path, cat));
    const auto& c = cat.specs[static_cast<size_t>(SkillId::Calcify)];
    CHECK(c.cooldown_seconds == 20.0f);
    CHECK(c.effect == "Absorbs the next physical strike, then shatters.");
}
