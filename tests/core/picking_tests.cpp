#include <doctest.h>

#include <array>
#include <cmath>
#include <string>

#include <shapeshifter/ShapeshifterCore.h>

#include "camera.h"
#include "picking.h"
#include "scene.h"

using namespace sq;

namespace {

// Parameters are `const`: doctest's CHECK() binds the compared sub-expression
// to a reference, and Clang only allows that for *const* accesses of
// ext_vector_type components (e.g. `simd_float3.x`) — matches the pattern
// already used in lines_tests.cpp/camera_tests.cpp.
void check_float3_approx(const simd_float3 actual, const simd_float3 expected) {
    CHECK(actual.x == doctest::Approx(expected.x));
    CHECK(actual.y == doctest::Approx(expected.y));
    CHECK(actual.z == doctest::Approx(expected.z));
}

} // namespace

// --- ray_unit_sphere ---------------------------------------------------

TEST_CASE("ray_unit_sphere: head-on hit through the near pole") {
    // a=dot(d,d)=1, b=2*dot(o,d)=2*(2*-1)=-4, c=dot(o,o)-0.25=4-0.25=3.75.
    // disc=16-15=1, sqrt=1; roots (4-1)/2=1.5 and (4+1)/2=2.5 -> smallest >
    // kEps is 1.5.
    const auto hit = ray_unit_sphere(Ray{{0.0f, 0.0f, 2.0f}, {0.0f, 0.0f, -1.0f}});
    REQUIRE(hit.has_value());
    CHECK(hit->t == doctest::Approx(1.5f));
    check_float3_approx(hit->point, simd_float3{0.0f, 0.0f, 0.5f});
    check_float3_approx(hit->normal, simd_float3{0.0f, 0.0f, 1.0f});
}

TEST_CASE("ray_unit_sphere: just-outside miss, tangent-silhouette grazing hit") {
    // origin.x=0.6 is outside the radius-0.5 silhouette of a ray running
    // parallel to -z: a=1, b=2*dot(o,d)=-4, c=(0.36+4)-0.25=4.11,
    // disc=16-16.44=-0.44 < 0 -> no real roots.
    CHECK_FALSE(ray_unit_sphere(Ray{{0.6f, 0.0f, 2.0f}, {0.0f, 0.0f, -1.0f}}).has_value());

    // origin.x=0.5 sits exactly on the silhouette: a=1, b=2*dot(o,d)=-4,
    // c=(0.25+4)-0.25=4.0, disc=16-16=0 (grazing, still counts as a hit).
    // Double root t=(4-0)/2=2 -> point (0.5,0,0), normal +x.
    const auto graze = ray_unit_sphere(Ray{{0.5f, 0.0f, 2.0f}, {0.0f, 0.0f, -1.0f}});
    REQUIRE(graze.has_value());
    CHECK(graze->t == doctest::Approx(2.0f));
    check_float3_approx(graze->point, simd_float3{0.5f, 0.0f, 0.0f});
    check_float3_approx(graze->normal, simd_float3{1.0f, 0.0f, 0.0f});
}

TEST_CASE("ray_unit_sphere: origin inside the sphere takes the exit root") {
    // a=1, b=2*dot(o,d)=0, c=0-0.25=-0.25, disc=0+1=1, sqrt=1;
    // roots (0-1)/2=-0.5 and (0+1)/2=0.5 -> the negative (entry, behind the
    // ray) root is <= kEps, so the exit root 0.5 wins.
    const auto hit = ray_unit_sphere(Ray{{0.0f, 0.0f, 0.0f}, {0.0f, 0.0f, 1.0f}});
    REQUIRE(hit.has_value());
    CHECK(hit->t == doctest::Approx(0.5f));
    check_float3_approx(hit->point, simd_float3{0.0f, 0.0f, 0.5f});
    check_float3_approx(hit->normal, simd_float3{0.0f, 0.0f, 1.0f});
}

// --- ray_unit_cube -------------------------------------------------------

TEST_CASE("ray_unit_cube: face hits from all 6 axis directions") {
    struct Case {
        simd_float3 origin;
        simd_float3 dir;
        simd_float3 normal;
        simd_float3 point;
    };
    // Every case starts 2 units out along one axis aimed straight in; it
    // crosses that axis's near face (at +-0.5) after traveling 2-0.5=1.5
    // units, with the outward normal opposing the direction of travel.
    const std::array<Case, 6> cases = {{
        {{0.0f, 0.0f, 2.0f}, {0.0f, 0.0f, -1.0f}, {0.0f, 0.0f, 1.0f}, {0.0f, 0.0f, 0.5f}},
        {{0.0f, 0.0f, -2.0f}, {0.0f, 0.0f, 1.0f}, {0.0f, 0.0f, -1.0f}, {0.0f, 0.0f, -0.5f}},
        {{2.0f, 0.0f, 0.0f}, {-1.0f, 0.0f, 0.0f}, {1.0f, 0.0f, 0.0f}, {0.5f, 0.0f, 0.0f}},
        {{-2.0f, 0.0f, 0.0f}, {1.0f, 0.0f, 0.0f}, {-1.0f, 0.0f, 0.0f}, {-0.5f, 0.0f, 0.0f}},
        {{0.0f, 2.0f, 0.0f}, {0.0f, -1.0f, 0.0f}, {0.0f, 1.0f, 0.0f}, {0.0f, 0.5f, 0.0f}},
        {{0.0f, -2.0f, 0.0f}, {0.0f, 1.0f, 0.0f}, {0.0f, -1.0f, 0.0f}, {0.0f, -0.5f, 0.0f}},
    }};

    for (size_t i = 0; i < cases.size(); ++i) {
        CAPTURE(i);
        const Case& c = cases[i];
        const auto hit = ray_unit_cube(Ray{c.origin, c.dir});
        REQUIRE(hit.has_value());
        CHECK(hit->t == doctest::Approx(1.5f));
        check_float3_approx(hit->point, c.point);
        check_float3_approx(hit->normal, c.normal);
    }
}

TEST_CASE("ray_unit_cube: angled entry hits the correct face") {
    // o=(2,0.25,0.25), d=normalize(-1,0,0)=(-1,0,0). x-axis: t1=(-0.5-2)/-1=2.5,
    // t2=(0.5-2)/-1=1.5 -> near=1.5, far=2.5. y/z axes are parallel and the
    // origin (0.25) is inside [-0.5,0.5], so they don't constrain the
    // interval. tmin=1.5 > kEps -> entry at x=+0.5, normal +x.
    const simd_float3 dir = simd_normalize(simd_float3{-1.0f, 0.0f, 0.0f});
    const auto hit = ray_unit_cube(Ray{{2.0f, 0.25f, 0.25f}, dir});
    REQUIRE(hit.has_value());
    CHECK(hit->t == doctest::Approx(1.5f));
    check_float3_approx(hit->point, simd_float3{0.5f, 0.25f, 0.25f});
    check_float3_approx(hit->normal, simd_float3{1.0f, 0.0f, 0.0f});
}

TEST_CASE("ray_unit_cube: parallel-to-slab ray outside that slab misses") {
    // y is exactly parallel (dir.y == 0) and origin.y=0.8 is outside
    // [-0.5,0.5], so the slab test rejects immediately regardless of x/z.
    CHECK_FALSE(ray_unit_cube(Ray{{0.0f, 0.8f, 2.0f}, {0.0f, 0.0f, -1.0f}}).has_value());
}

TEST_CASE("ray_unit_cube: origin inside the cube takes the exit face") {
    // x-axis: t1=(-0.5-0)/1=-0.5, t2=(0.5-0)/1=0.5 -> tmin=-0.5 (<=kEps),
    // tmax=0.5; y/z parallel-and-inside don't constrain. Exit at x=+0.5.
    const auto hit = ray_unit_cube(Ray{{0.0f, 0.0f, 0.0f}, {1.0f, 0.0f, 0.0f}});
    REQUIRE(hit.has_value());
    CHECK(hit->t == doctest::Approx(0.5f));
    check_float3_approx(hit->point, simd_float3{0.5f, 0.0f, 0.0f});
    check_float3_approx(hit->normal, simd_float3{1.0f, 0.0f, 0.0f});
}

// --- raycast_scene ---------------------------------------------------------

TEST_CASE("raycast_scene: non-uniform scale + translation still renormalizes to a unit world normal") {
    // world_from_local: local (x,y,z) -> (3+2x, y, z). Its inverse maps
    // world X -> local ((X-3)/2, Y, Z), so local origin (6,0,0) -> (1.5,0,0)
    // and local dir (-1,0,0,w=0) -> (-0.5,0,0) (NOT renormalized).
    // Sphere test: a=dot(d,d)=0.25, b=2*dot(o,d)=2*(1.5*-0.5)=-1.5,
    // c=dot(o,o)-0.25=2.25-0.25=2.0, disc=2.25-2.0=0.25, sqrt=0.5;
    // roots (1.5-0.5)/0.5=2.0 and (1.5+0.5)/0.5=4.0 -> t_local=2.0,
    // local point=(1.5,0,0)+2*(-0.5,0,0)=(0.5,0,0), local normal=(1,0,0).
    // World point = M*(0.5,0,0,1) = (3+2*0.5,0,0) = (4,0,0).
    // Minv's linear part is diag(0.5,1,1); its transpose is itself, so the
    // raw world normal is (0.5,0,0) before renormalizing back to unit length.
    SceneDocument doc;
    Node node;
    node.id = 7;
    node.shape = Shape::Sphere;
    node.position = {3.0f, 0.0f, 0.0f};
    node.scale = {2.0f, 1.0f, 1.0f};
    doc.add(node);

    const auto hit = raycast_scene(doc, Ray{{6.0f, 0.0f, 0.0f}, {-1.0f, 0.0f, 0.0f}});
    REQUIRE(hit.has_value());
    CHECK(hit->node_id == 7);
    check_float3_approx(hit->hit.point, simd_float3{4.0f, 0.0f, 0.0f});
    CHECK(simd_length(hit->hit.normal) == doctest::Approx(1.0f));
    check_float3_approx(hit->hit.normal, simd_float3{1.0f, 0.0f, 0.0f});
}

TEST_CASE("raycast_scene: translated cube") {
    SceneDocument doc;
    Node node;
    node.id = 9;
    node.shape = Shape::Cube;
    node.position = {0.0f, 3.0f, 0.0f};
    doc.add(node);

    const auto hit = raycast_scene(doc, Ray{{0.0f, 3.0f, 5.0f}, {0.0f, 0.0f, -1.0f}});
    REQUIRE(hit.has_value());
    CHECK(hit->node_id == 9);
    check_float3_approx(hit->hit.point, simd_float3{0.0f, 3.0f, 0.5f});
}

TEST_CASE("raycast_scene: nearest node wins, miss-all is nullopt, Subtract nodes are pickable") {
    SceneDocument doc;

    Node front; // z=0
    front.id = 10;
    front.shape = Shape::Sphere;
    front.op = Op::Add;
    front.position = {0.0f, 0.0f, 0.0f};
    doc.add(front);

    Node back; // z=-3
    back.id = 20;
    back.shape = Shape::Sphere;
    back.op = Op::Subtract;
    back.position = {0.0f, 0.0f, -3.0f};
    doc.add(back);

    SUBCASE("ray down -z from z=5 hits the front sphere first") {
        const auto hit = raycast_scene(doc, Ray{{0.0f, 0.0f, 5.0f}, {0.0f, 0.0f, -1.0f}});
        REQUIRE(hit.has_value());
        CHECK(hit->node_id == 10);
    }

    SUBCASE("a ray offset far enough in xy misses both spheres") {
        CHECK_FALSE(raycast_scene(doc, Ray{{5.0f, 5.0f, 5.0f}, {0.0f, 0.0f, -1.0f}}).has_value());
    }

    SUBCASE("Subtract-op node is pickable like any other") {
        // Starting between the two spheres (z=-2) and aiming further -z only
        // reaches the back (Subtract) sphere's near face at world z=-2.5.
        const auto hit = raycast_scene(doc, Ray{{0.0f, 0.0f, -2.0f}, {0.0f, 0.0f, -1.0f}});
        REQUIRE(hit.has_value());
        CHECK(hit->node_id == 20);
    }
}

// --- Editor integration: scene built entirely through spawn() -------------

TEST_CASE("Editor: spawn/pick/select/nodeName integration, scene built entirely through spawn()") {
    Editor* editor = Editor::create();
    editor->setViewportSize(800.0f, 500.0f, 2.0f);

    // Same camera Editor::create() wires up internally (core/src/editor.cpp):
    // eye {4,3,6}, target {0,0.5,0}, up {0,1,0}, fov_y 1.0472 rad, and aspect
    // 800/500=1.6 — matching the setViewportSize call above means this is
    // exactly the camera editor->spawn()/pick() use, which is the whole
    // point of this cross-validation. The scene starts empty (no hardcoded
    // nodes as of this milestone), so every id/name below comes from spawn().
    const simd_float3 eye = {4.0f, 3.0f, 6.0f};
    const simd_float3 target = {0.0f, 0.5f, 0.0f};
    // Independent re-derivation of ray_through_view_point's direction at the
    // exact viewport center (400,250 of 800x500): there ndc_x=ndc_y=0, so
    // the ray direction collapses to the look-at forward vector, matching
    // the same pattern camera_tests.cpp uses for its own center-ray check.
    const simd_float3 dir_center = simd_normalize(target - eye);

    SUBCASE("spawning on an empty scene misses everything and lands unsnapped at the fixed distance") {
        const SpawnResult cube = editor->spawn(Shape::Cube, Op::Add, 400.0f, 250.0f);
        CHECK(cube.node_id == 1); // first spawn into a fresh, empty scene
        CHECK(cube.snapped == false);
        CHECK(editor->selectedNode() == cube.node_id);

        char buf[64];
        editor->nodeName(cube.node_id, buf, sizeof(buf));
        CHECK(std::string(buf) == "Cube 1");

        // nodeName buffer-safety, exercised here since it needs a real node
        // (the hardcoded-scene node this used to check against is gone).
        char short_buf[3] = {'X', 'X', 'X'};
        editor->nodeName(cube.node_id, short_buf, 3);
        CHECK(short_buf[0] == 'C');
        CHECK(short_buf[1] == 'u');
        CHECK(short_buf[2] == '\0');

        // kUnsnappedSpawnDistance (core/src/scene.h) == 6.0f: an unsnapped
        // spawn's position is exactly eye + dir_center * that distance (no
        // surface offset, unlike the snapped case).
        const simd_float3 expected_position = eye + dir_center * kUnsnappedSpawnDistance;

        // Verify indirectly: pick() along the same ray must now hit the
        // newly-spawned cube. Kept robust rather than brittle — assert the
        // hit is finite, in front of the camera, and near the cube's known
        // center (within a unit cube's bounding-sphere radius,
        // sqrt(3)*0.5 ~= 0.866), rather than pinning an exact face point.
        const PickResult picked = editor->pick(400.0f, 250.0f);
        CHECK(picked.node_id == cube.node_id);

        const simd_float3 point = {picked.point.x, picked.point.y, picked.point.z};
        const simd_float3 normal = {picked.normal.x, picked.normal.y, picked.normal.z};
        CHECK(std::isfinite(point.x));
        CHECK(std::isfinite(point.y));
        CHECK(std::isfinite(point.z));
        CHECK(simd_length(normal) == doctest::Approx(1.0f));

        const float t = simd_dot(point - eye, dir_center); // in front of the camera along the ray
        CHECK(t > 0.0f);
        CHECK(simd_length(point - expected_position) < 0.87f);

        SUBCASE("spawning again at the same point snaps onto the cube just placed") {
            const SpawnResult sphere = editor->spawn(Shape::Sphere, Op::Subtract, 400.0f, 250.0f);
            CHECK(sphere.node_id == 2);
            CHECK(sphere.snapped == true); // SpawnResult carries no parent id; snap_parent == cube's
                                            // id is covered at the SceneDocument level (scene_tests.cpp)
            CHECK(editor->selectedNode() == sphere.node_id); // selection moved off the cube

            editor->nodeName(sphere.node_id, buf, sizeof(buf));
            CHECK(std::string(buf) == "Sphere 1");
        }
    }

    SUBCASE("pick at the far corner misses everything") {
        const PickResult result = editor->pick(5.0f, 5.0f);
        CHECK(result.node_id == kInvalidNode);
    }

    SUBCASE("select/selectedNode round-trips, including clearing with kInvalidNode") {
        CHECK(editor->selectedNode() == kInvalidNode);
        const SpawnResult cube = editor->spawn(Shape::Cube, Op::Add, 400.0f, 250.0f);
        CHECK(editor->selectedNode() == cube.node_id); // spawn() selects the new node
        editor->select(kInvalidNode);
        CHECK(editor->selectedNode() == kInvalidNode);
    }

    SUBCASE("nodeName returns an empty string for an unknown id") {
        char buf[64];
        editor->nodeName(99, buf, sizeof(buf));
        CHECK(std::string(buf) == "");
    }
}
