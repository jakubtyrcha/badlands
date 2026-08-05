#include <doctest.h>

#include <cmath>

#include <shapeshifter/ShapeshifterCore.h>

#include "camera.h"
#include "navigation.h"
#include "scene.h"

using namespace sq;

namespace {

void check_float3_approx(const simd_float3 actual, const simd_float3 expected) {
    CHECK(actual.x == doctest::Approx(expected.x));
    CHECK(actual.y == doctest::Approx(expected.y));
    CHECK(actual.z == doctest::Approx(expected.z));
}

Node make_sphere(int32_t id, simd_float3 position, simd_float3 scale) {
    Node node;
    node.id = id;
    node.shape = Shape::Sphere;
    node.position = position;
    node.scale = scale;
    return node;
}

} // namespace

// --- resolve_focus ---------------------------------------------------------

TEST_CASE("resolve_focus: the model wins over the ground beneath it") {
    // Rung 1 is not a peer of the others — it is the rung that makes "point at
    // the feature and drag" work, so a ray that reaches both a node and the
    // plate must report the node.
    SceneDocument doc;
    doc.add(make_sphere(1, simd_float3{0.0f, 2.0f, 0.0f}, simd_float3{2.0f, 2.0f, 2.0f}));

    // Straight down the +y axis from above: hits the sphere's top at y=3, and
    // would otherwise reach y=0.
    const FocusPoint f =
        resolve_focus(doc, Ray{{0.0f, 10.0f, 0.0f}, {0.0f, -1.0f, 0.0f}}, simd_float3{0.0f, 0.0f, 0.0f});

    CHECK(f.source == FocusSource::Scene);
    check_float3_approx(f.point, simd_float3{0.0f, 3.0f, 0.0f});
}

TEST_CASE("resolve_focus: a scene miss falls to the ground plate") {
    SceneDocument doc; // empty

    // 45 degrees down from (0,4,4): meets y=0 at (0,0,0).
    const simd_float3 dir = simd_normalize(simd_float3{0.0f, -1.0f, -1.0f});
    const FocusPoint f = resolve_focus(doc, Ray{{0.0f, 4.0f, 4.0f}, dir}, simd_float3{9.0f, 9.0f, 9.0f});

    CHECK(f.source == FocusSource::Ground);
    check_float3_approx(f.point, simd_float3{0.0f, 0.0f, 0.0f});
}

TEST_CASE("resolve_focus: a ray above the horizon falls through to the target's depth") {
    SceneDocument doc; // empty

    // Aimed upward, so y=0 is behind the origin and ray_plane rejects it.
    const simd_float3 target = {0.0f, 1.0f, 0.0f};
    const simd_float3 origin = {0.0f, 1.0f, 5.0f};
    const FocusPoint f = resolve_focus(doc, Ray{origin, simd_normalize(simd_float3{0.0f, 1.0f, -1.0f})}, target);

    CHECK(f.source == FocusSource::TargetPlane);
    // The point sits at the target's depth ALONG the cursor ray, not at the
    // target itself — that is what keeps a pan anchored where the cursor is.
    const float depth = simd_dot(target - origin, simd_normalize(simd_float3{0.0f, 1.0f, -1.0f}));
    check_float3_approx(f.point, origin + depth * simd_normalize(simd_float3{0.0f, 1.0f, -1.0f}));
}

TEST_CASE("resolve_focus: a grazing ray is rejected rather than placed near infinity") {
    // The failure this prevents: a ray a hair below horizontal passes
    // ray_plane's parallel test, but meets y=0 thousands of units out. Pivoting
    // there would make the next orbit swing around a point effectively at
    // infinity — the camera would appear to pan instead of rotate.
    SceneDocument doc; // empty

    const simd_float3 origin = {0.0f, 1.0f, 0.0f};
    // dy/dz chosen so the plane crossing lands far past kFocusGroundMaxT:
    // t_y = 1 / 1e-4 = 10000 >> 100.
    const simd_float3 dir = simd_normalize(simd_float3{0.0f, -1e-4f, -1.0f});
    const simd_float3 target = {0.0f, 1.0f, -5.0f};

    const FocusPoint f = resolve_focus(doc, Ray{origin, dir}, target);

    CHECK(f.source == FocusSource::TargetPlane);
    CHECK(simd_length(f.point - origin) < kFocusGroundMaxT);
}

TEST_CASE("resolve_focus: never returns a non-finite point") {
    // The NaN-producing poses, all of which reach the ground rung. IEEE
    // comparisons against NaN are uniformly false, so a `t < bound` test alone
    // would pass a NaN straight through — hence the explicit finiteness check
    // in resolve_focus. A NaN pivot is unrecoverable in a way a failed pick
    // never is, which is why this is a case of its own.
    SceneDocument doc;
    doc.add(make_sphere(1, simd_float3{0.0f, 0.0f, 0.0f}, simd_float3{1.0f, 1.0f, 1.0f}));

    struct Case {
        const char* label;
        Ray ray;
        simd_float3 target;
    };
    const Case cases[] = {
        {"camera exactly on the ground plane, ray parallel to it",
         Ray{{0.0f, 0.0f, 5.0f}, {0.0f, 0.0f, -1.0f}}, {0.0f, 0.0f, 0.0f}},
        {"ray exactly parallel to y=0, above it",
         Ray{{0.0f, 3.0f, 5.0f}, {0.0f, 0.0f, -1.0f}}, {0.0f, 3.0f, 0.0f}},
        {"ray exactly parallel to y=0, below it",
         Ray{{0.0f, -3.0f, 5.0f}, {0.0f, 0.0f, -1.0f}}, {0.0f, -3.0f, 0.0f}},
        {"degenerate target at the ray origin",
         Ray{{0.0f, 5.0f, 5.0f}, simd_normalize(simd_float3{0.0f, 1.0f, 0.0f})}, {0.0f, 5.0f, 5.0f}},
    };

    for (const Case& c : cases) {
        INFO("case: " << c.label);
        const FocusPoint f = resolve_focus(doc, c.ray, c.target);
        CHECK(std::isfinite(f.point.x));
        CHECK(std::isfinite(f.point.y));
        CHECK(std::isfinite(f.point.z));
    }

    SUBCASE("a non-finite fallback target cannot leak out through the last rung") {
        const float nan = std::nanf("");
        const FocusPoint f = resolve_focus(SceneDocument{},
                                            Ray{{0.0f, 3.0f, 5.0f}, {0.0f, 0.0f, -1.0f}},
                                            simd_float3{nan, nan, nan});
        CHECK(std::isfinite(f.point.x));
        CHECK(std::isfinite(f.point.y));
        CHECK(std::isfinite(f.point.z));
    }
}

// --- node_bounding_radius --------------------------------------------------

TEST_CASE("node_bounding_radius: cube takes the half-diagonal, sphere the longest semi-axis") {
    Node cube;
    cube.shape = Shape::Cube;
    cube.scale = {2.0f, 4.0f, 4.0f};
    // Half-extents (1,2,2) -> corner distance sqrt(1+4+4) = 3.
    CHECK(node_bounding_radius(cube) == doctest::Approx(3.0f));

    Node sphere;
    sphere.shape = Shape::Sphere;
    sphere.scale = {2.0f, 4.0f, 4.0f};
    // Semi-axes (1,2,2) -> bounding radius is the longest, 2.
    CHECK(node_bounding_radius(sphere) == doctest::Approx(2.0f));

    SUBCASE("the cube's bound strictly contains the sphere's for the same scale") {
        CHECK(node_bounding_radius(cube) > node_bounding_radius(sphere));
    }

    SUBCASE("negative scale components are treated by magnitude") {
        Node flipped;
        flipped.shape = Shape::Cube;
        flipped.scale = {-2.0f, 4.0f, -4.0f};
        CHECK(node_bounding_radius(flipped) == doctest::Approx(3.0f));
    }
}
