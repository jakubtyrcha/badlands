// Exploration: choosing somewhere unknown to go, and finding out the world
// will not let you get there.
//
// Two independent contracts:
//   1. the PICKER -- a pure query over a hand-drawn vision grid. Tested without
//      a sim, so the algorithm can be replaced (contour tracing, region scoring)
//      behind the same signature and these still pass.
//   2. the BLOCKED EVENT -- the world refuses a step, the character stops, and
//      the brain reacts by abandoning the goal. This is the seam that decouples
//      "where the AI decided to go" from "where a character can actually get",
//      and it must keep working unchanged when a real terrain navmesh replaces
//      the stand-in walkability rule.

#include "behaviours/blocks.h"
#include "behaviours/selectors.h"
#include "behaviours/world_view.h"

#include "command.h"
#include "components.h"
#include "exploration.h"
#include "game_state.h"
#include "heroes.h"
#include "movement.h"
#include "placement.h"
#include "sim_internal.hpp"
#include "town_brain.h"
#include "vision.h"

#include <catch_amalgamated.hpp>

#include <entt/entt.hpp>

using namespace badlands;

namespace {

// A vision grid with a discovered disc of `radius` texels about the centre --
// a colony that has explored its immediate surroundings and nothing beyond.
VisionGrid discovered_disc(int n, float radius) {
    VisionGrid g;
    configure_vision(g, -static_cast<float>(n) * 0.5f, -static_cast<float>(n) * 0.5f,
                     static_cast<float>(n), static_cast<float>(n), 1.0f);
    REQUIRE(g.configured());
    for (int j = 0; j < g.nz; ++j) {
        for (int i = 0; i < g.nx; ++i) {
            const glm::vec2 c = g.texel_center(i, j);
            if (glm::length(c) <= radius) {
                g.front[2 * (static_cast<size_t>(j) * g.nx + i)] = 255;  // discovered
            }
        }
    }
    return g;
}

HeroFactors picker_factors() {
    SimFactors f;
    f.hero.explore_min_distance = 4.0f;
    f.hero.explore_max_distance = 8.0f;
    f.hero.explore_search_radius = 200.0f;
    return f.hero;
}

}  // namespace

// --- 1. the picker ----------------------------------------------------------

TEST_CASE("the picker aims past the edge of what is known") {
    // The essential property: wherever it sends you, you end up somewhere
    // nobody has been -- otherwise it is not exploration.
    const VisionGrid g = discovered_disc(120, 20.0f);
    const HeroFactors f = picker_factors();

    for (uint32_t seed = 1; seed <= 40; ++seed) {
        const auto target = pick_exploration_target(g, {0.0f, 0.0f}, seed, f);
        REQUIRE(target.has_value());
        INFO("seed " << seed << " -> (" << target->x << ", " << target->y << ")");
        // Beyond the discovered disc, by at least the minimum push.
        CHECK(glm::length(*target) > 20.0f);
        // ...and not absurdly beyond it: the frontier is at radius 20, and the
        // push is bounded, so the target stays in the neighbourhood.
        CHECK(glm::length(*target) < 20.0f + f.explore_max_distance + 2.0f);
    }
}

TEST_CASE("the picker is deterministic in its seed") {
    // What keeps a run reproducible and its command log replayable.
    const VisionGrid g = discovered_disc(120, 20.0f);
    const HeroFactors f = picker_factors();

    const auto a = pick_exploration_target(g, {0.0f, 0.0f}, 12345, f);
    REQUIRE(a.has_value());
    for (int i = 0; i < 8; ++i) {
        const auto again = pick_exploration_target(g, {0.0f, 0.0f}, 12345, f);
        REQUIRE(again.has_value());
        CHECK(again->x == a->x);
        CHECK(again->y == a->y);
    }
}

TEST_CASE("different seeds strike out in different directions") {
    // Two heroes setting off must not walk the same line, or exploring a map
    // takes as long as exploring it with one hero.
    const VisionGrid g = discovered_disc(120, 20.0f);
    const HeroFactors f = picker_factors();

    int distinct = 0;
    const auto first = pick_exploration_target(g, {0.0f, 0.0f}, 1, f);
    REQUIRE(first.has_value());
    for (uint32_t seed = 2; seed <= 20; ++seed) {
        const auto t = pick_exploration_target(g, {0.0f, 0.0f}, seed, f);
        REQUIRE(t.has_value());
        if (glm::distance(*t, *first) > 1.0f) {
            ++distinct;
        }
    }
    CHECK(distinct > 10);  // a spread, not a single direction
}

TEST_CASE("an unconfigured grid means there is nothing to explore") {
    // Graceful degradation: with no fog of war there is no notion of unknown
    // ground, so the activity simply never applies (rather than misbehaving).
    const VisionGrid empty;
    CHECK_FALSE(pick_exploration_target(empty, {0.0f, 0.0f}, 1, picker_factors()).has_value());
}

TEST_CASE("a fully explored map has no frontier left") {
    VisionGrid g = discovered_disc(40, 1000.0f);  // everything discovered
    CHECK_FALSE(pick_exploration_target(g, {0.0f, 0.0f}, 1, picker_factors()).has_value());
}

TEST_CASE("a fully UNexplored map has no outline to stand on") {
    // The frontier is discovered ground bordering the unknown; with nothing
    // discovered there is no outline, and nowhere to set out FROM.
    VisionGrid g;
    configure_vision(g, -20.0f, -20.0f, 40.0f, 40.0f, 1.0f);
    CHECK_FALSE(pick_exploration_target(g, {0.0f, 0.0f}, 1, picker_factors()).has_value());
}

TEST_CASE("the map edge is not mistaken for unexplored territory") {
    // Off the map is not "unknown" -- if it were, explorers would pile into the
    // world bound forever. A grid discovered right out to its border has no
    // frontier at all.
    VisionGrid g = discovered_disc(30, 1000.0f);
    CHECK_FALSE(pick_exploration_target(g, {0.0f, 0.0f}, 1, picker_factors()).has_value());
}

TEST_CASE("the search radius bounds how far afield a hero will look") {
    const VisionGrid g = discovered_disc(200, 20.0f);
    HeroFactors f = picker_factors();

    // Standing far from the frontier, with a short search radius: nothing worth
    // setting out for.
    f.explore_search_radius = 5.0f;
    CHECK_FALSE(pick_exploration_target(g, {90.0f, 90.0f}, 1, f).has_value());

    // Widen it and the same frontier comes into consideration.
    f.explore_search_radius = 300.0f;
    CHECK(pick_exploration_target(g, {90.0f, 90.0f}, 1, f).has_value());
}

// --- 2. the Explore block ---------------------------------------------------

TEST_CASE("Explore outranks every filler activity when it applies") {
    // It sits in the Productive band, so this is structural rather than a
    // matter of weights -- which is exactly why its vetoes carry the restraint.
    const SimFactors f;
    WorldView v;
    v.has_explore_goal = true;
    v.explore_goal = {50.0f, 50.0f};
    v.has_home = true;
    v.boredom = 1.0f;
    v.has_tavern = true;  // would otherwise be off to the tavern

    const BehaviourResult r =
        select_banded(hero_activities(), f.hero.weights[HERO_MERCENARY], v, f);
    CHECK(r.id == ActivityId::Explore);
    CHECK(r.target.x == 50.0f);
}

TEST_CASE("Explore stands down rather than being out-prioritized") {
    const SimFactors f;
    WorldView v;
    v.has_explore_goal = true;
    v.explore_goal = {50.0f, 50.0f};
    REQUIRE(score_explore(v, f) > 0.0f);

    // Too tired to go wandering off.
    v.fatigue = f.hero.explore_max_fatigue;
    CHECK(score_explore(v, f) == 0.0f);
    v.fatigue = 0.0f;

    // The world already refused this one.
    v.move_blocked = true;
    CHECK(score_explore(v, f) == 0.0f);
    v.move_blocked = false;

    // Something worth stopping for is right here.
    v.has_prey = true;
    CHECK(score_explore(v, f) == 0.0f);
    v.has_prey = false;

    // Nowhere unknown within reach (or simply not in the mood this window).
    v.has_explore_goal = false;
    CHECK(score_explore(v, f) == 0.0f);
}

TEST_CASE("a tired hero abandons exploring for rest") {
    // Preemption by a higher need, which is what makes exploration a
    // non-critical errand rather than a commitment.
    const SimFactors f;
    WorldView v;
    v.has_explore_goal = true;
    v.explore_goal = {50.0f, 50.0f};
    v.has_home = true;

    CHECK(select_banded(hero_activities(), f.hero.weights[HERO_MERCENARY], v, f).id ==
          ActivityId::Explore);

    v.fatigue = 0.7f;  // past explore_max_fatigue AND past fatigue_go_home
    CHECK(select_banded(hero_activities(), f.hero.weights[HERO_MERCENARY], v, f).id ==
          ActivityId::GoHome);

    v.fatigue = 1.0f;  // and exhaustion pre-empts from the Danger band
    CHECK(select_banded(hero_activities(), f.hero.weights[HERO_MERCENARY], v, f).id ==
          ActivityId::RestUrgent);
}

TEST_CASE("class appetite for exploring is heavily skewed") {
    // "Hunters do it naturally, everyone else rarely" -- a frequency, which a
    // weight cannot express while Explore is alone in its band.
    const SimFactors f;
    CHECK(f.hero.explore_chance[HERO_HUNTER] > 0.5f);
    for (int32_t c : {HERO_MERCENARY, HERO_GRAVE_ROBBER, HERO_APPRENTICE}) {
        INFO("class " << HeroClassName(static_cast<HeroClassId>(c)));
        CHECK(f.hero.explore_chance[c] < 0.2f);
        CHECK(f.hero.explore_chance[c] > 0.0f);  // rarely, not never
    }
}

// --- 3. walkability and the blocked event -----------------------------------

TEST_CASE("open water is what stops a character, for now") {
    CHECK_FALSE(is_walkable(mapgen::Biome::Lake));
    CHECK(is_walkable(mapgen::Biome::Plains));
    CHECK(is_walkable(mapgen::Biome::Forest));
    CHECK(is_walkable(mapgen::Biome::Hills));
}

namespace {

// Walks a unit toward `goal` through the movement pipeline alone. Driving
// plan/follow directly (as movement_tests does) keeps the brain out of it --
// otherwise the town brain re-decides a goal of its own every tick and the test
// would be about arbitration rather than about walkability.
void walk_toward(BadlandsGame& g, entt::entity e, glm::vec2 goal, int ticks) {
    MoveTarget& mt = g.registry.get<MoveTarget>(e);
    mt.kind = MoveTarget::Kind::Point;
    mt.point = goal;
    mt.stop_distance = 0.1f;
    for (int i = 0; i < ticks; ++i) {
        plan_paths(g, 1.0f / 30.0f);
        follow_paths(g, 1.0f / 30.0f);
    }
}

}  // namespace

TEST_CASE("a character told to walk into water stops and raises the event") {
    // The contract the AI is built on: the decision is allowed to be
    // impossible, and finding out is an EVENT rather than a precondition.
    auto owned = make_world(nullptr);
    BadlandsGame& g = *owned;
    g.world_millis = 5000;  // so the event's stamp is distinguishable from zero

    // On the plains south of the central lake, aimed at open water.
    const uint32_t slot = spawn_into(g, MercenaryDesc(0.0f, kCastleSpawnZ));
    const entt::entity e = g.slots[slot];
    REQUIRE(is_walkable(biome_at(g, g.registry.get<Position>(e).pos)));

    walk_toward(g, e, {0.0f, 0.0f}, 400);

    // It got as far as the water and no further.
    const glm::vec2 stopped = g.registry.get<Position>(e).pos;
    CHECK(is_walkable(biome_at(g, stopped)));
    CHECK(glm::distance(stopped, glm::vec2{0.0f, 0.0f}) > 1.0f);  // never reached the goal
    REQUIRE(g.registry.all_of<MoveBlocked>(e));
    CHECK(g.registry.get<MoveBlocked>(e).at_millis == g.world_millis);
    CHECK_FALSE(is_walkable(biome_at(g, g.registry.get<MoveBlocked>(e).point)));
}

TEST_CASE("terrain blocking can be switched off for tests about mechanics") {
    // The documented escape hatch, mirroring the pathfinder's "no provider ->
    // obstacle-oblivious" fallback.
    auto owned = make_flat_world();
    BadlandsGame& g = *owned;
    CHECK_FALSE(g.terrain_blocking);

    const uint32_t slot = spawn_into(g, MercenaryDesc(0.0f, 20.0f));
    const entt::entity e = g.slots[slot];
    walk_toward(g, e, {0.0f, 0.0f}, 400);

    CHECK(glm::distance(g.registry.get<Position>(e).pos, glm::vec2{0.0f, 0.0f}) < 1.0f);
    CHECK_FALSE(g.registry.all_of<MoveBlocked>(e));
}

TEST_CASE("a hunter actually sets off into the unknown, through the sim") {
    // End to end: fog of war configured, so there IS an unknown; the hunter
    // reveals a patch around itself, that patch acquires an outline, and the
    // hunter picks a point beyond it and walks. Nothing here is stubbed --
    // perception, the appetite draw, the picker, and the band all participate.
    auto owned = make_world(nullptr);
    BadlandsGame& g = *owned;
    configure_vision(g.vision, -128.0f, -128.0f, 256.0f, 256.0f, 1.0f);

    CharacterDesc d{};
    d.archetype = Archetype::Hero;
    d.pos_x = 0.0f;
    d.pos_z = kCastleSpawnZ;
    d.team = kPlayerTeam;  // only the player's units reveal the map
    d.hp = 30.0f;
    d.move_speed = 4.0f;
    d.attack_range = 4.0f;
    d.attack_damage = 4.0f;
    d.attack_cooldown = 1.0f;
    d.size_x = d.size_y = d.size_z = 1.0f;
    d.vision_radius = 14.0f;
    d.vision_cone_half_angle_deg = 60.0f;
    const uint32_t slot = spawn_into(g, d);
    const entt::entity e = g.slots[slot];
    g.registry.get<HeroCharacter>(e).hero_class = HERO_HUNTER;

    bool explored = false;
    glm::vec2 goal{};
    for (int i = 0; i < 600 && !explored; ++i) {
        g.registry.get<HeroSimulationState>(e).fatigue = 0.0f;  // keep rest out of it
        tick_world(g, 1.0f / 30.0f);
        if (g.registry.get<HeroSimulationState>(e).behavior ==
            static_cast<int32_t>(ActivityId::Explore)) {
            explored = true;
            goal = g.registry.get<MoveTarget>(e).point;
        }
    }
    REQUIRE(explored);

    // It is heading for ground the colony has never seen.
    const VisionLevel level = query_vision(g.vision, goal.x, goal.y, 0.0f);
    CHECK(level == VisionLevel::Unknown);
}

TEST_CASE("a blocked hero abandons exploring, then tries elsewhere next window") {
    // The full loop the user asked for: unreachable is not a dead end. The
    // refusal vetoes exploring for the window it happened in -- so the hero
    // does something else -- and stops mattering in the next one, so it strikes
    // out somewhere new rather than giving up on exploring for good.
    auto owned = make_world(nullptr);
    BadlandsGame& g = *owned;
    const int64_t lease = g.factors.hero.explore_lease_millis;

    const uint32_t slot = spawn_into(g, MercenaryDesc(0.0f, kCastleSpawnZ));
    const entt::entity e = g.slots[slot];
    g.world_millis = lease * 3;
    g.registry.emplace<MoveBlocked>(e, glm::vec2{0.0f, 0.0f}, g.world_millis);

    // A view built in the same window sees the blockage...
    WorldView v;
    v.has_explore_goal = true;
    v.move_blocked = true;
    CHECK(score_explore(v, g.factors) == 0.0f);

    // ...and one built a window later does not.
    v.move_blocked = false;
    CHECK(score_explore(v, g.factors) > 0.0f);
}
