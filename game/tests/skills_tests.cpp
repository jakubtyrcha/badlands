#include "badlands_sim.hpp"
#include "components.h"
#include "skills.h"
#include "behaviours/world_view.h"

#include <catch_amalgamated.hpp>

using badlands::SkillId;
using badlands::Skills;

TEST_CASE("skill catalog is dense and named") {
    REQUIRE(badlands::SkillDefs().size() == static_cast<size_t>(badlands::kSkillCount));
    CHECK(badlands::SkillDefs()[0].id == SkillId::Calcify);
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

TEST_CASE("skill template catalog carries the compiled Calcify defaults") {
    badlands::SkillCatalog cat;
    const badlands::SkillSpec& c =
        cat.specs[static_cast<size_t>(SkillId::Calcify)];
    CHECK(c.activation == badlands::SkillActivation::Active);
    CHECK(c.targeting == badlands::SkillTargeting::Direct);
    CHECK(c.duration_seconds == 0.0f);
    CHECK(c.cooldown_seconds == 20.0f);
    CHECK(c.effect == "Absorbs the next physical strike, then shatters.");
}

TEST_CASE("SkillIdFromName round-trips catalog names") {
    CHECK(badlands::SkillIdFromName("Calcify") == SkillId::Calcify);
    CHECK(badlands::SkillIdFromName("NotASkill") == badlands::SkillId::Count);
}

TEST_CASE("SetSkillCatalog clamps negative durations and cooldowns") {
    badlands::Sim sim{badlands::BrainDesc{}};
    badlands::SkillCatalog cat;
    cat.specs[0].duration_seconds = -3.0f;
    cat.specs[0].cooldown_seconds = -20.0f;
    sim.SetSkillCatalog(cat);
    CHECK(sim.Skills().specs[0].duration_seconds == 0.0f);
    CHECK(sim.Skills().specs[0].cooldown_seconds == 0.0f);
}
