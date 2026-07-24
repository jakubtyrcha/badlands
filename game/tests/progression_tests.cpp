#include "badlands_sim.hpp"
#include "components.h"
#include "game_state.h"
#include "progression.h"
#include "sim_internal.hpp"
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
