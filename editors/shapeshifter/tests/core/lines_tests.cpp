#include <doctest.h>

#include <array>
#include <cmath>
#include <vector>

#include <ground_grid.h> // kGroundAxisY, for the origin-marker colour case

#include "gizmo.h"
#include "lines.h"
#include "sdf.h"   // evaluate_scene_sdf -- the zero-set cross-check below
#include "scene.h"
#include "placement_helper.h"

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
// Handle emission order is pinned (the highlight cases below depend on it),
// 132 verts total:
//   [0,18)    3 axis shafts, one 6-vert thick segment each: u, v, n
//   [18,36)   3 camera-facing tip dots capping those shafts: u, v, n
//   [36,126)  3 plane patches, 30 verts each — a 6-vert fill quad followed by
//             a 4-segment (24-vert) hairline outline: uv, un, vn
//   [126,132) the origin pip
// Note an axis handle's verts are NOT contiguous: its shaft and its tip dot
// sit in different blocks, which the highlight cases below account for.
// (The tangent-basis formula itself is pinned in gizmo_tests.cpp.)

namespace {

GizmoFrame make_gizmo_frame(simd_float3 origin, simd_float3 n, float he) {
    GizmoFrame f;
    f.origin = origin;
    f.n = n;
    tangent_basis(n, f.u, f.v);
    // The attached-node case, where the grid plane and the u-v plane are the
    // same plane. The free case (grid_normal != n) has its own test below.
    f.grid_normal = n;
    f.grid_u = f.u;
    f.grid_v = f.v;
    f.half_extent = he;
    return f;
}

bool color_is(const LineVertex& v, const simd_float4 expected) {
    return v.color.x == doctest::Approx(expected.x) &&
           v.color.y == doctest::Approx(expected.y) &&
           v.color.z == doctest::Approx(expected.z) &&
           v.color.w == doctest::Approx(expected.w);
}

// Hue only. Needed because the restyle made alpha carry meaning of its own:
// a resting handle is its base color at kGizmoHandleRestAlpha, a highlighted
// one is white — but a highlighted patch's FILL is white at the fill alpha,
// not at 1.0. So "is this vertex hot?" is a question about rgb, and the
// alphas get checked separately.
bool rgb_is(const LineVertex& v, const simd_float4 expected) {
    return v.color.x == doctest::Approx(expected.x) &&
           v.color.y == doctest::Approx(expected.y) &&
           v.color.z == doctest::Approx(expected.z);
}

// The base color a resting handle should carry: its own hue, dimmed.
simd_float4 resting(simd_float4 base) {
    base.w = kGizmoHandleRestAlpha;
    return base;
}

// Returns alpha BY VALUE. `v.color.w` is a simd vector-element accessor, not
// an lvalue, so doctest's expression decomposer cannot bind a reference to it
// ("non-const reference cannot bind to vector element"). Going through a
// function forces the copy.
float alpha_of(const LineVertex& v) { return v.color.w; }

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

TEST_CASE("append_move_gizmo_grid: n={0,1,0}, he=2, divisions=12 — coplanar, radially faded") {
    const GizmoFrame f = make_gizmo_frame(simd_float3{0.0f, 0.0f, 0.0f}, simd_float3{0.0f, 1.0f, 0.0f}, 2.0f);
    const float he = f.half_extent;
    std::vector<LineVertex> out;
    append_move_gizmo_grid(out, f, 12, 1.0f);

    // 26 grid lines (2*(divisions+1), center lines included — the positive-
    // only axis handles no longer cover their negative halves), each split
    // into kGizmoGridSegmentsPerLine 2-vertex segments so the radial fade can
    // be evaluated per vertex.
    REQUIRE(out.size() == 26 * kGizmoGridSegmentsPerLine * 2);

    for (size_t i = 0; i < out.size(); ++i) {
        CAPTURE(i);
        CHECK(std::fabs(simd_dot(f.n, out[i].pos.xyz - f.origin)) < 1e-5f);
        CHECK(alpha_of(out[i]) >= 0.0f);
        CHECK(alpha_of(out[i]) <= kGizmoGridAlpha + 1e-5f);
    }

    SUBCASE("alpha is a function of radius: full at the center, gone past the fade end") {
        for (const LineVertex& v : out) {
            const simd_float3 d = v.pos.xyz - f.origin;
            const float r = simd_length(d);
            CAPTURE(r);
            if (r < kGizmoGridFadeBegin * he - 1e-4f) {
                CHECK(alpha_of(v) == doctest::Approx(kGizmoGridAlpha));
            } else if (r > kGizmoGridFadeEnd * he + 1e-4f) {
                CHECK(alpha_of(v) == doctest::Approx(0.0f));
            }
        }
    }

    SUBCASE("the origin vertex is at full grid alpha") {
        bool found = false;
        for (const LineVertex& v : out) {
            if (simd_length(v.pos.xyz - f.origin) < 1e-4f) {
                found = true;
                CHECK(alpha_of(v) == doctest::Approx(kGizmoGridAlpha));
            }
        }
        CHECK(found);
    }

    // Regression (post-R3 review): the center lines must reach the NEGATIVE
    // ends — the positive-only axis handles no longer cover them. They are
    // fully faded out there, but the geometry has to exist or the fade would
    // start from a hard cut instead.
    for (const simd_float3 neg_end : {f.origin - he * f.u, f.origin - he * f.v}) {
        bool found = false;
        for (const LineVertex& v : out) {
            found = found || simd_length(v.pos.xyz - neg_end) < 1e-4f;
        }
        CHECK(found);
    }
}

TEST_CASE("append_move_gizmo_grid: the grid follows grid_normal, not n") {
    // A free node's frame: axes on the node's own (here world) basis, but the
    // grid world-horizontal. The two disagree, which is the whole point -- the
    // grid is a world reference plane, not the u-v drag plane, so a free node
    // gets a local echo of the ground plate instead of a vertical wall.
    GizmoFrame f;
    f.origin = {0.0f, 1.5f, 0.0f};
    f.u = {1.0f, 0.0f, 0.0f};
    f.v = {0.0f, 1.0f, 0.0f};
    f.n = {0.0f, 0.0f, 1.0f};
    f.grid_normal = {0.0f, 1.0f, 0.0f};
    grid_basis(f.grid_normal, f.u, f.v, f.n, f.grid_u, f.grid_v);
    f.half_extent = 2.0f;

    std::vector<LineVertex> out;
    append_move_gizmo_grid(out, f, 12, 1.0f);
    REQUIRE(out.size() == 26 * kGizmoGridSegmentsPerLine * 2);

    for (size_t i = 0; i < out.size(); ++i) {
        CAPTURE(i);
        // Horizontal: every vertex sits at the origin's height...
        CHECK(std::fabs(out[i].pos.y - f.origin.y) < 1e-5f);
        // ...which is emphatically NOT the u-v plane (z = origin.z), where the
        // grid would have been drawn had it followed n.
        CHECK(std::fabs(simd_dot(f.grid_normal, out[i].pos.xyz - f.origin)) < 1e-5f);
    }

    // The grid genuinely spans z, so it cannot be the (constant-z) u-v plane.
    bool spans_z = false;
    for (const LineVertex& v : out) {
        spans_z = spans_z || std::fabs(v.pos.z - f.origin.z) > 0.5f;
    }
    CHECK(spans_z);
}

TEST_CASE("append_move_gizmo_handles: counts, geometry, desaturated axis colors and "
          "filled plane patches") {
    const GizmoFrame f = make_gizmo_frame(simd_float3{0.0f, 0.0f, 0.0f}, simd_float3{0.0f, 1.0f, 0.0f}, 2.0f);
    const float he = f.half_extent;
    const float hw = kGizmoHandleHalfWidthFrac * he;
    const float border_hw = kGizmoPatchBorderHalfWidthFrac * he;
    const float tip = kGizmoAxisTipHalfSizeFrac * he;
    const simd_float3 eye = {3.0f, 8.0f, 2.0f}; // off every axis: no degenerate expansion
    std::vector<LineVertex> out;
    append_move_gizmo_handles(out, f, GizmoHandle::None, eye, kGizmoHandleRestAlpha);

    // 3 shafts*6 + 3 tips*6 + 1 patch*(6 fill + 24 outline) + 6 pip = 72.
    REQUIRE(out.size() == 72);

    const struct { simd_float3 dir; simd_float4 color; size_t shaft; size_t tip; } axes[] = {
        {f.u, kColorAxisU, 0, 18}, {f.v, kColorAxisV, 6, 24}, {f.n, kColorAxisN, 12, 30},
    };

    SUBCASE("shafts run the POSITIVE half only, stopping short for the tip dot") {
        for (const auto& axis : axes) {
            const simd_float3 end = f.origin + kMoveAxisShaftFrac * he * axis.dir;
            for (size_t i = axis.shaft; i < axis.shaft + 6; ++i) {
                CAPTURE(i);
                check_on_thick_segment(out[i].pos.xyz, f.origin, end, hw);
                CHECK(color_is(out[i], resting(axis.color)));
                // R3: no negative half.
                CHECK(simd_dot(out[i].pos.xyz - f.origin, axis.dir) > -hw - 1e-5f);
            }
        }
    }

    SUBCASE("tip dots cap the shafts and face the eye") {
        for (const auto& axis : axes) {
            const simd_float3 center = f.origin + kMoveAxisShaftFrac * he * axis.dir;
            for (size_t i = axis.tip; i < axis.tip + 6; ++i) {
                CAPTURE(i);
                // Corners of a square of half-size `tip`, so at most sqrt(2)*tip out.
                CHECK(simd_length(out[i].pos.xyz - center) < 1.4143f * tip + 1e-5f);
                CHECK(color_is(out[i], resting(axis.color)));
            }
        }
    }

    // The ONE patch: 6 fill verts exactly on the patch bounds, then 24 outline
    // verts within the thickened bounds, in the GRID plane. Bounds come from the
    // SHARED constants that pick_gizmo_handle also reads — that is the drawn =
    // hit invariant.

    SUBCASE("the patch fill sits exactly on the pickable bounds") {
        for (size_t i = 36; i < 42; ++i) {
            CAPTURE(i);
            const simd_float3 p = out[i].pos.xyz;
            const float x = simd_dot(p - f.origin, f.grid_u);
            const float y = simd_dot(p - f.origin, f.grid_v);
            CHECK(x >= kGizmoPatchInner * he - 1e-5f);
            CHECK(x <= kGizmoPatchOuter * he + 1e-5f);
            CHECK(y >= kGizmoPatchInner * he - 1e-5f);
            CHECK(y <= kGizmoPatchOuter * he + 1e-5f);
            CHECK(rgb_is(out[i], kColorPlane));
            CHECK(alpha_of(out[i]) == doctest::Approx(kGizmoPatchFillAlpha));
        }
    }

    SUBCASE("the patch outline is a hairline around those same bounds") {
        for (size_t i = 42; i < 66; ++i) {
            CAPTURE(i);
            const simd_float3 p = out[i].pos.xyz;
            const float x = simd_dot(p - f.origin, f.grid_u);
            const float y = simd_dot(p - f.origin, f.grid_v);
            CHECK(x > kGizmoPatchInner * he - 2.0f * border_hw - 1e-5f);
            CHECK(x < kGizmoPatchOuter * he + 2.0f * border_hw + 1e-5f);
            CHECK(y > kGizmoPatchInner * he - 2.0f * border_hw - 1e-5f);
            CHECK(y < kGizmoPatchOuter * he + 2.0f * border_hw + 1e-5f);
            CHECK(color_is(out[i], resting(kColorPlane)));
        }
    }

    SUBCASE("origin pip closes the run") {
        for (size_t i = 66; i < 72; ++i) {
            CAPTURE(i);
            CHECK(simd_length(out[i].pos.xyz - f.origin) < 1.4143f * tip + 1e-5f);
            CHECK(color_is(out[i], kColorOriginPip));
        }
    }
}

TEST_CASE("append_move_gizmo_handles: the highlighted handle's vertices — and only those — "
          "go hot (white)") {
    const GizmoFrame f = make_gizmo_frame(simd_float3{1.0f, 2.0f, 3.0f},
                                          simd_normalize(simd_float3{1.0f, 1.0f, 0.0f}), 2.0f);
    const simd_float3 eye = {4.0f, 9.0f, 5.0f};
    std::vector<LineVertex> out;

    // An axis handle's verts are split across two blocks (shaft + tip dot),
    // so ranges are given as a list rather than a single begin/count. The
    // origin pip [66,72) is white too but is NOT a handle, so it is excluded
    // everywhere -- hence the explicit exclusion in the sweep below.
    auto check_hot = [&](GizmoHandle highlighted, std::vector<std::pair<size_t, size_t>> ranges) {
        out.clear();
        append_move_gizmo_handles(out, f, highlighted, eye, kGizmoHandleRestAlpha);
        REQUIRE(out.size() == 72);

        for (size_t i = 0; i < out.size(); ++i) {
            CAPTURE(i);
            if (i >= 66) {
                continue; // origin pip: always white, never a handle
            }
            bool in_range = false;
            for (const auto& r : ranges) {
                in_range = in_range || (i >= r.first && i < r.first + r.second);
            }
            CHECK(rgb_is(out[i], kColorGizmoHot) == in_range);
        }
    };

    SUBCASE("AxisU: shaft and tip dot both") { check_hot(GizmoHandle::AxisU, {{0, 6}, {18, 6}}); }
    SUBCASE("AxisN: shaft and tip dot both") { check_hot(GizmoHandle::AxisN, {{12, 6}, {30, 6}}); }
    SUBCASE("Plane: fill and outline both") { check_hot(GizmoHandle::Plane, {{36, 30}}); }

    SUBCASE("a highlighted patch brightens its fill without making it opaque") {
        out.clear();
        append_move_gizmo_handles(out, f, GizmoHandle::Plane, eye, kGizmoHandleRestAlpha);
        // Fill stays translucent (it sits over the model), outline goes solid.
        CHECK(alpha_of(out[36]) == doctest::Approx(2.0f * kGizmoPatchFillAlpha));
        CHECK(alpha_of(out[36]) < 1.0f);
        CHECK(alpha_of(out[42]) == doctest::Approx(1.0f));
    }

    SUBCASE("unhighlighted handles rest below full opacity") {
        out.clear();
        append_move_gizmo_handles(out, f, GizmoHandle::None, eye, kGizmoHandleRestAlpha);
        CHECK(alpha_of(out[0]) == doctest::Approx(kGizmoHandleRestAlpha));
        CHECK(alpha_of(out[0]) < 1.0f);
    }
}

// --- append_pivot_crosshair (camera-pivot marker) ----------------------------

TEST_CASE("append_pivot_crosshair: a flat eye-facing ring with four ticks crossing it") {
    const simd_float3 center = {1.0f, -2.0f, 3.0f};
    const float radius = 0.5f;
    const float hw = 0.02f;
    const simd_float3 eye = {5.0f, 3.0f, 9.0f};
    const simd_float4 white = {1.0f, 1.0f, 1.0f, 0.85f};

    std::vector<LineVertex> out;
    append_pivot_crosshair(out, center, radius, hw, eye, white);

    // (kPivotRingSegments ring + 4 tick) thick segments * 6 verts.
    REQUIRE(out.size() == static_cast<size_t>(kPivotRingSegments + 4) * 6);

    const simd_float3 n = simd_normalize(eye - center);

    SUBCASE("everything is coplanar in the eye-facing plane") {
        // This is what makes the marker read as a flat annotation rather than
        // an object: its silhouette cannot change as the camera orbits.
        for (size_t i = 0; i < out.size(); ++i) {
            CAPTURE(i);
            CHECK(std::fabs(simd_dot(out[i].pos.xyz - center, n)) < hw + 1e-5f);
        }
    }

    SUBCASE("bounded by the outermost tick") {
        for (size_t i = 0; i < out.size(); ++i) {
            CAPTURE(i);
            CHECK(simd_length(out[i].pos.xyz - center) <
                  kPivotTickOuterFrac * radius + 2.0f * hw + 1e-5f);
            CHECK(color_is(out[i], white));
        }
    }

    SUBCASE("the ring sits at the radius") {
        for (size_t i = 0; i < static_cast<size_t>(kPivotRingSegments) * 6; ++i) {
            CAPTURE(i);
            CHECK(simd_length(out[i].pos.xyz - center) == doctest::Approx(radius).epsilon(0.1));
        }
    }

    SUBCASE("the ticks cross the ring rather than stopping at it") {
        // A tick must have vertices both inside and outside the ring radius,
        // which is what makes this read as a crosshair and not a bare circle.
        bool inside = false, outside = false;
        for (size_t i = static_cast<size_t>(kPivotRingSegments) * 6; i < out.size(); ++i) {
            const float r = simd_length(out[i].pos.xyz - center);
            inside = inside || r < radius - 1e-3f;
            outside = outside || r > radius + 1e-3f;
        }
        CHECK(inside);
        CHECK(outside);
    }
}

TEST_CASE("append_pivot_crosshair: eye exactly at the pivot emits nothing") {
    const simd_float3 center = {1.0f, -2.0f, 3.0f};
    std::vector<LineVertex> out;
    append_pivot_crosshair(out, center, 0.5f, 0.02f, center, kColorPivot);
    CHECK(out.empty());
}

// --- append_origin_marker ----------------------------------------------------

TEST_CASE("append_origin_marker: +Y shaft in the shared world-axis green, plus a white pip") {
    const simd_float3 eye = {5.0f, 3.0f, 9.0f};
    const float height = 3.0f, hw = 0.02f, pip = 0.05f;

    std::vector<LineVertex> out;
    append_origin_marker(out, height, hw, pip, eye);

    // 1 shaft segment + 1 pip quad, 6 verts each.
    REQUIRE(out.size() == 12);

    SUBCASE("the shaft runs from the origin up +Y only") {
        for (size_t i = 0; i < 6; ++i) {
            CAPTURE(i);
            const simd_float3 p = out[i].pos.xyz;
            CHECK(p.y > -hw - 1e-5f);
            CHECK(p.y < height + hw + 1e-5f);
            CHECK(std::fabs(p.x) < hw + 1e-5f);
            CHECK(std::fabs(p.z) < hw + 1e-5f);
        }
    }

    SUBCASE("the shaft's colour IS the shader's own Y-axis constant") {
        // Not a copy of it: ground_grid.h is dual-compiled, so the +Y marker
        // here and the X/Z lines the fragment shader draws read the same
        // definition. If this ever needs a literal, the axes have drifted.
        for (size_t i = 0; i < 6; ++i) {
            CAPTURE(i);
            CHECK(color_is(out[i], kGroundAxisY));
        }
    }

    SUBCASE("the pip is a small white square at the origin") {
        for (size_t i = 6; i < 12; ++i) {
            CAPTURE(i);
            CHECK(simd_length(out[i].pos.xyz) < 1.4143f * pip + 1e-5f);
            CHECK(color_is(out[i], kColorOriginPip));
        }
    }
}

// --- wireframes for the six shapes added after cube and sphere ---------------

TEST_CASE("append_node_wireframe: every shape draws, and stays inside its own box") {
    // The containment claim the wireframe shares with scene_aabb and
    // node_bounding_radius: each shape is inscribed in position +- scale*0.5.
    // Non-uniform scale on purpose -- the builders draw in the unit box and let
    // the transform stretch them, so this is also the check that they are being
    // stretched rather than drawn at some fixed size.
    const simd_float3 scale = {3.0f, 5.0f, 2.0f};
    const simd_float3 position = {1.0f, -2.0f, 0.5f};
    const simd_float3 eye = {0.0f, 0.0f, 12.0f};

    struct Case { const char* label; Shape shape; float param; };
    const Case cases[] = {
        {"cone, sharp", Shape::Cone, 0.0f},        {"cone, truncated", Shape::Cone, 0.5f},
        {"capsule, flat", Shape::Capsule, 0.0f},   {"capsule, round", Shape::Capsule, 1.0f},
        {"octahedron", Shape::Octahedron, 0.0f},   {"pyramid, sharp", Shape::Pyramid, 0.0f},
        {"pyramid, frustum", Shape::Pyramid, 0.4f},{"prism, 3", Shape::Prism, 3.0f},
        {"prism, 12", Shape::Prism, 12.0f},        {"vesica", Shape::Vesica, 0.0f},
    };

    for (const Case& c : cases) {
        INFO("case: " << std::string(c.label));
        Node node;
        node.id = 1;
        node.shape = c.shape;
        node.shape_param = c.param;
        node.local_position = position;
        node.scale = scale;

        std::vector<LineVertex> lines;
        append_node_wireframe(lines, node, kColorSelected, eye);

        REQUIRE_FALSE(lines.empty());
        CHECK(lines.size() % 2 == 0); // LINE primitives: vertices come in pairs
        const simd_float3 half = 0.5f * scale;
        for (const LineVertex& v : lines) {
            const simd_float3 local = simd_float3{v.pos.x, v.pos.y, v.pos.z} - position;
            CHECK(std::fabs(local.x) <= half.x + 1e-4f);
            CHECK(std::fabs(local.y) <= half.y + 1e-4f);
            CHECK(std::fabs(local.z) <= half.z + 1e-4f);
        }
    }
}

TEST_CASE("append_node_wireframe: the drawn outline tracks the shape parameter") {
    // What makes setNodeShapeParam's set_scene_lines_dirty() load-bearing: the
    // wireframe genuinely changes with the dial, so a stale line buffer would
    // show the previous value.
    const simd_float3 eye = {0.0f, 0.0f, 12.0f};

    SUBCASE("a prism's side count changes its vertex count") {
        Node node;
        node.id = 1;
        node.shape = Shape::Prism;

        std::vector<LineVertex> triangle;
        node.shape_param = 3.0f;
        append_node_wireframe(triangle, node, kColorSelected, eye);

        std::vector<LineVertex> dodecagon;
        node.shape_param = 12.0f;
        append_node_wireframe(dodecagon, node, kColorSelected, eye);

        // Two end rings plus one vertical per side -> 3 segments per side.
        CHECK(triangle.size() == 2u * 3u * 3u);
        CHECK(dodecagon.size() == 2u * 3u * 12u);
    }

    SUBCASE("blunting a cone's tip adds its top ring") {
        Node node;
        node.id = 1;
        node.shape = Shape::Cone;

        std::vector<LineVertex> sharp;
        node.shape_param = 0.0f;
        append_node_wireframe(sharp, node, kColorSelected, eye);

        std::vector<LineVertex> truncated;
        node.shape_param = 0.5f;
        append_node_wireframe(truncated, node, kColorSelected, eye);

        CHECK(truncated.size() == sharp.size() + 2u * kShapeRingSegments);

        // And the tip really is blunt: the sharp cone's apex is a single point
        // on the axis, the truncated one's top ring is not.
        int on_axis_at_top = 0;
        for (const LineVertex& v : truncated) {
            if (v.pos.y > 0.49f && std::hypot(v.pos.x, v.pos.z) < 1e-3f) {
                ++on_axis_at_top;
            }
        }
        CHECK(on_axis_at_top == 0);
    }
}

TEST_CASE("every wireframe vertex lies on the shape's own zero set") {
    // The invariant that makes a wireframe an outline rather than a decoration,
    // and the one this file was missing: it is what catches a builder that
    // ignores the dial. Cube and octahedron did exactly that -- they drew hard
    // edges at every roundness, so a Subtract cube dialled round carved a ball
    // while its wireframe, which is a Subtract node's ONLY visual when
    // unselected, still showed a box.
    //
    // Vertices only. The straight segments between them cut inside a curved
    // surface by design; it is the sampled points that have to be on it.
    struct Case { const char* label; Shape shape; float param; };
    const Case cases[] = {
        {"cube, sharp", Shape::Cube, 0.0f},          {"cube, half round", Shape::Cube, 0.5f},
        {"cube, ball", Shape::Cube, 1.0f},           {"sphere", Shape::Sphere, 0.0f},
        {"cone, sharp", Shape::Cone, 0.0f},          {"cone, truncated", Shape::Cone, 0.45f},
        {"cone, cylinder", Shape::Cone, 1.0f},       {"capsule, flat", Shape::Capsule, 0.0f},
        {"capsule, half", Shape::Capsule, 0.5f},     {"capsule, round", Shape::Capsule, 1.0f},
        {"octahedron, sharp", Shape::Octahedron, 0.0f},
        {"octahedron, half round", Shape::Octahedron, 0.5f},
        {"octahedron, ball", Shape::Octahedron, 1.0f},
        {"pyramid, sharp", Shape::Pyramid, 0.0f},    {"pyramid, frustum", Shape::Pyramid, 0.6f},
        {"prism, 3", Shape::Prism, 3.0f},            {"prism, 7", Shape::Prism, 7.0f},
        {"vesica, cusped", Shape::Vesica, 0.0f},     {"vesica, half round", Shape::Vesica, 0.5f},
        {"vesica, capsule", Shape::Vesica, 1.0f},
    };
    const simd_float3 scales[] = {
        {2.0f, 2.0f, 2.0f},  // uniform
        {3.0f, 5.0f, 2.0f},  // taller than wide
        {5.0f, 1.5f, 4.0f},  // wider than tall
    };
    const simd_float3 eye = {0.0f, 0.0f, 14.0f};

    for (const Case& c : cases) {
        for (const simd_float3& scale : scales) {
            INFO("case: " << std::string(c.label));
            INFO("scale: (" << scale.x << ", " << scale.y << ", " << scale.z << ")");
            SceneDocument doc;
            Node node;
            node.id = 1;
            node.shape = c.shape;
            node.shape_param = c.param;
            node.scale = scale;
            doc.add(node);

            std::vector<LineVertex> lines;
            append_node_wireframe(lines, node, kColorSelected, eye);
            REQUIRE_FALSE(lines.empty());

            float worst = 0.0f;
            for (const LineVertex& v : lines) {
                const auto d = evaluate_scene_sdf(doc, simd_float3{v.pos.x, v.pos.y, v.pos.z});
                REQUIRE(d.has_value());
                worst = std::fmax(worst, std::fabs(*d));
            }
            INFO("worst |sdf| at a wireframe vertex: " << worst);
            CHECK(worst < 2e-3f);
        }
    }
}

TEST_CASE("append_node_wireframe emits no NaN for a degenerate scale") {
    // append_rounded_box_edges divides the rounding radius by the half-extents
    // to get a unit-local flat width. With a zero extent, rb is also zero, so
    // that is 0/0 -- and the NaN goes straight into the line vertex buffer.
    //
    // The shader's cube branch deliberately floors NOTHING, precisely so a
    // zero-scale cube stays benign rather than becoming a special case. The
    // wireframe has to hold up its end of that.
    for (const Shape shape : {Shape::Cube, Shape::Octahedron, Shape::Capsule,
                              Shape::Vesica}) {
        CAPTURE(static_cast<int>(shape));
        Node node;
        node.id = 1;
        node.shape = shape;
        node.scale = {1.0f, 0.0f, 1.0f};
        node.shape_param = 0.5f; // rounding on, so the division is reached

        std::vector<LineVertex> out;
        append_node_wireframe(out, node, simd_float4{1, 1, 1, 1},
                              simd_float3{0.0f, 0.0f, 5.0f});
        int nan_count = 0;
        for (const LineVertex& v : out) {
            if (std::isnan(v.pos.x) || std::isnan(v.pos.y) || std::isnan(v.pos.z)) {
                ++nan_count;
            }
        }
        CAPTURE(out.size());
        CHECK(nan_count == 0);
    }
}
