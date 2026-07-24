#include "badlands_sim.hpp"
#include "components.h"
#include "game_state.h"
#include "progression.h"
#include "sim_internal.hpp"

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

TEST_CASE("award_xp levels up, grants class skills, emits one event per level") {
    auto owned = badlands::make_world(BrainDesc{});
    BadlandsGame& g = *owned;
    const uint32_t slot =
        badlands::spawn_into(g, badlands::MercenaryDesc(0.0f, 20.0f));
    entt::entity e = badlands::entity_for_slot(g, static_cast<int32_t>(slot));
    // Homeless spawn has class -1; the grant table keys on class, so set it.
    g.registry.get<badlands::HeroCharacter>(e).hero_class = badlands::HERO_APPRENTICE;
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
    CHECK(sk.ids[0] == SkillId::Calcify);

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
