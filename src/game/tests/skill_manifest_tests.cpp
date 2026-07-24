// Skill template manifest: designer-authored skill data as JSON. Mirrors
// factors_manifest_tests -- pure CPU, reads the shipped file, so
// WORKING_DIRECTORY is the repo root.

#include "game/skill_manifest.hpp"

#include <catch_amalgamated.hpp>

#include <cstdio>
#include <fstream>
#include <string>

using badlands::SkillActivation;
using badlands::SkillCatalog;
using badlands::SkillId;
using badlands::SkillTargeting;

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
    CHECK(c.activation == SkillActivation::Active);   // untouched default
    CHECK(c.targeting == SkillTargeting::Direct);     // untouched default
    CHECK(c.duration_seconds == 0.0f);                // untouched default
}

TEST_CASE("enum fields parse their named choices") {
    TempManifest m(R"({"Calcify": {"activation": "passive", "targeting": "aoe", "duration": 4}})");
    SkillCatalog cat;
    REQUIRE(badlands::LoadSkillCatalog(m.path, cat));
    const auto& c = cat.specs[static_cast<size_t>(SkillId::Calcify)];
    CHECK(c.activation == SkillActivation::Passive);
    CHECK(c.targeting == SkillTargeting::Aoe);
    CHECK(c.duration_seconds == 4.0f);
}

TEST_CASE("unknown skill names, bad choices, and wrong types fail loudly") {
    SkillCatalog cat;
    const float default_cd = cat.specs[0].cooldown_seconds;
    {
        TempManifest m(R"({"NotASkill": {"cooldown": 5}})");
        CHECK_FALSE(badlands::LoadSkillCatalog(m.path, cat));
    }
    {
        TempManifest m(R"({"Calcify": {"activation": "sometimes"}})");
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
    CHECK(cat.specs[0].cooldown_seconds == 20.0f);
}

TEST_CASE("the shipped skill manifest loads") {
    SkillCatalog cat;
    REQUIRE(badlands::LoadSkillCatalog("assets/skills/skills.json", cat));
    const auto& c = cat.specs[static_cast<size_t>(SkillId::Calcify)];
    CHECK(c.cooldown_seconds == 20.0f);
    CHECK(c.effect == "Absorbs the next physical strike, then shatters.");
}
