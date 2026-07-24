#include "components.h"
#include "skills.h"

#include <catch_amalgamated.hpp>

using badlands::SkillId;
using badlands::Skills;

TEST_CASE("skill catalog is dense and named") {
    REQUIRE(badlands::SkillCatalog().size() == static_cast<size_t>(badlands::kSkillCount));
    CHECK(badlands::SkillCatalog()[0].id == SkillId::Calcify);
    CHECK(std::string(badlands::SkillName(0)) == "Calcify");
    CHECK(std::string(badlands::SkillName(-1)) == "-");
    CHECK(std::string(badlands::SkillName(badlands::kSkillCount)) == "-");
}

TEST_CASE("learn_skill is dupe-proof and bounded") {
    Skills s{};
    CHECK(badlands::learn_skill(s, SkillId::Calcify));
    CHECK_FALSE(badlands::learn_skill(s, SkillId::Calcify));
    REQUIRE(s.count == 1);
    CHECK(s.ids[0] == SkillId::Calcify);
    CHECK(s.cooldown_remaining[0] == 0.0f);
}

TEST_CASE("the grant table teaches the Apprentice Calcify at level 5") {
    Skills s{};
    badlands::grant_skills_for_level(s, badlands::HERO_APPRENTICE, 4);
    CHECK(s.count == 0);
    badlands::grant_skills_for_level(s, badlands::HERO_APPRENTICE, 5);
    REQUIRE(s.count == 1);
    CHECK(s.ids[0] == SkillId::Calcify);
    Skills merc{};
    badlands::grant_skills_for_level(merc, badlands::HERO_MERCENARY, 5);
    CHECK(merc.count == 0);
}
