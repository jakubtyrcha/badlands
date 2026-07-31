#include "badlands_sim.hpp"
#include "components.h"
#include "game_state.h"
#include "progression.h"
#include "sim_internal.hpp"
#include "skills.h"
#include "vision.h"

#include <catch_amalgamated.hpp>

using badlands::BrainDesc;
using badlands::GameEvent;
using badlands::GameEventKind;
using badlands::ProgressionFactors;
using badlands::SkillId;

TEST_CASE("xp_to_next follows floor(base * level^exponent)") {
    ProgressionFactors p;  // defaults 100 / 1.6 (the design table)
    CHECK(badlands::xp_to_next(p, 1) == 100);
    CHECK(badlands::xp_to_next(p, 2) == 303);
    CHECK(badlands::xp_to_next(p, 3) == 579);
    CHECK(badlands::xp_to_next(p, 4) == 918);
}

TEST_CASE("award_xp levels up, grants the desc's skills, emits one event per level") {
    auto owned = badlands::make_world(BrainDesc{});
    BadlandsGame& g = *owned;
    // A catalog mercenary: its desc carries the grant list (ShieldBash at 3),
    // so nothing has to be patched onto the entity after the fact -- which is
    // the whole point of grants riding on the desc.
    const uint32_t slot = badlands::spawn_creature_into(
        g, badlands::CreatureId::Mercenary, badlands::kPlayerTeam, {0.0f, 20.0f});
    entt::entity e = badlands::entity_for_slot(g, static_cast<int32_t>(slot));
    auto& sim = g.registry.get<badlands::HeroSimulationState>(e);
    REQUIRE(sim.level == 1);
    REQUIRE(sim.xp == 0);

    badlands::award_xp(g, slot, 150);   // crosses L1 (cost 100), 50 left over
    CHECK(sim.level == 2);
    CHECK(sim.xp == 50);

    badlands::award_xp(g, slot, 1750);  // 303 + 579 + 918 - 50: lands exactly at L5
    CHECK(sim.level == 5);
    CHECK(sim.xp == 0);

    const auto& sk = g.registry.get<badlands::Skills>(e);
    REQUIRE(sk.count == 1);
    CHECK(sk.ids[0] == SkillId::ShieldBash);  // granted on reaching level 3

    int leveled = 0;
    for (const GameEvent& ev : g.events) {
        if (ev.kind == GameEventKind::HeroLeveledUp && ev.actor_id == slot) {
            ++leveled;
        }
    }
    CHECK(leveled == 4);  // 2, 3, 4, 5
}

TEST_CASE("award_xp is a no-op for non-heroes and non-positive amounts") {
    auto owned = badlands::make_world(BrainDesc{});
    BadlandsGame& g = *owned;
    const uint32_t rat = badlands::spawn_creature_into(
        g, badlands::CreatureId::Rat, 1, {0.0f, 0.0f});
    badlands::award_xp(g, rat, 100);          // not a hero: ignored
    badlands::award_xp(g, 9999u, 100);        // invalid slot: ignored
    const uint32_t h = badlands::spawn_into(g, badlands::MercenaryDesc(0.0f, 20.0f));
    badlands::award_xp(g, h, 0);
    badlands::award_xp(g, h, -5);
    entt::entity e = badlands::entity_for_slot(g, static_cast<int32_t>(h));
    CHECK(g.registry.get<badlands::HeroSimulationState>(e).xp == 0);
}

TEST_CASE("spawn_creature_into stamps the catalog class before spawn-time grants") {
    // The catalog defs are the single source of truth for a hero creature's
    // class (Fix 1a) -- assert that FIRST, since it is what spawn_entity's
    // spawn-time grant_skills_for_level call now reads.
    const auto& catalog = badlands::DefaultCreatureCatalog();
    CHECK(catalog.defs[static_cast<int>(badlands::CreatureId::Mercenary)].hero_class ==
          badlands::HERO_MERCENARY);
    CHECK(catalog.defs[static_cast<int>(badlands::CreatureId::Hunter)].hero_class ==
          badlands::HERO_HUNTER);
    CHECK(catalog.defs[static_cast<int>(badlands::CreatureId::GraveRobber)].hero_class ==
          badlands::HERO_GRAVE_ROBBER);
    CHECK(catalog.defs[static_cast<int>(badlands::CreatureId::Apprentice)].hero_class ==
          badlands::HERO_APPRENTICE);

    struct Row {
        badlands::CreatureId id;
        int32_t hero_class;
    };
    const Row rows[] = {
        {badlands::CreatureId::Mercenary, badlands::HERO_MERCENARY},
        {badlands::CreatureId::Hunter, badlands::HERO_HUNTER},
        {badlands::CreatureId::GraveRobber, badlands::HERO_GRAVE_ROBBER},
        {badlands::CreatureId::Apprentice, badlands::HERO_APPRENTICE},
    };
    auto owned = badlands::make_world(BrainDesc{});
    BadlandsGame& g = *owned;
    for (const Row& row : rows) {
        const uint32_t slot =
            badlands::spawn_creature_into(g, row.id, badlands::kPlayerTeam, {0.0f, 0.0f});
        entt::entity e = badlands::entity_for_slot(g, static_cast<int32_t>(slot));
        CHECK(g.registry.get<badlands::HeroCharacter>(e).hero_class == row.hero_class);

        // The spawn-time grant (heroes.cpp) applies the DESC's own level-1
        // rows -- and copies the whole list onto the entity so the level-up
        // hook can apply the rest later.
        badlands::SkillGrants grants{};
        const badlands::CharacterDesc& desc =
            badlands::DefaultCreatureCatalog().defs[static_cast<int>(row.id)];
        grants.count = desc.skill_grant_count;
        for (int32_t i = 0; i < grants.count; ++i) {
            grants.rows[i] = desc.skill_grants[i];
        }
        CHECK(g.registry.get<badlands::SkillGrants>(e).count == grants.count);
        badlands::Skills expected{};
        badlands::grant_skills_for_level(expected, grants, 1);
        const auto& sk = g.registry.get<badlands::Skills>(e);
        REQUIRE(sk.count == expected.count);
        for (int32_t i = 0; i < expected.count; ++i) {
            CHECK(sk.ids[i] == expected.ids[i]);
        }
    }
}

namespace {
constexpr float kTickDt = 1.0f / 30.0f;

int32_t xp_of(BadlandsGame& g, uint32_t slot) {
    entt::entity e = badlands::entity_for_slot(g, static_cast<int32_t>(slot));
    return g.registry.get<badlands::HeroSimulationState>(e).xp;
}
}  // namespace

TEST_CASE("a monster's xp_reward splits evenly (round up) over nearby heroes") {
    auto owned = badlands::make_flat_world();
    BadlandsGame& g = *owned;
    badlands::CharacterDesc hero = badlands::MercenaryDesc(0.0f, 0.0f);
    const uint32_t h1 = badlands::spawn_into(g, hero);
    hero.pos_x = 2.0f;
    const uint32_t h2 = badlands::spawn_into(g, hero);
    hero.pos_x = 50.0f;  // far outside the default 10u radius
    const uint32_t far = badlands::spawn_into(g, hero);

    const uint32_t rat =
        badlands::spawn_creature_into(g, badlands::CreatureId::Rat, 1, {1.0f, 0.0f});
    g.registry.get<badlands::Health>(
        badlands::entity_for_slot(g, static_cast<int32_t>(rat))).hp = 0.0f;
    badlands::tick_world(g, kTickDt);  // death sweep spreads the reward

    CHECK(xp_of(g, h1) == 5);  // ceil(10 / 2)
    CHECK(xp_of(g, h2) == 5);
    CHECK(xp_of(g, far) == 0);
}

TEST_CASE("heroes hidden inside buildings get no kill XP; alone gets it all") {
    auto owned = badlands::make_flat_world();
    BadlandsGame& g = *owned;
    badlands::CharacterDesc hero = badlands::MercenaryDesc(0.0f, 0.0f);
    const uint32_t outside = badlands::spawn_into(g, hero);
    hero.pos_x = 1.0f;
    const uint32_t hidden = badlands::spawn_into(g, hero);
    g.registry.emplace<badlands::InsideBuilding>(
        badlands::entity_for_slot(g, static_cast<int32_t>(hidden)), 0, 0);

    const uint32_t rat =
        badlands::spawn_creature_into(g, badlands::CreatureId::Rat, 1, {0.5f, 0.0f});
    g.registry.get<badlands::Health>(
        badlands::entity_for_slot(g, static_cast<int32_t>(rat))).hp = 0.0f;
    badlands::tick_world(g, kTickDt);

    CHECK(xp_of(g, outside) == 10);  // whole reward: the only eligible hero
    CHECK(xp_of(g, hidden) == 0);
}

namespace {
int discovered_texels(const badlands::VisionGrid& vg) {
    int n = 0;
    for (int k = 0; k < vg.nx * vg.nz; ++k) {
        n += vg.front[2 * k] ? 1 : 0;
    }
    return n;
}
}  // namespace

TEST_CASE("newly discovered texels award xp_per_texel to the discovering hero") {
    // Empty flat world: no colony, so every discovered texel is hero-stamped.
    auto owned = badlands::make_world(
        badlands::BrainDesc{},
        badlands::WorldConfig{.prebuild_colony = false, .terrain_blocking = false});
    BadlandsGame& g = *owned;
    // A radius-6 full circle discovers ~113 texels, over the default level-1
    // cost (100) -- decouple this test from Task 3's (separately-tested)
    // level-up rollover so it only exercises discovery crediting.
    g.factors.progression.level_base_xp = 1'000'000;
    badlands::configure_vision(g.vision, -32.0f, -32.0f, 64.0f, 64.0f, 1.0f);

    badlands::CharacterDesc d = badlands::MercenaryDesc(0.0f, 0.0f);
    d.vision_radius = 6.0f;
    d.vision_cone_half_angle_deg = 180.0f;  // full circle
    const uint32_t slot = badlands::spawn_into(g, d);

    badlands::tick_world(g, kTickDt);
    const int total = discovered_texels(g.vision);
    REQUIRE(total > 0);
    CHECK(xp_of(g, slot) == total * g.factors.progression.xp_per_texel);

    // The invariant holds tick over tick (the hero may roam and reveal more):
    badlands::tick_world(g, kTickDt);
    CHECK(xp_of(g, slot) ==
          discovered_texels(g.vision) * g.factors.progression.xp_per_texel);
}

TEST_CASE("overlapping discoveries are credited exactly once (union, no double)") {
    auto owned = badlands::make_world(
        badlands::BrainDesc{},
        badlands::WorldConfig{.prebuild_colony = false, .terrain_blocking = false});
    BadlandsGame& g = *owned;
    // See the level-up decoupling note above: the first hero's circle alone
    // exceeds the level-1 cost.
    g.factors.progression.level_base_xp = 1'000'000;
    badlands::configure_vision(g.vision, -32.0f, -32.0f, 64.0f, 64.0f, 1.0f);

    badlands::CharacterDesc d = badlands::MercenaryDesc(0.0f, 0.0f);
    d.vision_radius = 6.0f;
    d.vision_cone_half_angle_deg = 180.0f;
    const uint32_t a = badlands::spawn_into(g, d);
    d.pos_x = 4.0f;  // overlapping circles
    const uint32_t b = badlands::spawn_into(g, d);

    badlands::tick_world(g, kTickDt);
    CHECK(xp_of(g, a) + xp_of(g, b) == discovered_texels(g.vision));
    CHECK(xp_of(g, a) > 0);
    CHECK(xp_of(g, b) > 0);
}

TEST_CASE("oversized exploration awards saturate instead of wrapping") {
    auto owned = badlands::make_world(
        badlands::BrainDesc{},
        badlands::WorldConfig{.prebuild_colony = false, .terrain_blocking = false});
    BadlandsGame& g = *owned;
    // A per-texel reward this large times a radius-6 circle's ~113 texels
    // overflows int32 (Fix 1b) -- award_xp must saturate the sum in int64
    // rather than wrap.
    g.factors.progression.xp_per_texel = 20'000'000;
    g.factors.progression.level_base_xp = 1'000'000'000;
    badlands::configure_vision(g.vision, -32.0f, -32.0f, 64.0f, 64.0f, 1.0f);

    badlands::CharacterDesc d = badlands::MercenaryDesc(0.0f, 0.0f);
    d.vision_radius = 6.0f;
    d.vision_cone_half_angle_deg = 180.0f;  // full circle
    const uint32_t slot = badlands::spawn_into(g, d);

    badlands::tick_world(g, kTickDt);

    // cost(1) = level_base_xp = 1e9, consumed on the way to level 2; cost(2)
    // (~3.03e9) saturates to INT32_MAX, so the level-up loop stops there.
    CHECK(xp_of(g, slot) == INT32_MAX - 1'000'000'000);
    entt::entity e = badlands::entity_for_slot(g, static_cast<int32_t>(slot));
    CHECK(g.registry.get<badlands::HeroSimulationState>(e).level == 2);
}

TEST_CASE("xp accumulation saturates at INT32_MAX") {
    auto owned = badlands::make_world(BrainDesc{});
    BadlandsGame& g = *owned;
    g.factors.progression.level_base_xp = 1'000'000'000;

    const uint32_t slot = badlands::spawn_into(g, badlands::MercenaryDesc(0.0f, 20.0f));
    entt::entity e = badlands::entity_for_slot(g, static_cast<int32_t>(slot));
    auto& sim = g.registry.get<badlands::HeroSimulationState>(e);
    sim.level = 2;
    sim.xp = INT32_MAX - 10;

    badlands::award_xp(g, slot, 100);  // would wrap negative pre-fix

    // The sum saturates to exactly INT32_MAX -- and so does cost(2) (~3.03e9,
    // over xp_to_next's own 2e9 saturation threshold): both sides hit the same
    // sentinel, so the level-up loop's `>=` (deliberately inclusive elsewhere,
    // e.g. an exact-cost crossing) fires once more and drains xp to 0. The
    // property this test guards is still upheld: no wraparound, ever.
    CHECK(sim.xp == 0);
    CHECK(sim.level == 3);
}

TEST_CASE("award_xp is bounded by kMaxHeroLevel even under a degenerate zero-cost curve") {
    // Finding 3: level_base_xp=1, level_exponent=0 is a legal (if silly)
    // config -- xp_to_next(level) = floor(1 * level^0) = 1 for every level,
    // forever. Without a cap, one big saturated award loops the level-up
    // while() roughly 2^31 times. The cap must stop the loop long before
    // that, and xp is left saturating (not consumed further) once capped.
    auto owned = badlands::make_world(BrainDesc{});
    BadlandsGame& g = *owned;
    g.factors.progression.level_base_xp = 1;
    g.factors.progression.level_exponent = 0.0f;

    const uint32_t slot = badlands::spawn_into(g, badlands::MercenaryDesc(0.0f, 20.0f));
    entt::entity e = badlands::entity_for_slot(g, static_cast<int32_t>(slot));
    auto& sim = g.registry.get<badlands::HeroSimulationState>(e);
    REQUIRE(sim.level == 1);

    badlands::award_xp(g, slot, 100'000);

    CHECK(sim.level == badlands::kMaxHeroLevel);
}

TEST_CASE("the snapshot carries level/xp/skills; zeroed for non-heroes") {
    auto owned = badlands::make_world(badlands::BrainDesc{});
    BadlandsGame& g = *owned;
    const uint32_t h = badlands::spawn_creature_into(
        g, badlands::CreatureId::Mercenary, badlands::kPlayerTeam, {0.0f, 20.0f});
    badlands::award_xp(g, h, 2000);  // past level 5: ShieldBash granted at 3
    const uint32_t rat =
        badlands::spawn_creature_into(g, badlands::CreatureId::Rat, 1, {5.0f, 0.0f});

    const auto rows = badlands::characters_of(g);
    const badlands::CharacterState* hero_row = nullptr;
    const badlands::CharacterState* rat_row = nullptr;
    for (const auto& r : rows) {
        if (r.id == h) hero_row = &r;
        if (r.id == rat) rat_row = &r;
    }
    REQUIRE(hero_row != nullptr);
    REQUIRE(rat_row != nullptr);
    CHECK(hero_row->level == 5);
    CHECK(hero_row->xp == 100);  // 2000 - 1900 (100+303+579+918)
    CHECK(hero_row->xp_next ==
          badlands::xp_to_next(g.factors.progression, hero_row->level));
    REQUIRE(hero_row->skill_count == 1);
    CHECK(hero_row->skills[0] == static_cast<int32_t>(SkillId::ShieldBash));
    CHECK(rat_row->level == 0);
    CHECK(rat_row->skill_count == 0);
}

// --- spawning at a level -----------------------------------------------------
// A hero can start life partway up the ladder. The rule is that it looks
// exactly like one that EARNED its way there: stats recomputed from the growth
// row, and every grant row up to and including its level applied. Nothing here
// is a shortcut past award_xp -- it is the same two operations award_xp's loop
// performs, run once per level instead of once per XP threshold.

TEST_CASE("a hero spawned at a level has that level's stats", "[progression][spawn]") {
    auto owned = badlands::make_world(BrainDesc{});
    BadlandsGame& g = *owned;

    const uint32_t base_slot = badlands::spawn_creature_into(
        g, badlands::CreatureId::Mercenary, badlands::kPlayerTeam, {0.0f, 20.0f}, 1);
    const uint32_t high_slot = badlands::spawn_creature_into(
        g, badlands::CreatureId::Mercenary, badlands::kPlayerTeam, {8.0f, 20.0f}, 5);
    entt::entity lo = badlands::entity_for_slot(g, static_cast<int32_t>(base_slot));
    entt::entity hi = badlands::entity_for_slot(g, static_cast<int32_t>(high_slot));

    CHECK(g.registry.get<badlands::HeroSimulationState>(hi).level == 5);
    // Starts fresh toward the NEXT level rather than carrying phantom progress:
    // xp_next is derived from the level in the snapshot, so the level is the
    // only thing there is to stamp.
    CHECK(g.registry.get<badlands::HeroSimulationState>(hi).xp == 0);

    // Recomputed from the entity's OWN rows rather than compared against an
    // authored number -- the growth table is tuning and may move.
    const badlands::BaseStats& base = g.registry.get<badlands::BaseStats>(hi);
    const badlands::StatGrowth& grow = g.registry.get<badlands::Growth>(hi).rows;
    CHECK(g.registry.get<badlands::Health>(hi).max_hp ==
          Catch::Approx(base.hp + grow.hp * 4.0f));
    CHECK(g.registry.get<badlands::Combatant>(hi).armour ==
          Catch::Approx(base.armour + grow.armour * 4.0f));
    // ...and the level-1 twin is untouched, so the argument did the work.
    CHECK(g.registry.get<badlands::Health>(lo).max_hp == Catch::Approx(base.hp));
}

TEST_CASE("a hero spawned at a level knows every skill up to it", "[progression][spawn]") {
    // The grant ladder is the point: rows fire at their EXACT level, so a
    // spawn at N has to replay 1..N or it arrives missing everything it should
    // have learned on the way. Test-local grant rows, never the shipped catalog.
    auto owned = badlands::make_world(BrainDesc{});
    BadlandsGame& g = *owned;

    badlands::CharacterDesc d{};
    d.archetype = badlands::Archetype::Hero;
    d.hero_class = 0;
    d.hp = 20.0f;
    d.move_speed = 2.0f;
    d.size_x = d.size_y = d.size_z = 1.0f;
    d.team = badlands::kPlayerTeam;
    d.skill_grants[0] = {static_cast<int32_t>(SkillId::Curse), 1};
    d.skill_grants[1] = {static_cast<int32_t>(SkillId::ShieldBash), 3};
    d.skill_grants[2] = {static_cast<int32_t>(SkillId::Backstab), 5};
    d.skill_grant_count = 3;

    auto learned_at = [&](int32_t level) {
        badlands::CharacterDesc copy = d;
        copy.pos_x = static_cast<float>(level) * 4.0f;
        copy.pos_z = 30.0f;
        const uint32_t slot = badlands::spawn_into(g, copy, level);
        entt::entity e = badlands::entity_for_slot(g, static_cast<int32_t>(slot));
        return g.registry.get<badlands::Skills>(e).count;
    };
    CHECK(learned_at(1) == 1);
    CHECK(learned_at(2) == 1);
    CHECK(learned_at(3) == 2);
    CHECK(learned_at(4) == 2);
    CHECK(learned_at(5) == 3);
}

TEST_CASE("set_hero_level is idempotent", "[progression]") {
    // Recompute-from-base, never accumulate: calling it twice at one level must
    // land on the identical float, or a replay could drift from a live run.
    auto owned = badlands::make_world(BrainDesc{});
    BadlandsGame& g = *owned;
    const uint32_t slot = badlands::spawn_creature_into(
        g, badlands::CreatureId::Hunter, badlands::kPlayerTeam, {0.0f, 20.0f}, 6);
    entt::entity e = badlands::entity_for_slot(g, static_cast<int32_t>(slot));

    const float hp = g.registry.get<badlands::Health>(e).max_hp;
    const int32_t skills = g.registry.get<badlands::Skills>(e).count;
    badlands::set_hero_level(g, e, 6);
    CHECK(g.registry.get<badlands::Health>(e).max_hp == hp);
    CHECK(g.registry.get<badlands::Skills>(e).count == skills);
}

TEST_CASE("spawning a monster at a level changes nothing", "[progression][spawn]") {
    // Monsters carry a zeroed growth row and a threat anchor authored at level
    // 1 -- "level" is not a thing they have, so the argument is ignored rather
    // than quietly inventing a stronger rat.
    auto owned = badlands::make_world(BrainDesc{});
    BadlandsGame& g = *owned;
    const uint32_t a =
        badlands::spawn_creature_into(g, badlands::CreatureId::Rat, 1, {5.0f, 0.0f}, 1);
    const uint32_t b =
        badlands::spawn_creature_into(g, badlands::CreatureId::Rat, 1, {9.0f, 0.0f}, 8);
    entt::entity ea = badlands::entity_for_slot(g, static_cast<int32_t>(a));
    entt::entity eb = badlands::entity_for_slot(g, static_cast<int32_t>(b));
    CHECK(g.registry.get<badlands::Health>(ea).max_hp ==
          g.registry.get<badlands::Health>(eb).max_hp);
    CHECK(!g.registry.all_of<badlands::HeroSimulationState>(eb));
}
