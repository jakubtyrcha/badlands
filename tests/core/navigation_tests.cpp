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

// --- auto-pivot: does re-pivoting hold the view still? ----------------------

TEST_CASE("beginCameraGesture(Orbit) leaves what is on screen exactly where it was") {
    // The claim auto-pivot is sold on: re-deriving the pivot at press time is
    // invisible. Not just the eye -- the whole IMAGE has to hold still, or
    // every drag starts with a jolt.
    Editor* editor = Editor::create();
    editor->setViewportSize(800.0f, 500.0f, 2.0f);

    const SpawnResult spawned = editor->spawn(Shape::Cube, Op::Add, 400.0f, 250.0f);
    REQUIRE(spawned.node_id != kInvalidNode);

    // Press OFF-CENTRE, on the node but away from the view axis -- the case
    // that matters, since aiming at a local feature is aiming off-centre.
    const ScreenPoint before = editor->projectSelectedAnchor();
    REQUIRE(before.visible);

    editor->beginCameraGesture(CameraGesture::Orbit, 430.0f, 230.0f);

    const ScreenPoint after = editor->projectSelectedAnchor();
    REQUIRE(after.visible);
    CHECK(after.x == doctest::Approx(before.x).epsilon(1e-3));
    CHECK(after.y == doctest::Approx(before.y).epsilon(1e-3));
}

TEST_CASE("beginCameraGesture(Orbit) re-centres the rotation on what the press aimed at") {
    Editor* editor = Editor::create();
    editor->setViewportSize(800.0f, 500.0f, 2.0f);
    REQUIRE(editor->spawn(Shape::Cube, Op::Add, 400.0f, 250.0f).node_id != kInvalidNode);

    // Press well off-centre but still on the node, then rotate. The node's
    // projected anchor must swing, because the camera is now orbiting a point
    // on the node's surface rather than the node's centre.
    editor->beginCameraGesture(CameraGesture::Orbit, 440.0f, 220.0f);
    const ScreenPoint before = editor->projectSelectedAnchor();
    REQUIRE(before.visible);

    editor->updateCameraGesture(120.0f, 0.0f);
    const ScreenPoint after = editor->projectSelectedAnchor();
    REQUIRE(after.visible);
    CHECK(std::fabs(after.x - before.x) > 1.0f); // the view actually rotated
    editor->endCameraGesture();
}

TEST_CASE("updateCameraGesture is cumulative, so coalesced events cannot drift") {
    // One update to (120, -40) must equal many updates ending there. This is
    // what makes the API immune to however AppKit chooses to batch events, and
    // it is why each call re-derives from the captured start rather than
    // integrating a per-event delta.
    const auto anchor_after = [](int steps) {
        Editor* editor = Editor::create();
        editor->setViewportSize(800.0f, 500.0f, 2.0f);
        REQUIRE(editor->spawn(Shape::Cube, Op::Add, 400.0f, 250.0f).node_id != kInvalidNode);
        editor->beginCameraGesture(CameraGesture::Orbit, 420.0f, 240.0f);
        for (int i = 1; i <= steps; ++i) {
            const float t = static_cast<float>(i) / static_cast<float>(steps);
            editor->updateCameraGesture(120.0f * t, -40.0f * t);
        }
        editor->endCameraGesture();
        return editor->projectSelectedAnchor();
    };

    const ScreenPoint one = anchor_after(1);
    const ScreenPoint many = anchor_after(37);
    REQUIRE(one.visible);
    REQUIRE(many.visible);
    CHECK(many.x == doctest::Approx(one.x).epsilon(1e-4));
    CHECK(many.y == doctest::Approx(one.y).epsilon(1e-4));

    SUBCASE("and a gesture dragged back to its start returns exactly there") {
        Editor* editor = Editor::create();
        editor->setViewportSize(800.0f, 500.0f, 2.0f);
        REQUIRE(editor->spawn(Shape::Cube, Op::Add, 400.0f, 250.0f).node_id != kInvalidNode);
        const ScreenPoint start = editor->projectSelectedAnchor();
        REQUIRE(start.visible);

        editor->beginCameraGesture(CameraGesture::Orbit, 420.0f, 240.0f);
        editor->updateCameraGesture(200.0f, 90.0f);
        editor->updateCameraGesture(0.0f, 0.0f);
        editor->endCameraGesture();

        const ScreenPoint back = editor->projectSelectedAnchor();
        REQUIRE(back.visible);
        CHECK(back.x == doctest::Approx(start.x).epsilon(1e-4));
        CHECK(back.y == doctest::Approx(start.y).epsilon(1e-4));
    }
}

TEST_CASE("camera gestures on an empty scene fall through the chain and stay finite") {
    // No node to hit, so every gesture runs on the ground rung or the target
    // plane. The camera must remain usable rather than flying off or NaNing.
    for (const CameraGesture kind :
         {CameraGesture::Orbit, CameraGesture::Pan, CameraGesture::Dolly}) {
        Editor* editor = Editor::create();
        editor->setViewportSize(800.0f, 500.0f, 2.0f);

        editor->beginCameraGesture(kind, 400.0f, 250.0f);
        editor->updateCameraGesture(150.0f, -80.0f);
        editor->endCameraGesture();

        // Observed through the public surface: spawn lands somewhere finite and
        // in front of the camera, which it cannot do from a corrupt camera.
        const SpawnResult s = editor->spawn(Shape::Cube, Op::Add, 400.0f, 250.0f);
        REQUIRE(s.node_id != kInvalidNode);
        const Vec3f p = editor->nodePosition(s.node_id);
        CHECK(std::isfinite(p.x));
        CHECK(std::isfinite(p.y));
        CHECK(std::isfinite(p.z));
        CHECK(editor->projectSelectedAnchor().visible);
    }
}

TEST_CASE("frameSelected re-centres and re-ranges without reorienting") {
    Editor* editor = Editor::create();
    editor->setViewportSize(800.0f, 500.0f, 2.0f);
    const SpawnResult spawned = editor->spawn(Shape::Cube, Op::Add, 400.0f, 250.0f);
    REQUIRE(spawned.node_id != kInvalidNode);

    // Push the camera somewhere unhelpful first.
    editor->beginCameraGesture(CameraGesture::Pan, 400.0f, 250.0f);
    editor->updateCameraGesture(300.0f, 200.0f);
    editor->endCameraGesture();

    editor->frameSelected();

    // Framing centres the node: its anchor lands at the middle of the viewport.
    const ScreenPoint anchor = editor->projectSelectedAnchor();
    REQUIRE(anchor.visible);
    CHECK(anchor.x == doctest::Approx(400.0f).epsilon(1e-3));
    CHECK(anchor.y == doctest::Approx(250.0f).epsilon(1e-3));

    SUBCASE("no selection is a safe no-op") {
        editor->select(kInvalidNode);
        editor->frameSelected(); // must not crash or move anything
        CHECK(editor->selectedNode() == kInvalidNode);
    }
}

TEST_CASE("frame_radius_for_bound floors tiny nodes so framing keeps context") {
    const float fov = 1.0472f;

    // A head-sized bound fits normally: bound * kFrameMargin / sin(fov/2).
    const float big = frame_radius_for_bound(1.0f, fov);
    CHECK(big == doctest::Approx(1.0f * kFrameMargin / std::sin(0.5f * fov)));
    CHECK(big > kMinFrameRadius);

    // A nostril-scale bound would fit at ~0.16, filling the view with
    // featureless surface. The floor is what keeps surroundings on screen.
    CHECK(frame_radius_for_bound(0.05f, fov) == doctest::Approx(kMinFrameRadius));
    CHECK(frame_radius_for_bound(0.0f, fov) == doctest::Approx(kMinFrameRadius));
    CHECK(frame_radius_for_bound(std::nanf(""), fov) == doctest::Approx(kMinFrameRadius));
    CHECK(frame_radius_for_bound(1.0f, 0.0f) == doctest::Approx(kMinFrameRadius)); // degenerate fov
}
