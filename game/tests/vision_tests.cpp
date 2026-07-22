// Fog-of-war (vision) resolve: building discs, character cones, cumulative
// discovery, and the object-bounds QueryVision. Pure CPU over the internal
// world (make_world prebuilds the Castle at the origin, vision_radius 20).

#include "components.h"
#include "game_state.h"
#include "placement.h"
#include "sim_internal.hpp"  // make_world / spawn_into
#include "vision.h"

#include <catch_amalgamated.hpp>
#include <entt/entt.hpp>
#include <glm/glm.hpp>

using namespace badlands;

namespace {

// Grid covering [-64, 64] on each axis at 1 m/texel (the colony fits inside).
void configure(BadlandsGame& g) {
    configure_vision(g.vision, -64.0f, -64.0f, 128.0f, 128.0f, 1.0f);
}

VisionLevel level_at(const BadlandsGame& g, float x, float z) {
    return query_vision(g.vision, x, z, 0.0f);
}

CharacterDesc player_char(float x, float z, float radius, float cone_half_deg) {
    CharacterDesc d{};
    d.pos_x = x;
    d.pos_z = z;
    d.team = kPlayerTeam;
    d.hp = 10.0f;
    d.move_speed = 1.0f;
    d.size_x = d.size_y = d.size_z = 1.0f;
    d.vision_radius = radius;
    d.vision_cone_half_angle_deg = cone_half_deg;  // facing defaults to +Z
    return d;
}

}  // namespace

TEST_CASE("unconfigured vision field is empty and queries Unknown") {
    auto g = make_world(nullptr);
    VisionField f = vision_field_of(g->vision);
    CHECK(f.rg == nullptr);
    CHECK(f.nx == 0);
    resolve_vision(*g);  // no-op, must not crash
    CHECK(query_vision(g->vision, 0.0f, 0.0f, 5.0f) == VisionLevel::Unknown);
}

TEST_CASE("ConfigureVision sizes the field and publishes it") {
    auto g = make_world(nullptr);
    configure(*g);
    resolve_vision(*g);
    VisionField f = vision_field_of(g->vision);
    REQUIRE(f.rg != nullptr);
    CHECK(f.nx == 128);
    CHECK(f.nz == 128);
    CHECK(f.texel_m == Catch::Approx(1.0f));
    CHECK(f.world_min_x == Catch::Approx(-64.0f));
}

TEST_CASE("a building reveals a euclidean disc from its footprint edges") {
    auto g = make_world(nullptr);  // Castle at origin: 4x4 footprint, radius 20
    configure(*g);
    resolve_vision(*g);

    // Inside the footprint, and along an axis within the radius from the edge.
    CHECK(level_at(*g, 0.0f, 0.0f) == VisionLevel::Visible);
    CHECK(level_at(*g, 0.0f, 21.0f) == VisionLevel::Visible);   // 19 m past the +Z edge
    CHECK(level_at(*g, 0.0f, 25.0f) == VisionLevel::Unknown);   // 23 m past the edge

    // Euclidean (not L1): a diagonal point just inside vs. just outside radius
    // (texel centers sit at *.5, ~19.1 m and ~21.9 m from the footprint corner).
    CHECK(level_at(*g, 15.0f, 15.0f) == VisionLevel::Visible);
    CHECK(level_at(*g, 17.0f, 17.0f) == VisionLevel::Unknown);
}

TEST_CASE("character vision is a forward cone") {
    auto g = make_world(nullptr);
    configure(*g);
    // Player scout at (30,0), far from the castle disc, facing +Z, 45deg cone.
    spawn_into(*g, player_char(30.0f, 0.0f, 10.0f, 45.0f));
    resolve_vision(*g);

    CHECK(level_at(*g, 30.0f, 5.0f) == VisionLevel::Visible);    // straight ahead
    CHECK(level_at(*g, 33.0f, 4.0f) == VisionLevel::Visible);    // within the cone
    CHECK(level_at(*g, 30.0f, -5.0f) == VisionLevel::Unknown);   // behind
    CHECK(level_at(*g, 35.0f, 1.0f) == VisionLevel::Unknown);    // beside, outside cone
    CHECK(level_at(*g, 30.0f, 20.0f) == VisionLevel::Unknown);   // ahead but beyond radius
}

TEST_CASE("a unit always sees the texel it stands on, even with a narrow cone") {
    auto g = make_world(nullptr);
    configure(*g);
    // Narrow 30deg cone facing +Z: the unit's own texel center sits off-axis and
    // would fall outside the cone, but the unit must still see its own tile.
    spawn_into(*g, player_char(30.0f, 0.0f, 10.0f, 30.0f));
    resolve_vision(*g);
    CHECK(level_at(*g, 30.0f, 0.0f) == VisionLevel::Visible);
}

TEST_CASE("QueryVision outside the grid is Unknown even next to a revealed edge") {
    auto g = make_world(nullptr);
    configure(*g);  // grid spans [-64, 64]
    // Scout near the +X edge reveals the border column; a query well outside the
    // grid must not sample that border texel (bounds miss => Unknown).
    spawn_into(*g, player_char(60.0f, 0.0f, 20.0f, 180.0f));
    resolve_vision(*g);
    CHECK(query_vision(g->vision, 60.0f, 0.0f, 0.0f) == VisionLevel::Visible);
    CHECK(query_vision(g->vision, 100.0f, 0.0f, 2.0f) == VisionLevel::Unknown);
}

TEST_CASE("enemy characters grant the player no vision") {
    auto g = make_world(nullptr);
    configure(*g);
    CharacterDesc enemy = player_char(30.0f, 0.0f, 10.0f, 180.0f);
    enemy.team = kPlayerTeam + 1;  // not the player team
    spawn_into(*g, enemy);
    resolve_vision(*g);
    CHECK(level_at(*g, 30.0f, 5.0f) == VisionLevel::Unknown);
}

TEST_CASE("discovery is cumulative: a departed source leaves dormant terrain") {
    auto g = make_world(nullptr);
    configure(*g);
    uint32_t slot = spawn_into(*g, player_char(30.0f, 0.0f, 10.0f, 180.0f));  // full circle
    resolve_vision(*g);
    REQUIRE(level_at(*g, 30.0f, 5.0f) == VisionLevel::Visible);

    // Teleport the scout across the map and re-resolve.
    entt::entity e = g->slots[slot];
    g->registry.get<Position>(e).pos = glm::vec2(-30.0f, 0.0f);
    resolve_vision(*g);

    CHECK(level_at(*g, 30.0f, 5.0f) == VisionLevel::Dormant);    // remembered, not seen
    CHECK(level_at(*g, -30.0f, 5.0f) == VisionLevel::Visible);   // now seen here
    CHECK(level_at(*g, 55.0f, 55.0f) == VisionLevel::Unknown);   // never discovered
}

TEST_CASE("QueryVision returns the highest level over the bounds radius") {
    auto g = make_world(nullptr);
    configure(*g);
    uint32_t slot = spawn_into(*g, player_char(30.0f, 0.0f, 10.0f, 180.0f));
    resolve_vision(*g);
    entt::entity e = g->slots[slot];
    g->registry.get<Position>(e).pos = glm::vec2(-30.0f, 0.0f);
    resolve_vision(*g);  // (30,0..) is now dormant, (-30,0..) visible

    // A bounds straddling the visible/unknown boundary near (-30,0) => Visible.
    CHECK(query_vision(g->vision, -22.0f, 0.0f, 5.0f) == VisionLevel::Visible);
    // Purely in the dormant region => Dormant.
    CHECK(query_vision(g->vision, 30.0f, 5.0f, 1.0f) == VisionLevel::Dormant);
    // Purely undiscovered => Unknown.
    CHECK(query_vision(g->vision, 55.0f, 55.0f, 2.0f) == VisionLevel::Unknown);
}
