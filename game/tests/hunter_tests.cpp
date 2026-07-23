// Hunter (Hero, Hunter class): the town hero brain plus a Hunt block. It tracks
// the nearest deer and shoots it -- predator/prey emerging from shared blocks
// (Hunt reuses the chase/shoot primitive; the deer runs via the shared Flee
// block). Deer are NEUTRAL, so the shot is a targeted Shoot command, not the
// team-based combat pass.

#include "behaviours/blocks.h"
#include "behaviours/selectors.h"
#include "behaviours/world_view.h"
#include "command.h"
#include "components.h"
#include "game_state.h"
#include "heroes.h"
#include "placement.h"
#include "sim_internal.hpp"

#include <catch_amalgamated.hpp>

#include <entt/entt.hpp>

#include <array>

using namespace badlands;

namespace {

uint32_t place(BadlandsGame& g, BuildingKind kind, float x, float z) {
    Action a{ActionKind::PlaceBuilding, 0, x, z, static_cast<int32_t>(kind), 0};
    int64_t r = dispatch_into(g, a);
    return (r < 0) ? UINT32_MAX : static_cast<uint32_t>(r);
}

uint32_t spawn_deer(BadlandsGame& g, glm::vec2 pos) {
    CharacterDesc d{};
    d.archetype = Archetype::Critter;
    d.pos_x = pos.x;
    d.pos_z = pos.y;
    d.team = 2;
    d.hp = 8.0f;
    d.move_speed = 3.0f;
    d.attack_cooldown = 1.0f;
    d.size_x = d.size_y = d.size_z = 0.7f;
    return spawn_into(g, d);
}

// A hunter placed directly on the map (HeroCharacter with the Hunter class), so
// tests need not stand up a Hunter's Camp + recruit.
uint32_t spawn_hunter(BadlandsGame& g, glm::vec2 pos) {
    CharacterDesc d{};
    d.archetype = Archetype::Hero;
    d.pos_x = pos.x;
    d.pos_z = pos.y;
    d.team = 0;
    d.hp = 30.0f;
    d.move_speed = 4.0f;  // faster than the deer, so the chase closes
    d.attack_range = 4.0f;  // a bow: shoots from a few tiles away
    d.attack_damage = 4.0f;
    d.attack_cooldown = 1.0f;
    d.size_x = d.size_y = d.size_z = 1.0f;
    uint32_t slot = spawn_into(g, d);
    g.registry.get<HeroCharacter>(g.slots[slot]).hero_class = HERO_HUNTER;
    return slot;
}

}  // namespace

TEST_CASE("recruiting at a Hunter's Camp yields a Hunter") {
    auto owned = make_world(nullptr);
    BadlandsGame& g = *owned;
    uint32_t camp = place(g, BuildingKind::HuntersCamp, -20.0f, 20.0f);
    REQUIRE(camp != UINT32_MAX);
    Action recruit{ActionKind::RecruitHero, camp, 0.0f, 0.0f, 0, 0};
    int64_t hid = dispatch_into(g, recruit);
    REQUIRE(hid >= 0);
    CHECK(g.registry.get<HeroCharacter>(g.slots[hid]).hero_class == HERO_HUNTER);
}

TEST_CASE("Hunt scores only with prey, and shoots once in range") {
    const SimFactors f;
    WorldView v;
    v.slot = 0;
    v.pos = {0.0f, 0.0f};
    v.self_attack_range = 4.0f;

    CHECK(score_hunt(v, f) == 0.0f);  // no prey -> not applicable

    v.has_prey = true;
    v.prey_slot = 7;
    v.prey_pos = {10.0f, 0.0f};
    v.prey_dist = 10.0f;  // out of range: chase, do not shoot
    CHECK(score_hunt(v, f) > 0.0f);
    BehaviourResult r = act_hunt(v, f);
    CHECK(r.id == Behavior::Hunt);
    CHECK(r.target.x == 10.0f);
    CHECK_FALSE(r.follow_up.has_value());

    v.prey_dist = 3.0f;  // within reach -> take the shot
    r = act_hunt(v, f);
    REQUIRE(r.follow_up.has_value());
    CHECK(r.follow_up->kind == CommandKind::Shoot);
    CHECK(r.follow_up->target_id == 7u);
}

TEST_CASE("only a hunter hunts; a mercenary ignores deer") {
    auto owned = make_world(nullptr);
    BadlandsGame& g = *owned;
    uint32_t deer = spawn_deer(g, {8.0f, 0.0f});
    entt::entity de = g.slots[deer];

    // A plain mercenary right next to the deer does not hunt it.
    CharacterDesc merc = MercenaryDesc(4.0f, 0.0f);
    uint32_t mid = spawn_into(g, merc);  // Mercenary class by default
    tick_world(g, 1.0f / 30.0f);
    CHECK(g.registry.get<HeroSimulationState>(g.slots[mid]).behavior != static_cast<int32_t>(Behavior::Hunt));
    // The deer survives the mercenary's presence (it may flee, but is not shot).
    CHECK(g.registry.valid(de));
}

TEST_CASE("a hunter runs down a deer and kills it") {
    auto owned = make_world(nullptr);
    BadlandsGame& g = *owned;
    // On the plains: the map origin is the central lake, which terrain blocking
    // makes impassable, so a chase staged there would never close.
    uint32_t deer = spawn_deer(g, {6.0f, kCastleSpawnZ});
    uint32_t hunter = spawn_hunter(g, {0.0f, kCastleSpawnZ});
    entt::entity de = g.slots[deer];

    tick_world(g, 1.0f / 30.0f);
    CHECK(g.registry.get<HeroSimulationState>(g.slots[hunter]).behavior ==
          static_cast<int32_t>(Behavior::Hunt));

    bool killed = false;
    for (int i = 0; i < 600; ++i) {
        tick_world(g, 1.0f / 30.0f);
        if (!g.registry.valid(de)) {
            killed = true;
            break;
        }
    }
    CHECK(killed);  // chased down and shot dead
}

TEST_CASE("a hunter hunts while it can, and rests once spent") {
    // No tier decides this: a rested hunter with prey in sight hunts; a spent
    // one goes home even with the deer right there. The crossover is need x
    // weight -- the hunter keeps at the job until fatigue is low enough that
    // rest's urgency overtakes Hunt's flat pull. Recruit at a camp so it has a
    // home to return to.
    auto owned = make_world(nullptr);
    BadlandsGame& g = *owned;
    uint32_t camp = place(g, BuildingKind::HuntersCamp, -20.0f, 20.0f);
    REQUIRE(camp != UINT32_MAX);
    Action recruit{ActionKind::RecruitHero, camp, 0.0f, 0.0f, 0, 0};
    uint32_t hid = static_cast<uint32_t>(dispatch_into(g, recruit));
    entt::entity e = g.slots[hid];

    // Away from the camp door, or the hunter walks straight in on the first
    // tick and is hidden before the second decision happens.
    g.registry.get<Position>(e).pos = {0.0f, 0.0f};
    spawn_deer(g, {5.0f, 0.0f});          // prey right there
    g.world_millis = kMillisPerDay / 2;   // daytime
    SimFactors off = g.factors;
    off.hero.think_max_millis = 0;        // not a test about deliberation
    set_factors_of(g, off);

    // Rested: it hunts.
    g.registry.get<HeroSimulationState>(e).fatigue = 1.0f;
    tick_world(g, 1.0f / 30.0f);
    CHECK(g.registry.get<HeroSimulationState>(e).behavior ==
          static_cast<int32_t>(Behavior::Hunt));

    // Spent: rest's urgency now dominates, and it heads home despite the prey.
    g.registry.get<HeroSimulationState>(e).fatigue = 0.05f;
    tick_world(g, 1.0f / 30.0f);
    CHECK(g.registry.get<HeroSimulationState>(e).behavior ==
          static_cast<int32_t>(Behavior::GoHome));
}
