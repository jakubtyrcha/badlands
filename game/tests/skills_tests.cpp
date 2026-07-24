#include "components.h"
#include "skills.h"
#include "behaviours/world_view.h"

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

TEST_CASE("Calcify recommends on a close melee threat, gated by cooldown") {
    Skills s{};
    badlands::learn_skill(s, SkillId::Calcify);
    badlands::PerceivedThreat threats[1] = {{{1.0f, 0.0f}, 2.5f, 7u}};
    badlands::SkillContext ctx{1.0f, threats, 1};
    badlands::SkillRecommendation rec[badlands::kMaxSkills];

    REQUIRE(badlands::evaluate_skill_triggers(s, ctx, rec) == 1);
    CHECK(rec[0].id == SkillId::Calcify);
    CHECK(rec[0].ready);
    CHECK(rec[0].recommended);  // threat at 2.5 <= trigger_param 3.0

    threats[0].dist = 5.0f;  // nearest threat too far
    badlands::evaluate_skill_triggers(s, ctx, rec);
    CHECK_FALSE(rec[0].recommended);

    threats[0].dist = 2.5f;
    s.cooldown_remaining[0] = 5.0f;  // on cooldown: still recommended, not ready
    badlands::evaluate_skill_triggers(s, ctx, rec);
    CHECK(rec[0].recommended);
    CHECK_FALSE(rec[0].ready);

    badlands::SkillContext no_threats{1.0f, nullptr, 0};
    badlands::evaluate_skill_triggers(s, no_threats, rec);
    CHECK_FALSE(rec[0].recommended);
}
