// Facing + fog-of-war-from-movement. Upstream's vision_tests cover the vision
// RESOLVE (disc/cone/cumulative); these cover the two things the game layer
// leans on: (1) a unit's facing tracks its direction of travel and is surfaced
// correctly in the snapshot (so the renderer can orient the model + draw the
// vision cone), and (2) moving a vision source reveals fresh terrain.
//
// These drive the movement/vision pipeline directly (follow_paths, resolve_
// vision, characters_of) rather than tick_world, so the entity's brain does not
// overwrite the MoveTarget we set up -- the test controls the motion.

#include "components.h"
#include "game_state.h"
#include "heroes.h"
#include "movement.h"
#include "sim_internal.hpp"
#include "vision.h"

#include <catch_amalgamated.hpp>

#include <entt/entt.hpp>
#include <glm/glm.hpp>

#include <cmath>
#include <vector>

using namespace badlands;

namespace {

// A player hero (grants vision) placed on the map, facing +Z by default.
uint32_t spawn_hero(BadlandsGame& g, glm::vec2 pos) {
    CharacterDesc d = MercenaryDesc(pos.x, pos.y);
    d.vision_radius = 14.0f;
    d.vision_cone_half_angle_deg = 60.0f;
    d.move_speed = 6.0f;
    return spawn_into(g, d);
}

const CharacterState& row_for(const std::vector<CharacterState>& rows, uint32_t id) {
    for (const CharacterState& r : rows) {
        if (r.id == id) {
            return r;
        }
    }
    FAIL("no snapshot row for id " << id);
    return rows.front();  // unreachable
}

// Walk a unit to `goal` by feeding the movement pipeline a NavPath directly
// (no plan_paths, no brain): sets one waypoint and steps follow_paths.
void walk_to(BadlandsGame& g, entt::entity e, glm::vec2 goal, int ticks) {
    NavPath& np = g.registry.get<NavPath>(e);
    np.waypoints = {goal};
    np.cursor = 0;
    for (int i = 0; i < ticks; ++i) {
        follow_paths(g, 1.0f / 30.0f);
    }
}

void configure(BadlandsGame& g) {
    configure_vision(g.vision, -64.0f, -64.0f, 128.0f, 128.0f, 1.0f);
}

int discovered_texels(const BadlandsGame& g) {
    int n = 0;
    for (size_t k = 0; k + 1 < g.vision.front.size(); k += 2) {
        if (g.vision.front[k] != 0) {
            ++n;
        }
    }
    return n;
}

}  // namespace

TEST_CASE("facing tracks the direction of travel and is surfaced in the snapshot") {
    auto owned = make_flat_world();
    BadlandsGame& g = *owned;
    uint32_t hero = spawn_hero(g, {0.0f, 0.0f});
    entt::entity e = g.slots[hero];

    // Due east (+X).
    walk_to(g, e, {40.0f, 0.0f}, 20);
    CHECK(g.registry.get<Facing>(e).dir.x == Catch::Approx(1.0f).margin(1e-3f));
    CHECK(g.registry.get<Facing>(e).dir.y == Catch::Approx(0.0f).margin(1e-3f));
    {
        const CharacterState r = row_for(characters_of(g), hero);
        CHECK(r.facing_x == Catch::Approx(1.0f).margin(1e-3f));  // snapshot matches
        CHECK(r.facing_z == Catch::Approx(0.0f).margin(1e-3f));
    }

    // Now due north (+Z): the facing must TURN to follow travel.
    const glm::vec2 pos = g.registry.get<Position>(e).pos;
    walk_to(g, e, pos + glm::vec2{0.0f, 40.0f}, 20);
    CHECK(g.registry.get<Facing>(e).dir.y == Catch::Approx(1.0f).margin(1e-3f));
    {
        const CharacterState r = row_for(characters_of(g), hero);
        CHECK(r.facing_x == Catch::Approx(0.0f).margin(1e-3f));
        CHECK(r.facing_z == Catch::Approx(1.0f).margin(1e-3f));
    }
}

TEST_CASE("facing equals the normalized displacement actually moved") {
    auto owned = make_flat_world();
    BadlandsGame& g = *owned;
    uint32_t hero = spawn_hero(g, {5.0f, -5.0f});
    entt::entity e = g.slots[hero];

    NavPath& np = g.registry.get<NavPath>(e);
    np.waypoints = {{45.0f, 35.0f}};  // up-and-to-the-right, far enough not to arrive
    np.cursor = 0;

    const glm::vec2 before = g.registry.get<Position>(e).pos;
    follow_paths(g, 1.0f / 30.0f);
    const glm::vec2 after = g.registry.get<Position>(e).pos;
    const glm::vec2 delta = after - before;
    REQUIRE(glm::length(delta) > 1e-4f);
    const glm::vec2 travel = glm::normalize(delta);

    const CharacterState r = row_for(characters_of(g), hero);
    CHECK(r.facing_x == Catch::Approx(travel.x).margin(1e-3f));
    CHECK(r.facing_z == Catch::Approx(travel.y).margin(1e-3f));
    // Snapshot facing is always unit length.
    CHECK(std::hypot(r.facing_x, r.facing_z) == Catch::Approx(1.0f).margin(1e-3f));
}

TEST_CASE("an idle (arrived) unit keeps its last facing") {
    auto owned = make_world(BrainDesc{});
    BadlandsGame& g = *owned;
    uint32_t hero = spawn_hero(g, {0.0f, 0.0f});
    entt::entity e = g.slots[hero];
    walk_to(g, e, {40.0f, 0.0f}, 20);  // now facing +X, path consumed
    const glm::vec2 faced = g.registry.get<Facing>(e).dir;

    // No waypoints left -> follow_paths leaves facing untouched.
    for (int i = 0; i < 30; ++i) {
        follow_paths(g, 1.0f / 30.0f);
    }
    CHECK(g.registry.get<Facing>(e).dir.x == Catch::Approx(faced.x));
    CHECK(g.registry.get<Facing>(e).dir.y == Catch::Approx(faced.y));
}

TEST_CASE("the snapshot exposes the hero's vision cone") {
    auto owned = make_world(BrainDesc{});
    BadlandsGame& g = *owned;
    uint32_t hero = spawn_hero(g, {0.0f, 0.0f});
    const CharacterState r = row_for(characters_of(g), hero);
    CHECK(r.vision_radius == Catch::Approx(14.0f));
    CHECK(r.vision_cone_half_angle_deg == Catch::Approx(60.0f).margin(0.5f));
}

TEST_CASE("moving a vision source reveals fresh terrain (fog-of-war follows movement)") {
    auto owned = make_world(BrainDesc{});
    BadlandsGame& g = *owned;
    configure(g);

    uint32_t hero = spawn_hero(g, {0.0f, 0.0f});
    entt::entity e = g.slots[hero];
    resolve_vision(g);
    const int before = discovered_texels(g);
    REQUIRE(before > 0);  // the hero reveals its starting cone

    // Teleport the vision source to fresh ground and resolve again: cumulative
    // discovery must grow (the map remembers where the hero has been).
    g.registry.get<Position>(e).pos = {40.0f, 40.0f};
    resolve_vision(g);
    CHECK(discovered_texels(g) > before);
}
