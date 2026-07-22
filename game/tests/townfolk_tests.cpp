// Tax collector (townfolk): the sequential-with-memory archetype. It rounds the
// buildings owing tax, banks each into its carry, returns to a Castle/Watchtower
// to deposit into player gold, and despawns. Also covers the periodic spawner
// and midnight house accrual.

#include "behaviours/blocks.h"
#include "behaviours/selectors.h"
#include "behaviours/world_view.h"
#include "command.h"
#include "components.h"
#include "economy.h"
#include "game_state.h"
#include "heroes.h"
#include "placement.h"
#include "sim_internal.hpp"

#include <catch_amalgamated.hpp>

#include <entt/entt.hpp>

#include <algorithm>
#include <array>

using namespace badlands;

namespace {

uint32_t place(BadlandsGame& g, BuildingKind kind, float x, float z) {
    Action a{ActionKind::PlaceBuilding, 0, x, z, static_cast<int32_t>(kind), 0};
    int64_t r = dispatch_into(g, a);
    return (r < 0) ? UINT32_MAX : static_cast<uint32_t>(r);
}

// Spawn a tax collector directly (bypassing the interval spawner) so a test can
// drive one round without waiting a full spawn interval.
uint32_t spawn_collector(BadlandsGame& g, glm::vec2 pos, int32_t home) {
    CharacterDesc d{};
    d.archetype = Archetype::Townfolk;
    d.pos_x = pos.x;
    d.pos_z = pos.y;
    d.team = 0;
    d.hp = 12.0f;
    d.move_speed = 8.0f;  // brisk, so a round finishes within a test's tick budget
    d.attack_cooldown = 1.0f;
    d.size_x = d.size_y = d.size_z = 0.8f;
    return spawn_entity(g, d, home);
}

uint32_t alive_collectors(const BadlandsGame& g) {
    uint32_t n = 0;
    for (auto e : g.registry.view<const TaxCollectorState>()) {
        (void)e;
        ++n;
    }
    return n;
}

constexpr std::array<Candidate, 3> kTownfolkBlocks{{
    {score_visit_taxable, act_visit_taxable},
    {score_deposit, act_deposit},
    {score_idle, act_idle},
}};

}  // namespace

TEST_CASE("townfolk blocks: collect the round before depositing") {
    const SimFactors f;
    WorldView v;
    v.slot = 0;
    v.has_deposit = true;
    v.deposit_door = {0.0f, 0.0f};

    // With a taxable target present, VisitTax wins and targets that door with a
    // CollectTax follow-up carrying the building id.
    v.has_tax_target = true;
    v.tax_target_door = {5.0f, 5.0f};
    v.tax_target_id = 3;
    BehaviourResult r = select_priority(kTownfolkBlocks, v, f);
    CHECK(r.id == Behavior::VisitTax);
    CHECK(r.target.x == 5.0f);
    REQUIRE(r.follow_up.has_value());
    CHECK(r.follow_up->kind == CommandKind::CollectTax);
    CHECK(r.follow_up->target_id == 3u);

    // Round done: Deposit takes over, heading for the bank with a Deposit follow-up.
    v.has_tax_target = false;
    r = select_priority(kTownfolkBlocks, v, f);
    CHECK(r.id == Behavior::Deposit);
    REQUIRE(r.follow_up.has_value());
    CHECK(r.follow_up->kind == CommandKind::Deposit);
}

TEST_CASE("midnight accrues tax on Houses only") {
    auto owned = make_world(nullptr);
    BadlandsGame& g = *owned;
    uint32_t house = place(g, BuildingKind::House, 10.0f, 10.0f);
    uint32_t tavern = place(g, BuildingKind::Tavern, -10.0f, 10.0f);
    REQUIRE(house != UINT32_MAX);
    REQUIRE(tavern != UINT32_MAX);
    REQUIRE(g.placement.buildings[house].taxable_income == 0);

    // Step to just before the first midnight, then across it.
    g.world_millis = kMillisPerDay - kMillisPerTick;
    tick_world(g, 1.0f / 30.0f);  // crosses into day 1

    CHECK(g.placement.buildings[house].taxable_income == g.factors.townfolk.house_income_per_day);
    CHECK(g.placement.buildings[tavern].taxable_income == 0);  // only Houses accrue
}

TEST_CASE("a tax collector rounds up the tax, banks it at the castle, and despawns") {
    auto owned = make_world(nullptr);
    BadlandsGame& g = *owned;
    // make_world prebuilds the Castle at the origin (id 0).
    REQUIRE(g.placement.buildings[0].kind == static_cast<int32_t>(BuildingKind::Castle));

    // Two houses owing tax.
    uint32_t h1 = place(g, BuildingKind::House, 12.0f, 6.0f);
    uint32_t h2 = place(g, BuildingKind::House, -12.0f, 6.0f);
    REQUIRE(h1 != UINT32_MAX);
    REQUIRE(h2 != UINT32_MAX);
    g.placement.buildings[h1].taxable_income = 30;
    g.placement.buildings[h2].taxable_income = 20;

    const uint32_t gold_before = g.gold;
    glm::vec2 castle_door;
    REQUIRE(building_approach_tile(g.placement, g.placement.buildings[0], castle_door));
    uint32_t slot = spawn_collector(g, castle_door, /*home=*/0);
    REQUIRE(alive_collectors(g) == 1);

    // Run the round to completion (collector is brisk; generous budget).
    bool despawned = false;
    for (int i = 0; i < 4000; ++i) {
        tick_world(g, 1.0f / 30.0f);
        if (entity_for_slot(g, static_cast<int32_t>(slot)) == entt::null) {
            despawned = true;
            break;
        }
    }

    CHECK(despawned);                                   // deposited and left
    CHECK(g.placement.buildings[h1].taxable_income == 0);  // both houses collected
    CHECK(g.placement.buildings[h2].taxable_income == 0);
    CHECK(g.gold == gold_before + 50);                  // 30 + 20 banked to the player
    CHECK(alive_collectors(g) == 0);
}

TEST_CASE("the spawner emits a collector at the castle, capped at max_alive") {
    auto owned = make_world(nullptr);
    BadlandsGame& g = *owned;
    SimFactors f = g.factors;
    f.townfolk.spawn_interval_millis = 1000;  // spawn quickly for the test
    f.townfolk.max_alive = 1;
    set_factors_of(g, f);

    // A distant house owing tax gives the spawned collector a multi-tick round,
    // so it stays alive to be observed (with no round it would collect nothing,
    // deposit 0 at its spawn castle, and despawn the same round).
    uint32_t house = place(g, BuildingKind::House, 60.0f, 60.0f);
    REQUIRE(house != UINT32_MAX);
    g.placement.buildings[house].taxable_income = 10;

    const int64_t interval_ticks = f.townfolk.spawn_interval_millis / kMillisPerTick;
    for (int64_t i = 0; i <= interval_ticks + 2; ++i) {
        tick_world(g, 1.0f / 30.0f);
    }
    CHECK(alive_collectors(g) == 1);  // spawned at the castle, still on its round

    // Keep ticking past several more intervals: the cap holds (never 2 at once).
    uint32_t peak = alive_collectors(g);
    for (int i = 0; i < static_cast<int>(interval_ticks) * 3; ++i) {
        tick_world(g, 1.0f / 30.0f);
        peak = std::max(peak, alive_collectors(g));
    }
    CHECK(peak <= 1);
}

TEST_CASE("dying mid-round loses the carried gold") {
    // The vulnerability the round creates: a collector killed before depositing
    // never banks its carry.
    auto owned = make_world(nullptr);
    BadlandsGame& g = *owned;
    uint32_t house = place(g, BuildingKind::House, 12.0f, 6.0f);
    g.placement.buildings[house].taxable_income = 40;

    glm::vec2 door;
    REQUIRE(building_approach_tile(g.placement, g.placement.buildings[house], door));
    uint32_t slot = spawn_collector(g, door, /*home=*/0);
    entt::entity e = g.slots[slot];

    // Let it collect (it starts at the house door), then verify it is carrying.
    for (int i = 0; i < 30; ++i) {
        tick_world(g, 1.0f / 30.0f);
    }
    REQUIRE(g.registry.valid(e));
    CHECK(g.registry.get<TaxCollectorState>(e).carried_gold == 40);

    // Kill it before it reaches the castle: gold is not credited.
    const uint32_t gold_before = g.gold;
    g.registry.get<Health>(e).hp = 0.0f;
    tick_world(g, 1.0f / 30.0f);  // death pass removes it
    CHECK((entity_for_slot(g, static_cast<int32_t>(slot)) == entt::null));
    CHECK(g.gold == gold_before);  // the 40 died with it
}
