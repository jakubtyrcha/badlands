#include <doctest.h>

#include <array>
#include <cmath>
#include <vector>

#include "gizmo.h"
#include "lines.h"
#include "scene.h"

using namespace sq;

namespace {

constexpr std::array<simd_float3, 8> kCubeCorners = {{
    {-0.5f, -0.5f, -0.5f}, // 0
    { 0.5f, -0.5f, -0.5f}, // 1
    { 0.5f,  0.5f, -0.5f}, // 2
    {-0.5f,  0.5f, -0.5f}, // 3
    {-0.5f, -0.5f,  0.5f}, // 4
    { 0.5f, -0.5f,  0.5f}, // 5
    { 0.5f,  0.5f,  0.5f}, // 6
    {-0.5f,  0.5f,  0.5f}, // 7
}};

constexpr std::array<std::array<int, 2>, 12> kCubeEdges = {{
    {0, 1}, {1, 5}, {5, 4}, {4, 0}, // bottom
    {3, 2}, {2, 6}, {6, 7}, {7, 3}, // top
    {0, 3}, {1, 2}, {5, 6}, {4, 7}, // verticals
}};

// Parameters are `const`: doctest's CHECK() binds the compared sub-expression
// to a reference, and Clang only allows that for *const* accesses of
// ext_vector_type components (e.g. `simd_float3.x`) — a non-const vector
// element cannot bind to a non-const reference.
void check_float3_approx(const simd_float3 actual, const simd_float3 expected) {
    CHECK(actual.x == doctest::Approx(expected.x));
    CHECK(actual.y == doctest::Approx(expected.y));
    CHECK(actual.z == doctest::Approx(expected.z));
}

void check_color_approx(const simd_float4 actual, const simd_float4 expected) {
    CHECK(actual.x == doctest::Approx(expected.x));
    CHECK(actual.y == doctest::Approx(expected.y));
    CHECK(actual.z == doctest::Approx(expected.z));
    CHECK(actual.w == doctest::Approx(expected.w));
}

} // namespace

TEST_CASE("append_cube_edges: identity transform emits the pinned edge list") {
    std::vector<LineVertex> out;
    append_cube_edges(out, matrix_identity_float4x4, kColorAdd);

    REQUIRE(out.size() == 24);

    for (size_t i = 0; i < kCubeEdges.size(); ++i) {
        const simd_float3 expected_a = kCubeCorners[kCubeEdges[i][0]];
        const simd_float3 expected_b = kCubeCorners[kCubeEdges[i][1]];
        CAPTURE(i);
        check_float3_approx(out[2 * i].pos.xyz, expected_a);
        check_float3_approx(out[2 * i + 1].pos.xyz, expected_b);
    }

    for (const LineVertex& v : out) {
        CHECK(v.pos.w == doctest::Approx(1.0f));
    }
}

TEST_CASE("append_cube_edges: translation shifts every vertex by exactly that offset") {
    const simd_float3 offset = {1.0f, 2.0f, 3.0f};
    simd_float4x4 world_from_local = matrix_identity_float4x4;
    world_from_local.columns[3] = (simd_float4){offset.x, offset.y, offset.z, 1.0f};

    std::vector<LineVertex> identity_out;
    append_cube_edges(identity_out, matrix_identity_float4x4, kColorAdd);
    std::vector<LineVertex> translated_out;
    append_cube_edges(translated_out, world_from_local, kColorAdd);

    REQUIRE(identity_out.size() == translated_out.size());
    for (size_t i = 0; i < identity_out.size(); ++i) {
        CAPTURE(i);
        check_float3_approx(translated_out[i].pos.xyz, identity_out[i].pos.xyz + offset);
    }
}

// Horizon-circle derivation for the unit sphere (r = 0.5) seen from local eye o,
// d = |o| (see https://iquilezles.org/articles/sphereproj/): the silhouette is the
// circle of tangency points — center (r^2/d^2)*o, radius r*sqrt(d^2-r^2)/d, in the
// plane perpendicular to o. For o = (0,0,2): d = 2, center (0,0,0.0625*2) =
// (0, 0, 0.125), radius 0.5*sqrt(4-0.25)/2 = 0.25*sqrt(3.75) = 0.48412292.
TEST_CASE("append_sphere_outline: identity transform, eye at (0,0,2): horizon circle "
          "on the sphere, tangent to the eye rays") {
    std::vector<LineVertex> out;
    const simd_float3 eye = {0.0f, 0.0f, 2.0f};
    append_sphere_outline(out, matrix_identity_float4x4, kColorSubtract, eye);

    REQUIRE(out.size() == 2 * kSphereOutlineSegments);

    for (size_t i = 0; i < out.size(); ++i) {
        CAPTURE(i);
        const LineVertex& vert = out[i];
        const simd_float3 p = vert.pos.xyz;
        // On the sphere surface...
        CHECK(simd_length(p) == doctest::Approx(0.5f).epsilon(1e-4));
        // ...in the horizon plane z = 0.125...
        CHECK(p.z == doctest::Approx(0.125f).epsilon(1e-4));
        // ...at the derived circle radius from the circle center...
        CHECK(simd_length(p - simd_float3{0.0f, 0.0f, 0.125f}) ==
              doctest::Approx(0.48412292f).epsilon(1e-4));
        // ...and tangent: the eye ray touches the sphere, so (p - eye) is
        // perpendicular to the surface normal (which is p, for an origin sphere).
        CHECK(std::fabs(simd_dot(p - eye, p)) < 1e-4f);
        CHECK(vert.pos.w == doctest::Approx(1.0f));
        check_color_approx(vert.color, kColorSubtract);
    }

    // The line loop closes: the last segment's far endpoint is the first vertex.
    check_float3_approx(out[out.size() - 1].pos.xyz, out[0].pos.xyz);
}

TEST_CASE("append_sphere_outline: translated sphere keeps the outline on its surface") {
    std::vector<LineVertex> out;
    const simd_float3 position = {0.0f, 3.0f, 0.0f};
    simd_float4x4 world_from_local = matrix_identity_float4x4;
    world_from_local.columns[3] = simd_float4{position.x, position.y, position.z, 1.0f};
    // Eye at (0,3,2) is (0,0,2) in local space: same circle as the identity case,
    // shifted by the translation.
    append_sphere_outline(out, world_from_local, kColorAdd, simd_float3{0.0f, 3.0f, 2.0f});

    REQUIRE(out.size() == 2 * kSphereOutlineSegments);
    for (size_t i = 0; i < out.size(); ++i) {
        CAPTURE(i);
        const simd_float3 p = out[i].pos.xyz;
        CHECK(simd_length(p - position) == doctest::Approx(0.5f).epsilon(1e-4));
        CHECK(p.z == doctest::Approx(0.125f).epsilon(1e-4));
    }
}

TEST_CASE("append_sphere_outline: non-uniform scale (ellipsoid) stays exact via the "
          "local-space construction") {
    // Ellipsoid = unit sphere under M = diag(2,1,1). Affine maps preserve
    // tangency, so the world silhouette is M * (local horizon circle seen from
    // M^-1 * eye). Eye (4,0,0) -> local (2,0,0): local circle x = 0.125,
    // radius 0.48412292 -> world plane x = 0.25, y^2 + z^2 = 0.48412292^2 =
    // 0.234375.
    std::vector<LineVertex> out;
    const simd_float4x4 world_from_local = simd_matrix(
        simd_float4{2.0f, 0.0f, 0.0f, 0.0f}, simd_float4{0.0f, 1.0f, 0.0f, 0.0f},
        simd_float4{0.0f, 0.0f, 1.0f, 0.0f}, simd_float4{0.0f, 0.0f, 0.0f, 1.0f});
    append_sphere_outline(out, world_from_local, kColorAdd, simd_float3{4.0f, 0.0f, 0.0f});

    REQUIRE(out.size() == 2 * kSphereOutlineSegments);
    for (size_t i = 0; i < out.size(); ++i) {
        CAPTURE(i);
        const simd_float3 p = out[i].pos.xyz;
        CHECK(p.x == doctest::Approx(0.25f).epsilon(1e-4));
        CHECK(p.y * p.y + p.z * p.z == doctest::Approx(0.234375f).epsilon(1e-4));
        // On the ellipsoid: (x/2)^2 + y^2 + z^2 = 0.25.
        CHECK((p.x * 0.5f) * (p.x * 0.5f) + p.y * p.y + p.z * p.z ==
              doctest::Approx(0.25f).epsilon(1e-4));
    }
}

TEST_CASE("append_sphere_outline: eye inside or on the sphere emits nothing") {
    std::vector<LineVertex> out;
    append_sphere_outline(out, matrix_identity_float4x4, kColorAdd, simd_float3{0.0f, 0.0f, 0.3f});
    CHECK(out.empty());
    append_sphere_outline(out, matrix_identity_float4x4, kColorAdd, simd_float3{0.0f, 0.0f, 0.5f});
    CHECK(out.empty());
}

// Wireframe policy (subtract-always + selected-always-overrides): the
// raymarched view is always the live primary, so the wireframe stays a thin
// annotation layer, not a full-scene outline -- EXCEPT that an unselected
// Subtract node still draws (kColorSubtract): its carve is the ONLY visual it
// has anywhere in the viewport (additive geometry is always visible via the
// raymarch regardless), so without this exception it would be entirely
// unpickable-by-sight. The selected node (either op) always overrides to
// kColorSelected. Unselected Add nodes emit nothing. This replaces (rather
// than extends) the previous selected-only-always test case.
TEST_CASE("build_scene_lines: subtract-always + selected-always-overrides policy") {
    SceneDocument doc;

    Node cube;
    cube.id = 1;
    cube.shape = Shape::Cube;
    cube.op = Op::Add;
    doc.add(cube);

    Node sphere;
    sphere.id = 2;
    sphere.shape = Shape::Sphere;
    sphere.op = Op::Subtract;
    doc.add(sphere);

    const simd_float3 eye = {0.0f, 0.0f, 5.0f};

    SUBCASE("no selection: the unselected Add cube emits nothing; the unselected "
            "Subtract sphere still emits its outline, in kColorSubtract") {
        const std::vector<LineVertex> lines = build_scene_lines(doc, kInvalidNode, eye);
        REQUIRE(lines.size() == 2 * kSphereOutlineSegments); // zero cube verts included
        for (const LineVertex& v : lines) {
            check_color_approx(v.color, kColorSubtract);
        }
    }

    SUBCASE("cube (Add) selected: cube's 24 edges in kColorSelected, PLUS the "
            "unselected Subtract sphere's outline in kColorSubtract (its always-on "
            "visual doesn't depend on what's selected)") {
        const std::vector<LineVertex> lines = build_scene_lines(doc, cube.id, eye);
        REQUIRE(lines.size() == 24 + 2 * kSphereOutlineSegments);
        for (size_t i = 0; i < 24; ++i) {
            CAPTURE(i);
            check_color_approx(lines[i].color, kColorSelected);
        }
        for (size_t i = 24; i < lines.size(); ++i) {
            CAPTURE(i);
            check_color_approx(lines[i].color, kColorSubtract);
        }
    }

    SUBCASE("sphere (Subtract) selected: the selected override wins over the "
            "always-on subtract color -- every vertex is kColorSelected") {
        const std::vector<LineVertex> lines = build_scene_lines(doc, sphere.id, eye);
        REQUIRE(lines.size() == 2 * kSphereOutlineSegments);
        for (const LineVertex& v : lines) {
            check_color_approx(v.color, kColorSelected);
        }
    }
}

// --- append_move_gizmo_grid / append_move_gizmo_handles ---------------------
//
// Handle emission order is pinned (the highlight cases below depend on it):
// thick segments of 6 triangle verts each — axes u, v, n (1 segment each,
// [0,18)), then patch outlines uv, un, vn (4 segments = 24 verts each:
// [18,42), [42,66), [66,90)) = 90 verts total.
// (The tangent-basis formula itself is pinned in gizmo_tests.cpp.)

namespace {

GizmoFrame make_gizmo_frame(simd_float3 origin, simd_float3 n, float he) {
    GizmoFrame f;
    f.origin = origin;
    f.n = n;
    tangent_basis(n, f.u, f.v);
    f.half_extent = he;
    return f;
}

bool color_is(const LineVertex& v, const simd_float4 expected) {
    return v.color.x == doctest::Approx(expected.x) &&
           v.color.y == doctest::Approx(expected.y) &&
           v.color.z == doctest::Approx(expected.z) &&
           v.color.w == doctest::Approx(expected.w);
}

// Every expanded vertex of a thick segment [a,b] lies within half_width of
// the segment's line, inside the endpoint extension along it.
void check_on_thick_segment(const simd_float3 p, const simd_float3 a, const simd_float3 b,
                            const float half_width) {
    const simd_float3 dir = simd_normalize(b - a);
    const float along = simd_dot(p - a, dir);
    CHECK(along > -half_width - 1e-5f);
    CHECK(along < simd_length(b - a) + half_width + 1e-5f);
    const simd_float3 off = p - a - along * dir;
    CHECK(simd_length(off) < half_width + 1e-5f);
}

} // namespace

TEST_CASE("append_move_gizmo_grid: n={0,1,0}, he=2, divisions=12 — thin, coplanar, faint") {
    const GizmoFrame f = make_gizmo_frame(simd_float3{0.0f, 0.0f, 0.0f}, simd_float3{0.0f, 1.0f, 0.0f}, 2.0f);
    std::vector<LineVertex> out;
    append_move_gizmo_grid(out, f, 12);

    // 24 grid lines (2*divisions, center skipped) = 48 verts.
    REQUIRE(out.size() == 48);
    for (size_t i = 0; i < out.size(); ++i) {
        CAPTURE(i);
        const simd_float3 p = out[i].pos.xyz;
        const simd_float4 c = out[i].color;
        CHECK(std::fabs(simd_dot(f.n, p - f.origin)) < 1e-5f);
        CHECK(c.w == doctest::Approx(0.18f));
    }
}

TEST_CASE("append_move_gizmo_handles: counts, thick-quad geometry, RGB axis colors and "
          "additive-mix plane colors") {
    const GizmoFrame f = make_gizmo_frame(simd_float3{0.0f, 0.0f, 0.0f}, simd_float3{0.0f, 1.0f, 0.0f}, 2.0f);
    const float he = f.half_extent;
    const float hw = kGizmoHandleHalfWidthFrac * he;
    const simd_float3 eye = {3.0f, 8.0f, 2.0f}; // off every axis: no degenerate expansion
    std::vector<LineVertex> out;
    append_move_gizmo_handles(out, f, GizmoHandle::None, eye);

    // (3 axes * 1 + 3 patches * 4) segments * 6 verts = 90.
    REQUIRE(out.size() == 90);

    // Axes: 6 verts each, on the POSITIVE 0..+he segment only (R3 user
    // ruling — no negative half), in their axis color.
    const struct { simd_float3 dir; simd_float4 color; size_t base; } axes[] = {
        {f.u, kColorAxisU, 0}, {f.v, kColorAxisV, 6}, {f.n, kColorAxisN, 12},
    };
    for (const auto& axis : axes) {
        for (size_t i = axis.base; i < axis.base + 6; ++i) {
            CAPTURE(i);
            check_on_thick_segment(out[i].pos.xyz, f.origin, f.origin + he * axis.dir, hw);
            CHECK(color_is(out[i], axis.color));
        }
    }

    // Patches: 24 verts each, within the outline's thickened bounds, mixed color.
    const struct { simd_float3 e1, e2; simd_float4 color; size_t base; } patches[] = {
        {f.u, f.v, kColorPlaneUV, 18}, {f.u, f.n, kColorPlaneUN, 42}, {f.v, f.n, kColorPlaneVN, 66},
    };
    for (const auto& patch : patches) {
        for (size_t i = patch.base; i < patch.base + 24; ++i) {
            CAPTURE(i);
            const simd_float3 p = out[i].pos.xyz;
            const float x = simd_dot(p - f.origin, patch.e1);
            const float y = simd_dot(p - f.origin, patch.e2);
            CHECK(x > 0.3f * he - 2.0f * hw - 1e-5f);
            CHECK(x < 0.6f * he + 2.0f * hw + 1e-5f);
            CHECK(y > 0.3f * he - 2.0f * hw - 1e-5f);
            CHECK(y < 0.6f * he + 2.0f * hw + 1e-5f);
            CHECK(color_is(out[i], patch.color));
        }
    }
}

TEST_CASE("append_move_gizmo_handles: the highlighted handle's vertices — and only those — "
          "go hot (white)") {
    const GizmoFrame f = make_gizmo_frame(simd_float3{1.0f, 2.0f, 3.0f},
                                          simd_normalize(simd_float3{1.0f, 1.0f, 0.0f}), 2.0f);
    const simd_float3 eye = {4.0f, 9.0f, 5.0f};
    std::vector<LineVertex> out;

    auto check_hot_range = [&](GizmoHandle highlighted, size_t expect_begin, size_t expect_count) {
        out.clear();
        append_move_gizmo_handles(out, f, highlighted, eye);
        REQUIRE(out.size() == 90);
        size_t hot = 0;
        for (size_t i = 0; i < out.size(); ++i) {
            CAPTURE(i);
            const bool should_be_hot = i >= expect_begin && i < expect_begin + expect_count;
            CHECK(color_is(out[i], kColorGizmoHot) == should_be_hot);
            hot += color_is(out[i], kColorGizmoHot) ? 1 : 0;
        }
        CHECK(hot == expect_count);
    };

    SUBCASE("AxisU") { check_hot_range(GizmoHandle::AxisU, 0, 6); }
    SUBCASE("AxisN") { check_hot_range(GizmoHandle::AxisN, 12, 6); }
    SUBCASE("PlaneUV") { check_hot_range(GizmoHandle::PlaneUV, 18, 24); }
    SUBCASE("PlaneVN") { check_hot_range(GizmoHandle::PlaneVN, 66, 24); }
}

// --- append_spiked_cube (camera-pivot marker) --------------------------------

TEST_CASE("append_spiked_cube: 12 world-axis-aligned edges + 6 face spikes, bounded, "
          "single color") {
    const simd_float3 center = {1.0f, -2.0f, 3.0f};
    const float he = 0.5f;
    const float hw = 0.02f;
    const simd_float3 eye = {5.0f, 3.0f, 9.0f};
    const simd_float4 gray = {0.5f, 0.5f, 0.5f, 1.0f};

    std::vector<LineVertex> out;
    append_spiked_cube(out, center, he, hw, eye, gray);

    // (12 cube edges + 6 spikes) thick segments * 6 verts = 108.
    REQUIRE(out.size() == 108);

    for (size_t i = 0; i < out.size(); ++i) {
        CAPTURE(i);
        const simd_float3 p = out[i].pos.xyz;
        // Everything lives inside the spike envelope: 2he per world axis
        // (+ expansion slack).
        CHECK(std::fabs(p.x - center.x) < 2.0f * he + 2.0f * hw + 1e-5f);
        CHECK(std::fabs(p.y - center.y) < 2.0f * he + 2.0f * hw + 1e-5f);
        CHECK(std::fabs(p.z - center.z) < 2.0f * he + 2.0f * hw + 1e-5f);
        CHECK(color_is(out[i], gray));
    }

    // Each spike reaches its tip at center + 2he along a world axis: some
    // vertex must land within the expansion slack of every tip.
    const simd_float3 axes_dirs[] = {
        {1.0f, 0.0f, 0.0f}, {-1.0f, 0.0f, 0.0f},
        {0.0f, 1.0f, 0.0f}, {0.0f, -1.0f, 0.0f},
        {0.0f, 0.0f, 1.0f}, {0.0f, 0.0f, -1.0f},
    };
    for (const simd_float3 dir : axes_dirs) {
        CAPTURE(dir.x); CAPTURE(dir.y); CAPTURE(dir.z);
        const simd_float3 tip = center + 2.0f * he * dir;
        bool found = false;
        for (const LineVertex& v : out) {
            found = found || simd_length(v.pos.xyz - tip) < 2.0f * hw + 1e-4f;
        }
        CHECK(found);
    }
}
