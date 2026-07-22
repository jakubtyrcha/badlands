// Factors manifest: behaviour tuning as data. Mirrors biome_manifest_tests --
// pure CPU, reads the shipped file, so WORKING_DIRECTORY is the repo root.

#include "game/factors_manifest.hpp"

#include <catch_amalgamated.hpp>

#include <cstdio>
#include <fstream>
#include <string>

using namespace badlands;

namespace {

// Writes a temp manifest and removes it on scope exit.
struct TempManifest {
    std::string path;
    explicit TempManifest(const std::string& body)
        : path(std::string(std::tmpnam(nullptr)) + ".json") {
        std::ofstream(path) << body;
    }
    ~TempManifest() { std::remove(path.c_str()); }
};

}  // namespace

TEST_CASE("the shipped factors manifest loads") {
    SimFactors f;
    REQUIRE(LoadSimFactors("assets/creatures/factors.json", f));
    CHECK(f.hero.fatigue_go_home == Catch::Approx(0.6f));
    CHECK(f.hero.roam_radius == Catch::Approx(6.0f));
}

TEST_CASE("a missing file fails and leaves the defaults untouched") {
    SimFactors f;
    const SimFactors defaults;
    CHECK_FALSE(LoadSimFactors("assets/creatures/does_not_exist.json", f));
    CHECK(f.hero.fatigue_go_home == Catch::Approx(defaults.hero.fatigue_go_home));
}

TEST_CASE("a partial manifest tunes only what it mentions") {
    const SimFactors defaults;
    TempManifest m(R"({ "hero": { "roam_radius": 21.5 } })");
    SimFactors f;
    REQUIRE(LoadSimFactors(m.path, f));
    CHECK(f.hero.roam_radius == Catch::Approx(21.5f));
    CHECK(f.hero.fatigue_go_home == Catch::Approx(defaults.hero.fatigue_go_home));
}

TEST_CASE("a non-numeric value fails loudly rather than being ignored") {
    // A typo must not silently do nothing -- that is the failure mode that
    // wastes a designer's afternoon.
    TempManifest m(R"({ "hero": { "roam_radius": "twenty" } })");
    SimFactors f;
    CHECK_FALSE(LoadSimFactors(m.path, f));
}

TEST_CASE("unparseable JSON and a non-object section fail") {
    SimFactors f;
    TempManifest bad(R"({ "hero": )");
    CHECK_FALSE(LoadSimFactors(bad.path, f));
    TempManifest wrong(R"({ "hero": 3 })");
    CHECK_FALSE(LoadSimFactors(wrong.path, f));
}

TEST_CASE("a failed parse leaves the caller's factors untouched") {
    // Half-applied tuning would be worse than none: a bad file must fall back
    // to defaults, not to a mixture.
    TempManifest m(R"({ "hero": { "roam_radius": 99.0, "fatigue_night": "oops" } })");
    SimFactors f;
    const SimFactors defaults;
    CHECK_FALSE(LoadSimFactors(m.path, f));
    CHECK(f.hero.roam_radius == Catch::Approx(defaults.hero.roam_radius));
}

TEST_CASE("the shipped manifest tunes every archetype section") {
    SimFactors f;
    REQUIRE(LoadSimFactors("assets/creatures/factors.json", f));
    CHECK(f.hero.hunt_sight_radius == Catch::Approx(22.0f));
    CHECK(f.critter.flee_radius == Catch::Approx(8.0f));
    CHECK(f.townfolk.house_income_per_day == 50u);
    CHECK(f.townfolk.spawn_interval_millis == 60000);
    CHECK(f.monster.max_alive == 4);
}

TEST_CASE("a non-numeric value in any section fails loudly") {
    const SimFactors defaults;
    {
        std::string p = std::string(std::tmpnam(nullptr)) + ".json";
        std::ofstream(p) << R"({ "monster": { "max_alive": "lots" } })";
        SimFactors f;
        CHECK_FALSE(LoadSimFactors(p, f));
        CHECK(f.monster.max_alive == defaults.monster.max_alive);
        std::remove(p.c_str());
    }
}
