// Level scaling: stat = base + growth * (level - 1).
//
// These assert the FORMULA against whatever the catalog authored, never the
// authored numbers themselves -- the balance rows are an approximation and are
// expected to move, so a test that pinned them would fail on every tuning pass
// while proving nothing about the mechanism. (game/tests/threat_table_tests.cpp
// is the deliberate exception: calibration anchors ARE the contract there.)

#include "badlands_sim.hpp"
#include "components.h"
#include "game_state.h"
#include "progression.h"
#include "sim_internal.hpp"

#include <catch_amalgamated.hpp>

using badlands::BrainDesc;
using badlands::CreatureId;

namespace {

entt::entity spawn(BadlandsGame& g, CreatureId id, int32_t team = badlands::kPlayerTeam) {
    const uint32_t slot = badlands::spawn_creature_into(g, id, team, {0.0f, 20.0f});
    return badlands::entity_for_slot(g, static_cast<int32_t>(slot));
}

}  // namespace

TEST_CASE("stats are base + growth * (level-1)", "[growth]") {
    auto owned = badlands::make_world(BrainDesc{});
    BadlandsGame& g = *owned;
    entt::entity e = spawn(g, CreatureId::Mercenary);

    const badlands::BaseStats base = g.registry.get<badlands::BaseStats>(e);
    const badlands::StatGrowth grow = g.registry.get<badlands::Growth>(e).rows;
    REQUIRE(base.attack_count > 0);

    for (int32_t level : {1, 3, 15}) {
        badlands::apply_level_stats(g.registry, e, level);
        const float steps = static_cast<float>(level - 1);

        CHECK(g.registry.get<badlands::Health>(e).max_hp ==
              Catch::Approx(base.hp + grow.hp * steps));
        const auto& c = g.registry.get<badlands::Combatant>(e);
        CHECK(c.armour == Catch::Approx(base.armour + grow.armour * steps));
        CHECK(c.evasion == Catch::Approx(base.evasion + grow.evasion * steps));
        CHECK(c.defense == Catch::Approx(base.defense + grow.defense * steps));
        CHECK(c.accuracy == Catch::Approx(base.accuracy + grow.accuracy * steps));
        // Damage grows as a FRACTION of its own base, so a big weapon gains
        // more per level than a small one at the same rating.
        const auto& atk = g.registry.get<badlands::Attacks>(e);
        for (int i = 0; i < atk.count; ++i) {
            CHECK(atk.defs[i].base_damage ==
                  Catch::Approx(base.attack_damage[i] * (1.0f + grow.damage_frac * steps)));
        }
    }
}

TEST_CASE("apply_level_stats is idempotent", "[growth]") {
    // Always base + growth*(level-1), never an accumulating +=, so a replay
    // that recomputes lands on identical floats rather than drifting.
    auto owned = badlands::make_world(BrainDesc{});
    BadlandsGame& g = *owned;
    entt::entity e = spawn(g, CreatureId::Hunter);

    badlands::apply_level_stats(g.registry, e, 7);
    const float max_hp = g.registry.get<badlands::Health>(e).max_hp;
    const badlands::Combatant c = g.registry.get<badlands::Combatant>(e);
    const badlands::Attacks atk = g.registry.get<badlands::Attacks>(e);

    badlands::apply_level_stats(g.registry, e, 7);
    CHECK(g.registry.get<badlands::Health>(e).max_hp == max_hp);
    CHECK(g.registry.get<badlands::Combatant>(e).armour == c.armour);
    CHECK(g.registry.get<badlands::Combatant>(e).accuracy == c.accuracy);
    REQUIRE(g.registry.get<badlands::Attacks>(e).count == atk.count);
    for (int i = 0; i < atk.count; ++i) {
        CHECK(g.registry.get<badlands::Attacks>(e).defs[i].base_damage ==
              atk.defs[i].base_damage);
    }
}

TEST_CASE("a level-up is not a free heal", "[growth]") {
    auto owned = badlands::make_world(BrainDesc{});
    BadlandsGame& g = *owned;
    entt::entity e = spawn(g, CreatureId::Mercenary);

    auto& health = g.registry.get<badlands::Health>(e);
    health.hp = 0.5f * health.max_hp;
    badlands::apply_level_stats(g.registry, e, 5);

    // Current hp rides the max: a hero at half health is still at half health
    // one level later, just with more of it.
    CHECK(health.hp / health.max_hp == Catch::Approx(0.5f));
    CHECK(health.hp <= health.max_hp);
}

TEST_CASE("a full-health hero stays full through a level-up", "[growth]") {
    auto owned = badlands::make_world(BrainDesc{});
    BadlandsGame& g = *owned;
    entt::entity e = spawn(g, CreatureId::Mercenary);
    auto& health = g.registry.get<badlands::Health>(e);
    REQUIRE(health.hp == Catch::Approx(health.max_hp));

    badlands::apply_level_stats(g.registry, e, 4);
    CHECK(health.hp == Catch::Approx(health.max_hp));
}

TEST_CASE("award_xp scales stats as it levels", "[growth][progression]") {
    auto owned = badlands::make_world(BrainDesc{});
    BadlandsGame& g = *owned;
    const uint32_t slot = badlands::spawn_creature_into(
        g, CreatureId::Mercenary, badlands::kPlayerTeam, {0.0f, 20.0f});
    entt::entity e = badlands::entity_for_slot(g, static_cast<int32_t>(slot));

    const float hp_at_1 = g.registry.get<badlands::Health>(e).max_hp;
    const float growth_hp = g.registry.get<badlands::Growth>(e).rows.hp;
    REQUIRE(growth_hp > 0.0f);  // otherwise this test proves nothing

    badlands::award_xp(g, slot, 100 + 303);  // exactly two levels
    REQUIRE(g.registry.get<badlands::HeroSimulationState>(e).level == 3);
    CHECK(g.registry.get<badlands::Health>(e).max_hp ==
          Catch::Approx(hp_at_1 + 2.0f * growth_hp));
}

TEST_CASE("monsters have zero growth", "[growth]") {
    auto owned = badlands::make_world(BrainDesc{});
    BadlandsGame& g = *owned;
    entt::entity e = spawn(g, CreatureId::Rat, 1);

    const float hp_1 = g.registry.get<badlands::Health>(e).max_hp;
    const float dmg_1 = g.registry.get<badlands::Attacks>(e).defs[0].base_damage;
    badlands::apply_level_stats(g.registry, e, 20);
    CHECK(g.registry.get<badlands::Health>(e).max_hp == Catch::Approx(hp_1));
    CHECK(g.registry.get<badlands::Attacks>(e).defs[0].base_damage == Catch::Approx(dmg_1));
}

TEST_CASE("apply_level_stats clamps a nonsense level to 1", "[growth]") {
    // Level 0 or negative would otherwise run the growth BACKWARDS and hand a
    // hero negative hp -- a spawn path that forgets to set the level must
    // produce a level-1 creature, not an invulnerable or dead one.
    auto owned = badlands::make_world(BrainDesc{});
    BadlandsGame& g = *owned;
    entt::entity e = spawn(g, CreatureId::Mercenary);
    const float base_hp = g.registry.get<badlands::BaseStats>(e).hp;

    badlands::apply_level_stats(g.registry, e, 0);
    CHECK(g.registry.get<badlands::Health>(e).max_hp == Catch::Approx(base_hp));
    badlands::apply_level_stats(g.registry, e, -5);
    CHECK(g.registry.get<badlands::Health>(e).max_hp == Catch::Approx(base_hp));
}

TEST_CASE("apply_level_stats is safe on an entity with no growth data", "[growth]") {
    auto owned = badlands::make_world(BrainDesc{});
    BadlandsGame& g = *owned;
    const entt::entity bare = g.registry.create();
    badlands::apply_level_stats(g.registry, bare, 10);  // must not assert
    CHECK_FALSE(g.registry.all_of<badlands::Health>(bare));
}
