#include <doctest.h>

#include <array>
#include <cmath>
#include <string>

#include <shapeshifter/ShapeshifterCore.h>

#include "camera.h"
#include "picking.h"
#include "scene.h"
#include "sdf.h" // evaluate_scene_sdf -- the zero-set cross-validation below

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

// --- raycast_scene: validation for camera-navigation use -------------------
//
// resolve_focus (navigation.h) runs raycast_scene at every camera-gesture start
// and on every mouse-move that feeds the focus-preview dot, so the cases below
// pin the properties that whole path leans on. Worth recording because it is
// easy to assume otherwise in an SDF editor: raycast_scene does NOT raymarch.
// It solves analytic ray-primitive intersections per node (ray_unit_sphere is a
// quadratic, ray_unit_cube a slab test) in each node's local space; sphere
// tracing lives only in shaders/raymarch.metal, on the GPU.

TEST_CASE("raycast_scene: oblique ellipsoid normal matches the closed form") {
    // A sphere scaled (2,1,1) is the ellipsoid (x/a)^2 + (y/b)^2 + (z/c)^2 = 1
    // with a=1, b=c=0.5. The gradient of that implicit function gives the exact
    // world normal at P: normalize((Px/a^2, Py/b^2, Pz/c^2)).
    //
    // This case exists because the non-uniform test above hits ALONG the scale
    // axis, where the expected normal is trivially +x and every plausible
    // implementation agrees. Here they diverge: the correct
    // transpose(inverse(M)) normal comes out (1,2,0)/sqrt(5), while both common
    // mistakes -- the radial direction normalize(P), and the local normal
    // pushed through M's linear part -- give (2,1,0)/sqrt(5), components
    // swapped.
    constexpr float a = 1.0f, b = 0.5f, c = 0.5f;

    // A point on the ellipsoid: local (0.5cos, 0.5sin, 0) -> world (cos, 0.5sin, 0),
    // at 45 degrees so neither component can be confused for the other.
    const float k = std::sqrt(0.5f);
    const simd_float3 P = {k, 0.5f * k, 0.0f};

    const simd_float3 expected_normal =
        simd_normalize(simd_float3{P.x / (a * a), P.y / (b * b), P.z / (c * c)});

    // Approach along the outward normal. For a convex body every point of
    // P + (d-t)*n with t < d lies strictly outside the supporting plane at P,
    // so the first surface crossing is exactly P, at t == d.
    const simd_float3 origin = P + 3.0f * expected_normal;

    SceneDocument doc;
    Node node;
    node.id = 3;
    node.shape = Shape::Sphere;
    node.scale = {2.0f, 1.0f, 1.0f};
    doc.add(node);

    const auto hit = raycast_scene(doc, Ray{origin, -expected_normal});
    REQUIRE(hit.has_value());
    CHECK(hit->node_id == 3);
    CHECK(hit->hit.t == doctest::Approx(3.0f));
    check_float3_approx(hit->hit.point, P);
    check_float3_approx(hit->hit.normal, expected_normal);
    CHECK(simd_length(hit->hit.normal) == doctest::Approx(1.0f));
    // Explicit discrimination against the swapped-component answer, so a
    // regression to either wrong transform fails on an obvious comparison
    // rather than only on the approx check above.
    CHECK(hit->hit.normal.x < hit->hit.normal.y);
}

TEST_CASE("raycast_scene: world_t is a true distance and the hit lies on the query ray") {
    // world_t is derived as dot(world_point - origin, dir), a real distance
    // only because `dir` is unit (Camera::ray_through_view_point normalizes,
    // and resolve_focus feeds it those same rays). Pan and dolly scale their
    // step by this distance, so an error here surfaces as gestures moving the
    // wrong AMOUNT rather than as a visibly wrong pick.
    //
    // The related invariant, worth recording because it explains why the local
    // ray's direction is deliberately left un-renormalized after the inverse
    // transform: M*(Minv*o + t*Minv_lin*d) == o + t*d, so the local parameter
    // and the world parameter coincide. That is what lets nearest-wins compare
    // across nodes with wildly different, non-uniform scales at all.
    SceneDocument doc;

    Node squashed; // world semi-axes (0.1, 1.5, 0.5); reaches z=+0.5
    squashed.id = 1;
    squashed.shape = Shape::Sphere;
    squashed.position = {0.0f, 0.0f, 0.0f};
    squashed.scale = {0.2f, 3.0f, 1.0f};
    doc.add(squashed);

    Node stretched; // world half-extents (4, 0.25, 1); near face at z=-5
    stretched.id = 2;
    stretched.shape = Shape::Cube;
    stretched.position = {0.0f, 0.0f, -6.0f};
    stretched.scale = {8.0f, 0.5f, 2.0f};
    doc.add(stretched);

    const simd_float3 origin = {0.0f, 0.0f, 9.0f};
    const simd_float3 dir = {0.0f, 0.0f, -1.0f};

    const auto hit = raycast_scene(doc, Ray{origin, dir});
    REQUIRE(hit.has_value());
    CHECK(hit->node_id == 1); // the sphere is nearer despite the cube being far larger
    CHECK(hit->hit.t == doctest::Approx(8.5f));
    CHECK(hit->hit.t == doctest::Approx(simd_length(hit->hit.point - origin)));
    check_float3_approx(hit->hit.point, origin + hit->hit.t * dir);

    SUBCASE("removing the nearer node promotes the farther one, still with a true t") {
        doc.remove_node(1);
        const auto far_hit = raycast_scene(doc, Ray{origin, dir});
        REQUIRE(far_hit.has_value());
        CHECK(far_hit->node_id == 2);
        CHECK(far_hit->hit.t == doctest::Approx(14.0f));
        CHECK(far_hit->hit.t == doctest::Approx(simd_length(far_hit->hit.point - origin)));
        check_float3_approx(far_hit->hit.point, origin + far_hit->hit.t * dir);
    }
}

TEST_CASE("raycast_scene: an origin inside a node returns its exit face, in front of the ray") {
    // Newly reachable: dolly translates the whole rig along the eye->focus ray,
    // so the eye can end up inside geometry. Whatever comes back must stay in
    // FRONT of the camera -- a focus point behind the eye would invert the next
    // orbit rather than merely misplace it.
    SceneDocument doc;
    Node node;
    node.id = 5;
    node.shape = Shape::Sphere;
    node.scale = {4.0f, 4.0f, 4.0f}; // world radius 2
    doc.add(node);

    const simd_float3 origin = {0.0f, 0.0f, 0.0f}; // dead centre, inside
    const auto hit = raycast_scene(doc, Ray{origin, {0.0f, 0.0f, -1.0f}});
    REQUIRE(hit.has_value());
    CHECK(hit->node_id == 5);
    CHECK(hit->hit.t > 0.0f);
    CHECK(hit->hit.t == doctest::Approx(2.0f));
    check_float3_approx(hit->hit.point, simd_float3{0.0f, 0.0f, -2.0f});
}

TEST_CASE("raycast_scene round-trips against the camera: ray -> hit -> project returns the pixel") {
    // Cross-validation with no hand-computed literals: whatever
    // ray_through_view_point aims at, projecting the resulting hit must land
    // back on the pixel it was fired through. Catches sign, aspect and y-flip
    // errors that the per-function tests agree on individually -- and it is
    // precisely the property auto-pivot rests on, since a pivot that isn't
    // under the cursor makes "point at the feature and drag" silently wrong.
    Camera camera;
    camera.eye = {4.0f, 3.0f, 6.0f};
    camera.target = {0.0f, 0.5f, 0.0f};
    camera.up = {0.0f, 1.0f, 0.0f};
    camera.fov_y_radians = 1.0472f;
    camera.aspect = 1.6f;
    constexpr float w = 800.0f, h = 500.0f;

    SceneDocument doc;
    Node node;
    node.id = 11;
    node.shape = Shape::Sphere;
    node.position = camera.target;
    node.scale = {3.0f, 3.0f, 3.0f}; // world radius 1.5 -- covers the sampled pixels
    doc.add(node);

    for (const float x : {350.0f, 400.0f, 450.0f}) {
        for (const float y : {200.0f, 250.0f, 300.0f}) {
            CAPTURE(x);
            CAPTURE(y);
            const Ray ray = camera.ray_through_view_point(x, y, w, h);
            const auto hit = raycast_scene(doc, ray);
            REQUIRE(hit.has_value());

            const ViewPoint vp = camera.project(hit->hit.point, w, h);
            CHECK(vp.visible);
            CHECK(vp.x == doctest::Approx(x).epsilon(1e-3));
            CHECK(vp.y == doctest::Approx(y).epsilon(1e-3));
        }
    }
}

TEST_CASE("raycast_scene: both ends of the scale clamp hit finitely with unit normals") {
    // Editor's scale gesture clamps each component to [0.05, 50]. Those bounds
    // are what simd_inverse actually has to invert here, and a degenerate
    // inverse would poison the focus point with NaN -- which, since the pivot
    // is derived from it, breaks the camera unrecoverably rather than merely
    // failing a pick. resolve_focus rejects non-finite candidates, but the
    // cheapest place to know they cannot arise is here.
    struct Case {
        const char* label;
        simd_float3 scale;
        float origin_z;
        float expect_t;
    };
    const std::array<Case, 3> cases = {{
        {"min uniform", {0.05f, 0.05f, 0.05f}, 5.0f, 4.975f},          // world radius 0.025
        {"max uniform", {50.0f, 50.0f, 50.0f}, 50.0f, 25.0f},          // world radius 25
        {"extreme non-uniform", {0.05f, 50.0f, 0.05f}, 5.0f, 4.975f},  // needle along +y
    }};

    for (const Case& c : cases) {
        INFO("case: " << c.label); // CAPTURE on a const char* logs the pointer, not the text
        SceneDocument doc;
        Node node;
        node.id = 1;
        node.shape = Shape::Sphere;
        node.scale = c.scale;
        doc.add(node);

        const auto hit = raycast_scene(doc, Ray{{0.0f, 0.0f, c.origin_z}, {0.0f, 0.0f, -1.0f}});
        REQUIRE(hit.has_value());
        CHECK(std::isfinite(hit->hit.t));
        CHECK(hit->hit.t == doctest::Approx(c.expect_t));
        CHECK(std::isfinite(hit->hit.point.x));
        CHECK(std::isfinite(hit->hit.point.y));
        CHECK(std::isfinite(hit->hit.point.z));
        CHECK(simd_length(hit->hit.normal) == doctest::Approx(1.0f));
    }
}

TEST_CASE("raycast_scene: a rotated node is picked in its own frame") {
    // Rotation was unreachable before it became renderable, so this pins
    // behaviour raycast_scene already had rather than behaviour it gained: it
    // inverts the full world_from_local (position, rotation AND scale), so the
    // only thing that had to be true was that nothing else assumed identity.
    //
    // A 1 x 1 x 3 box (half = 0.5, 0.5, 1.5) turned 45 deg about +Y, hit by a
    // ray straight down -z through x = 0. A CUBE would be the wrong subject
    // twice over: it is symmetric under this rotation at 90 deg, and at 45 deg
    // the ray lands exactly on an edge where the slab test's normal is
    // ambiguous. The elongated box puts the same ray cleanly on the local -X
    // face instead.
    //
    // Slab test in local space (numpy, Rodrigues inverse-rotating the ray):
    //   t = 4.292893219, local hit (-0.5, 0, 0.5), local normal (-1, 0, 0)
    //   -> world hit (0, 0, 0.70710678), world normal (-0.70710678, 0, 0.70710678)
    SceneDocument doc;
    Node node;
    node.id = 1;
    node.shape = Shape::Cube;
    node.scale = {1.0f, 1.0f, 3.0f};
    node.rotation = simd_quaternion(static_cast<float>(M_PI_4), simd_float3{0.0f, 1.0f, 0.0f});
    doc.add(node);

    const auto hit = raycast_scene(doc, Ray{{0.0f, 0.0f, 5.0f}, {0.0f, 0.0f, -1.0f}});
    REQUIRE(hit.has_value());
    CHECK(hit->hit.t == doctest::Approx(4.292893219f));
    check_float3_approx(hit->hit.point, simd_float3{0.0f, 0.0f, 0.70710678f});
    check_float3_approx(hit->hit.normal, simd_float3{-0.70710678f, 0.0f, 0.70710678f});
    // Unrotated, the same ray would hit the long +z face at z = 1.5 with normal
    // (0, 0, 1) -- so both assertions above are discriminating, not incidental.
    CHECK(hit->hit.t != doctest::Approx(3.5f));
}

TEST_CASE("raycast_scene hits lie on the SDF's zero set, for rotated and non-uniformly "
          "scaled nodes alike") {
    // The cross-validation between the two independent descriptions of a node's
    // surface: picking inverts world_from_local's T*R*S of a unit primitive,
    // while the SDF translates and rotates and then measures the shape at its
    // true half-extents. They agree only if scale stays baked into the shape
    // rather than folded into the transform -- so this is the assertion that
    // fails if anyone ever "simplifies" sdf_eval_node by dividing q by
    // half_extents, which would warp the field and desync the two paths.
    //
    // No hand-derived literals: the claim is agreement, not any particular
    // number. The box branch is exact; the ellipsoid's zero SET is exact too
    // (k0 == 1 at the surface makes the iq formula return 0), even though its
    // distances away from the surface are approximate.
    struct Case { const char* label; Shape shape; simd_float3 scale; };
    const std::array<Case, 4> cases = {{
        {"cube, uniform", Shape::Cube, {1.0f, 1.0f, 1.0f}},
        {"cube, non-uniform", Shape::Cube, {2.0f, 0.5f, 3.0f}},
        {"ellipsoid, uniform", Shape::Sphere, {1.5f, 1.5f, 1.5f}},
        {"ellipsoid, non-uniform", Shape::Sphere, {1.0f, 3.0f, 2.0f}},
    }};

    for (const Case& c : cases) {
        INFO("case: " << c.label);
        SceneDocument doc;
        Node node;
        node.id = 1;
        node.shape = c.shape;
        node.position = {0.25f, -0.5f, 0.75f};
        node.scale = c.scale;
        node.rotation = simd_quaternion(0.9f, simd_normalize(simd_float3{1.0f, 2.0f, -1.0f}));
        doc.add(node);

        // A spread of oblique rays, so no single lucky alignment can carry the
        // case and each one meets the surface at a different orientation.
        const std::array<simd_float3, 4> eyes = {{
            {0.0f, 0.0f, 8.0f}, {7.0f, 2.0f, 3.0f}, {-4.0f, -5.0f, 2.0f}, {1.0f, 9.0f, -2.0f},
        }};
        for (const simd_float3& eye : eyes) {
            INFO("eye: (" << eye.x << ", " << eye.y << ", " << eye.z << ")");
            const simd_float3 dir = simd_normalize(node.position - eye);
            const auto hit = raycast_scene(doc, Ray{eye, dir});
            REQUIRE(hit.has_value());
            const auto d = evaluate_scene_sdf(doc, hit->hit.point);
            REQUIRE(d.has_value());
            CHECK(std::fabs(*d) < 1e-4f);
        }
    }
}

// --- ray_plane -------------------------------------------------------------

TEST_CASE("ray_plane: literals from the task brief") {
    SUBCASE("head-on ray onto the z=0 plane hits at the origin") {
        const auto hit = ray_plane(Ray{{0.0f, 0.0f, 5.0f}, {0.0f, 0.0f, -1.0f}}, simd_float3{0.0f, 0.0f, 0.0f},
                                    simd_float3{0.0f, 0.0f, 1.0f});
        REQUIRE(hit.has_value());
        check_float3_approx(*hit, simd_float3{0.0f, 0.0f, 0.0f});
    }

    SUBCASE("oblique ray onto the y=0 plane hits at the origin") {
        // denom = dot(normalize(0,-1,-1), (0,1,0)) = -1/sqrt(2).
        // t = dot((0,0,0)-(0,2,2), (0,1,0)) / denom = -2 / (-1/sqrt(2)) = 2*sqrt(2).
        // point = (0,2,2) + 2*sqrt(2) * (0,-1/sqrt(2),-1/sqrt(2)) = (0, 2-2, 2-2) = (0,0,0).
        const simd_float3 dir = simd_normalize(simd_float3{0.0f, -1.0f, -1.0f});
        const auto hit = ray_plane(Ray{{0.0f, 2.0f, 2.0f}, dir}, simd_float3{0.0f, 0.0f, 0.0f},
                                    simd_float3{0.0f, 1.0f, 0.0f});
        REQUIRE(hit.has_value());
        check_float3_approx(*hit, simd_float3{0.0f, 0.0f, 0.0f});
    }

    SUBCASE("ray parallel to the plane misses") {
        // dir.z=-1 is perpendicular to the plane normal (0,1,0): dot == 0, |0| < 1e-6.
        CHECK_FALSE(ray_plane(Ray{{0.0f, 1.0f, 5.0f}, {0.0f, 0.0f, -1.0f}}, simd_float3{0.0f, 0.0f, 0.0f},
                               simd_float3{0.0f, 1.0f, 0.0f})
                        .has_value());
    }

    SUBCASE("plane behind the ray origin misses") {
        // Ray at z=5 heading toward +z (away from the z=0 plane): t would be
        // negative, so the plane is behind the ray origin.
        CHECK_FALSE(ray_plane(Ray{{0.0f, 0.0f, 5.0f}, {0.0f, 0.0f, 1.0f}}, simd_float3{0.0f, 0.0f, 0.0f},
                               simd_float3{0.0f, 0.0f, 1.0f})
                        .has_value());
    }
}

// drag_plane_for_node's two cases moved to gizmo_tests.cpp when the function
// was folded into gizmo_frame_for_node -- the behaviour they pinned (snapped ->
// the snap frame, free -> the node's own) is now a property of the frame, and
// the camera no longer takes part in either.

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
