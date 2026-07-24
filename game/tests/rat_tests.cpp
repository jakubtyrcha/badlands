// Rat (monster): spawns from the Sewer, attacks the nearest hostile unit, and
// falls back to gnawing the nearest targettable building (Castle/House) when no
// unit is in reach. Building health + the raze cascade are exercised here.

#include "brain.h"  // BrainKind
#include "components.h"
#include "economy.h"
#include "game_state.h"
#include "heroes.h"
#include "placement.h"
#include "sim_internal.hpp"

#include <catch_amalgamated.hpp>

#include <entt/entt.hpp>

#include <algorithm>

using namespace badlands;

namespace {

uint32_t place(BadlandsGame& g, BuildingKind kind, float x, float z) {
    Action a{ActionKind::PlaceBuilding, 0, x, z, static_cast<int32_t>(kind), 0};
    int64_t r = dispatch_into(g, a);
    return (r < 0) ? UINT32_MAX : static_cast<uint32_t>(r);
}

uint32_t spawn_rat(BadlandsGame& g, glm::vec2 pos) {
    CharacterDesc d{};
    d.archetype = Archetype::Monster;
    d.pos_x = pos.x;
    d.pos_z = pos.y;
    d.team = 1;  // hostile to the player's team 0
    d.hp = 6.0f;
    d.move_speed = 4.0f;
    d.attack_range = 1.0f;
    d.attack_damage = 3.0f;
    d.attack_cooldown = 1.0f;
    d.size_x = d.size_y = d.size_z = 0.5f;
    return spawn_into(g, d);
}

uint32_t alive_rats(const BadlandsGame& g) {
    uint32_t n = 0;
    for (auto [e, brain] : g.registry.view<const Brain>().each()) {
        if (brain.kind == BrainKind::Monster) {
            ++n;
        }
    }
    return n;
}

}  // namespace

TEST_CASE("a rat targets the nearest hostile unit") {
    auto owned = make_flat_world();
    BadlandsGame& g = *owned;
    CharacterDesc merc = MercenaryDesc(6.0f, 0.0f);  // team 0
    uint32_t hid = spawn_into(g, merc);
    uint32_t rid = spawn_rat(g, {8.0f, 0.0f});       // team 1, right beside the hero

    entt::entity rat = g.slots[rid];
    entt::entity hero = g.slots[hid];
    const float hero_hp0 = g.registry.get<Health>(hero).hp;

    tick_world(g, 1.0f / 30.0f);

    // The rat chases the hero (a unit), not a building.
    CHECK(g.registry.get<MoveTarget>(rat).kind == MoveTarget::Kind::Entity);

    for (int i = 0; i < 60; ++i) {
        tick_world(g, 1.0f / 30.0f);
    }
    // Combat happened -- someone lost health (hero hit the rat and/or vice versa).
    const bool rat_dead = (entity_for_slot(g, static_cast<int32_t>(rid)) == entt::null);
    CHECK((g.registry.get<Health>(hero).hp < hero_hp0 || rat_dead));
}

TEST_CASE("with no units in reach, a rat gnaws the nearest building down") {
    auto owned = make_flat_world();
    BadlandsGame& g = *owned;
    // A House (enemy_targettable) with low health so the raze is quick.
    uint32_t house = place(g, BuildingKind::House, 20.0f, 0.0f);
    REQUIRE(house != UINT32_MAX);
    g.placement.buildings[house].hp = 9.0f;  // 3 rat swings (3 dmg each)

    glm::vec2 door;
    REQUIRE(building_approach_tile(g.placement, g.placement.buildings[house], door));
    spawn_rat(g, door);  // right at the wall; no units anywhere

    const float hp0 = g.placement.buildings[house].hp;
    bool razed = false;
    for (int i = 0; i < 400; ++i) {
        tick_world(g, 1.0f / 30.0f);
        if (!g.placement.buildings[house].alive) {
            razed = true;
            break;
        }
    }
    CHECK(razed);                                       // chewed all the way down
    CHECK(g.placement.buildings[house].hp < hp0);       // took damage on the way
    CHECK_FALSE(g.placement.buildings[house].alive);    // tombstoned via the cascade
}

TEST_CASE("a rat prefers a hostile unit over a building") {
    auto owned = make_flat_world();
    BadlandsGame& g = *owned;
    place(g, BuildingKind::House, 20.0f, 0.0f);  // a gnawable building nearby
    spawn_into(g, MercenaryDesc(6.0f, 0.0f));    // but a hero is closer
    uint32_t rid = spawn_rat(g, {8.0f, 0.0f});

    tick_world(g, 1.0f / 30.0f);
    // Unit takes priority: the rat chases the hero (Entity), not the House door.
    CHECK(g.registry.get<MoveTarget>(g.slots[rid]).kind == MoveTarget::Kind::Entity);
}

TEST_CASE("the spawner emits rats from a Sewer, capped at max_alive") {
    auto owned = make_world(BrainDesc{});
    BadlandsGame& g = *owned;
    // Place a Sewer directly (poppable, but PlaceBuilding accepts the kind).
    uint32_t sewer = place(g, BuildingKind::Sewer, 30.0f, 30.0f);
    REQUIRE(sewer != UINT32_MAX);

    SimFactors f = g.factors;
    f.monster.spawn_interval_millis = 1000;
    f.monster.max_alive = 2;
    set_factors_of(g, f);

    const int64_t interval_ticks = f.monster.spawn_interval_millis / kMillisPerTick;
    uint32_t peak = 0;
    for (int64_t i = 0; i < interval_ticks * 5; ++i) {
        tick_world(g, 1.0f / 30.0f);
        peak = std::max(peak, alive_rats(g));
    }
    CHECK(peak >= 1);            // rats crawled out
    CHECK(peak <= 2);            // never past the cap
}
