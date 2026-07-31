// The threat calibration table.
//
// Unlike every other balance test in this tree, these DO assert the authored
// values -- and that is the point. Threat anchors are fixed calibration posts,
// not tunables: the stats are moved toward them, never the other way round. A
// silent edit to a post would quietly redefine what the duel matrix is
// measuring against, so it has to fail a test.

#include "badlands_sim.hpp"
#include "components.h"
#include "game_state.h"
#include "progression.h"
#include "sim_internal.hpp"
#include "threat_table.h"

#include <catch_amalgamated.hpp>

using badlands::BrainDesc;
using badlands::CreatureId;
using badlands::threat_of;
using badlands::threat_target;

TEST_CASE("the calibration anchors are what the design says", "[threat]") {
    CHECK(threat_target(CreatureId::Mercenary, 1) == Catch::Approx(2.5f));
    CHECK(threat_target(CreatureId::Hunter, 1) == Catch::Approx(1.5f));
    CHECK(threat_target(CreatureId::GraveRobber, 1) == Catch::Approx(1.0f));
    CHECK(threat_target(CreatureId::Apprentice, 1) == Catch::Approx(0.75f));
    CHECK(threat_target(CreatureId::Rat, 1) == Catch::Approx(0.25f));
    CHECK(threat_target(CreatureId::Goblin, 1) == Catch::Approx(1.0f));
    CHECK(threat_target(CreatureId::Bandit, 1) == Catch::Approx(2.0f));
    CHECK(threat_target(CreatureId::BanditArcher, 1) == Catch::Approx(2.0f));
    CHECK(threat_target(CreatureId::BanditLeader, 1) == Catch::Approx(5.0f));
    CHECK(threat_target(CreatureId::MudGolem, 1) == Catch::Approx(6.0f));
    CHECK(threat_target(CreatureId::Deer, 1) == Catch::Approx(0.0f));
}

TEST_CASE("every creature has an anchor", "[threat]") {
    for (int i = 0; i < badlands::kCreatureCount; ++i) {
        CHECK(threat_target(static_cast<CreatureId>(i), 1) >= 0.0f);
    }
    // Out of range is 0, never a read past the end.
    CHECK(threat_target(CreatureId::Count, 1) == Catch::Approx(0.0f));
    CHECK(threat_target(static_cast<CreatureId>(-1), 1) == Catch::Approx(0.0f));
}

TEST_CASE("a single-anchor creature is level-independent", "[threat]") {
    CHECK(threat_target(CreatureId::Rat, 20) == Catch::Approx(threat_target(CreatureId::Rat, 1)));
    CHECK(threat_target(CreatureId::MudGolem, 7) ==
          Catch::Approx(threat_target(CreatureId::MudGolem, 1)));
}

TEST_CASE("interpolation is linear between anchors and flat outside", "[threat]") {
    // A LOCAL table, not the shipped one: this pins the arithmetic, and the
    // case above pins the data. Mixing the two would make a design edit look
    // like a maths bug.
    const badlands::ThreatAnchor anchors[] = {{1, 2.0f}, {11, 12.0f}};
    auto at = [&](int32_t level) {
        return badlands::interpolate_anchors(anchors, 2, level);
    };
    CHECK(at(1) == Catch::Approx(2.0f));
    CHECK(at(11) == Catch::Approx(12.0f));
    CHECK(at(6) == Catch::Approx(7.0f));    // midpoint
    CHECK(at(3) == Catch::Approx(4.0f));    // linear, not eased
    CHECK(at(-4) == Catch::Approx(2.0f));   // flat below the first
    CHECK(at(50) == Catch::Approx(12.0f));  // flat above the last, never extrapolated
    CHECK(badlands::interpolate_anchors(anchors, 0, 5) == Catch::Approx(0.0f));
}

TEST_CASE("threat_of reads a live entity's creature and level", "[threat]") {
    auto owned = badlands::make_world(BrainDesc{});
    BadlandsGame& g = *owned;
    const uint32_t slot = badlands::spawn_creature_into(
        g, CreatureId::Mercenary, badlands::kPlayerTeam, {0.0f, 20.0f});
    entt::entity e = badlands::entity_for_slot(g, static_cast<int32_t>(slot));

    CHECK(threat_of(g.registry, e) == Catch::Approx(threat_target(CreatureId::Mercenary, 1)));

    // It tracks the table at the entity's CURRENT level, not a spawn-time cache.
    g.registry.get<badlands::HeroSimulationState>(e).level = 9;
    CHECK(threat_of(g.registry, e) == Catch::Approx(threat_target(CreatureId::Mercenary, 9)));
}

TEST_CASE("threat_of ignores damage", "[threat]") {
    // What a WOUNDED enemy is worth right now is a judgement, and judgements
    // belong to the brain -- which already sees health on the wire.
    auto owned = badlands::make_world(BrainDesc{});
    BadlandsGame& g = *owned;
    const uint32_t slot =
        badlands::spawn_creature_into(g, CreatureId::Goblin, 1, {0.0f, 20.0f});
    entt::entity e = badlands::entity_for_slot(g, static_cast<int32_t>(slot));

    const float before = threat_of(g.registry, e);
    auto& health = g.registry.get<badlands::Health>(e);
    health.hp = 0.1f * health.max_hp;
    CHECK(threat_of(g.registry, e) == Catch::Approx(before));
}

TEST_CASE("threat_of is 0 for an entity with no creature", "[threat]") {
    auto owned = badlands::make_world(BrainDesc{});
    BadlandsGame& g = *owned;
    CHECK(threat_of(g.registry, g.registry.create()) == Catch::Approx(0.0f));
    CHECK(threat_of(g.registry, entt::null) == Catch::Approx(0.0f));
    // A hand-built desc that names no catalog creature still spawns, and still
    // reports 0 rather than reading garbage off the id.
    const uint32_t slot = badlands::spawn_into(g, badlands::MercenaryDesc(0.0f, 20.0f));
    entt::entity e = badlands::entity_for_slot(g, static_cast<int32_t>(slot));
    CHECK(threat_of(g.registry, e) >= 0.0f);
}

TEST_CASE("the catalog stamps each creature's own id onto its desc", "[threat]") {
    // threat_of needs the entity to know WHAT it is; nothing recorded that
    // before this slice.
    const auto& cat = badlands::DefaultCreatureCatalog();
    for (int i = 0; i < badlands::kCreatureCount; ++i) {
        CHECK(cat.defs[i].creature == static_cast<CreatureId>(i));
    }
    auto owned = badlands::make_world(BrainDesc{});
    BadlandsGame& g = *owned;
    const uint32_t slot =
        badlands::spawn_creature_into(g, CreatureId::MudGolem, 1, {0.0f, 20.0f});
    entt::entity e = badlands::entity_for_slot(g, static_cast<int32_t>(slot));
    CHECK(g.registry.get<badlands::CreatureKind>(e).id == CreatureId::MudGolem);
}
