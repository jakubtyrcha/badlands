#include "badlands_sim.hpp"
#include "components.h"
#include "skills.h"
#include "behaviours/world_view.h"

#include <catch_amalgamated.hpp>

#include <limits>
#include <string>

using badlands::SkillId;
using badlands::Skills;

TEST_CASE("skill catalog is dense and named") {
    REQUIRE(badlands::SkillDefs().size() == static_cast<size_t>(badlands::kSkillCount));
    CHECK(badlands::SkillDefs()[0].id == SkillId::Calcify);
    CHECK(badlands::SkillDefs()[1].id == SkillId::ShieldBash);
    CHECK(std::string(badlands::SkillName(0)) == "Calcify");
    CHECK(std::string(badlands::SkillName(1)) == "ShieldBash");
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

TEST_CASE("grants fire at their exact level, once") {
    badlands::SkillGrants g{};
    g.rows[0] = {static_cast<int32_t>(SkillId::ShieldBash), 3};
    g.count = 1;

    Skills s{};
    badlands::grant_skills_for_level(s, g, 2);
    CHECK(s.count == 0);
    badlands::grant_skills_for_level(s, g, 4);
    CHECK(s.count == 0);  // a level PAST the grant does not teach it either
    badlands::grant_skills_for_level(s, g, 3);
    REQUIRE(s.count == 1);
    CHECK(s.ids[0] == SkillId::ShieldBash);
    CHECK(s.cooldown_remaining[0] == 0.0f);

    badlands::grant_skills_for_level(s, g, 3);  // replayed level: dupe-proof
    CHECK(s.count == 1);
}

TEST_CASE("an out-of-range grant row is ignored rather than trusted") {
    badlands::SkillGrants g{};
    g.rows[0] = {badlands::kSkillCount + 7, 1};
    g.rows[1] = {-1, 1};
    g.count = 2;
    Skills s{};
    badlands::grant_skills_for_level(s, g, 1);
    CHECK(s.count == 0);
}

TEST_CASE("the creature catalog carries the shipped grant lists") {
    const badlands::CreatureCatalog& cat = badlands::DefaultCreatureCatalog();
    const badlands::CharacterDesc& merc =
        cat.defs[static_cast<int>(badlands::CreatureId::Mercenary)];
    REQUIRE(merc.skill_grant_count == 1);
    CHECK(merc.skill_grants[0].skill == static_cast<int32_t>(SkillId::ShieldBash));
    CHECK(merc.skill_grants[0].level == 3);

    const badlands::CharacterDesc& app =
        cat.defs[static_cast<int>(badlands::CreatureId::Apprentice)];
    REQUIRE(app.skill_grant_count == 3);
    CHECK(app.skill_grants[0].skill == static_cast<int32_t>(SkillId::Curse));
    CHECK(app.skill_grants[0].level == 1);
    // Calcify moved 5 -> 4, per the design document.
    CHECK(app.skill_grants[1].skill == static_cast<int32_t>(SkillId::Calcify));
    CHECK(app.skill_grants[1].level == 4);
    CHECK(app.skill_grants[2].skill == static_cast<int32_t>(SkillId::Teleport));
    CHECK(app.skill_grants[2].level == 8);

    const badlands::CharacterDesc& hunter =
        cat.defs[static_cast<int>(badlands::CreatureId::Hunter)];
    // Also two at one level: with Skin Game deferred (nothing in this sim can
    // be skinned) the hunter would otherwise learn exactly one thing, ever.
    REQUIRE(hunter.skill_grant_count == 2);
    CHECK(hunter.skill_grants[0].skill == static_cast<int32_t>(SkillId::DressWounds));
    CHECK(hunter.skill_grants[0].level == 2);
    CHECK(hunter.skill_grants[1].skill == static_cast<int32_t>(SkillId::PrecisionShot));
    CHECK(hunter.skill_grants[1].level == 2);

    // Two rows at the same level: the approach and the payoff are one tool,
    // and grant_skills_for_level applies every row that matches, not the first.
    const badlands::CharacterDesc& gr =
        cat.defs[static_cast<int>(badlands::CreatureId::GraveRobber)];
    REQUIRE(gr.skill_grant_count == 3);
    CHECK(gr.skill_grants[0].skill == static_cast<int32_t>(SkillId::Sneak));
    CHECK(gr.skill_grants[0].level == 3);
    CHECK(gr.skill_grants[2].skill == static_cast<int32_t>(SkillId::PrecisionShot));
    CHECK(gr.skill_grants[2].level == 5);
    CHECK(gr.skill_grants[1].skill == static_cast<int32_t>(SkillId::Backstab));
    CHECK(gr.skill_grants[1].level == 3);
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
    CHECK(c.trigger == badlands::SkillTrigger::Action);
    CHECK(c.target == badlands::SkillTargetMode::SelfOnly);
    CHECK(c.attack_test == badlands::SkillAttackTest::None);
    CHECK(c.intention_duration_seconds == 0.0f);
    // A gap, not a rotation: the ward's 30 s must not cover its own cooldown,
    // or the armour is permanent (code review, finding 6).
    CHECK(c.cooldown_seconds == 45.0f);
    CHECK(c.cooldown_seconds > c.constant("duration_seconds", 0.0f));
    CHECK(c.effect == "Hardens the skin to stone; blows land, and glance.");
    // Its constant is the DURATION; how much armour it is worth belongs to the
    // status (combat.cpp), not to whatever applied it.
    CHECK(c.constant("duration_seconds", 0.0f) == 30.0f);
}

TEST_CASE("ShieldBash template: an action, at an enemy, with a melee test") {
    badlands::SkillCatalog cat;
    const badlands::SkillSpec& s =
        cat.specs[static_cast<size_t>(SkillId::ShieldBash)];
    CHECK(s.trigger == badlands::SkillTrigger::Action);
    CHECK(s.target == badlands::SkillTargetMode::Any);
    CHECK(s.target_limit == 1);
    CHECK(s.attack_test == badlands::SkillAttackTest::Melee);
    CHECK(s.cooldown_seconds == 12.0f);
    CHECK(s.intention_duration_seconds == 0.0f);
    CHECK(s.constant("stun_seconds") == Catch::Approx(3.0f));
    CHECK(s.constant("nope", -1.0f) == Catch::Approx(-1.0f));
    CHECK(s.constant("nope") == Catch::Approx(0.0f));  // default fallback
}

TEST_CASE("SkillIdFromName round-trips catalog names") {
    CHECK(badlands::SkillIdFromName("Calcify") == SkillId::Calcify);
    CHECK(badlands::SkillIdFromName("ShieldBash") == SkillId::ShieldBash);
    CHECK(badlands::SkillIdFromName("NotASkill") == badlands::SkillId::Count);
}

TEST_CASE("SetSkillCatalog sanitizes an adversarial catalog") {
    badlands::Sim sim{badlands::BrainDesc{}};
    badlands::SkillCatalog cat;
    cat.specs[0].intention_duration_seconds = -3.0f;
    cat.specs[0].cooldown_seconds = -20.0f;
    cat.specs[0].target_limit = 0;                       // < 1 is meaningless
    cat.specs[0].constants[0].name = "broken";
    cat.specs[0].constants[0].value = std::numeric_limits<float>::quiet_NaN();
    cat.specs[0].constant_count = 1;
    sim.SetSkillCatalog(cat);
    CHECK(sim.Skills().specs[0].intention_duration_seconds == 0.0f);
    CHECK(sim.Skills().specs[0].cooldown_seconds == 0.0f);
    CHECK(sim.Skills().specs[0].target_limit == 1);
    CHECK(sim.Skills().specs[0].constant("broken") == 0.0f);   // non-finite -> 0
}
