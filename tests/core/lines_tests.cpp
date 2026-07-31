#include <doctest.h>

#include <array>
#include <cmath>
#include <vector>

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

TEST_CASE("build_scene_lines: colors by op, selection overrides to kColorSelected") {
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

    SUBCASE("no selection") {
        const std::vector<LineVertex> lines =
            build_scene_lines(doc, kInvalidNode, simd_float3{0.0f, 0.0f, 5.0f}, false);
        REQUIRE(lines.size() == 120); // 24 cube + 96 sphere outline

        for (size_t i = 0; i < 24; ++i) {
            CAPTURE(i);
            check_color_approx(lines[i].color, kColorAdd);
        }
        for (size_t i = 24; i < 120; ++i) {
            CAPTURE(i);
            check_color_approx(lines[i].color, kColorSubtract);
        }
    }

    SUBCASE("sphere selected") {
        const std::vector<LineVertex> lines =
            build_scene_lines(doc, sphere.id, simd_float3{0.0f, 0.0f, 5.0f}, false);
        REQUIRE(lines.size() == 120);

        for (size_t i = 0; i < 24; ++i) {
            CAPTURE(i);
            check_color_approx(lines[i].color, kColorAdd);
        }
        for (size_t i = 24; i < 120; ++i) {
            CAPTURE(i);
            check_color_approx(lines[i].color, kColorSelected);
        }
    }
}

TEST_CASE("build_scene_lines: mesh_present=true restricts the wireframe to the selected node only") {
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

    SUBCASE("no selection: nothing is emitted") {
        const std::vector<LineVertex> lines = build_scene_lines(doc, kInvalidNode, eye, true);
        CHECK(lines.empty());
    }

    SUBCASE("cube selected: only the cube's 24 edge vertices") {
        const std::vector<LineVertex> lines = build_scene_lines(doc, cube.id, eye, true);
        REQUIRE(lines.size() == 24);
        for (const LineVertex& v : lines) {
            check_color_approx(v.color, kColorSelected);
        }
    }

    SUBCASE("sphere selected: only the sphere's outline vertices") {
        const std::vector<LineVertex> lines = build_scene_lines(doc, sphere.id, eye, true);
        REQUIRE(lines.size() == 2 * kSphereOutlineSegments);
        for (const LineVertex& v : lines) {
            check_color_approx(v.color, kColorSelected);
        }
    }
}

// --- append_tangent_frame ---------------------------------------------------

TEST_CASE("append_tangent_frame: n={0,1,0}, origin={0,0,0}, he=2, divisions=12: counts, "
          "coplanarity, and the stub's far endpoint") {
    std::vector<LineVertex> out;
    append_tangent_frame(out, simd_float3{0.0f, 0.0f, 0.0f}, simd_float3{0.0f, 1.0f, 0.0f}, 2.0f, 12);

    // 24 grid lines (2*divisions) + 2 axis lines + 1 normal stub = 27 lines = 54 vertices.
    REQUIRE(out.size() == 54);

    const simd_float3 origin = {0.0f, 0.0f, 0.0f};
    const simd_float3 normal = {0.0f, 1.0f, 0.0f};
    for (size_t i = 0; i < out.size() - 1; ++i) { // every vertex except the stub's far endpoint (last)
        CAPTURE(i);
        const simd_float3 p = out[i].pos.xyz;
        CHECK(std::fabs(simd_dot(normal, p - origin)) < 1e-5f);
    }

    // Final vertex: the stub's far endpoint, origin + n * (0.5 * he) = origin + n * 1.0.
    check_float3_approx(out.back().pos.xyz, simd_float3{0.0f, 1.0f, 0.0f});

    // Color counts: 48 grid-line verts (alpha 0.18), 6 axis/stub verts (alpha 0.9:
    // 2 axes * 2 endpoints + stub * 2 endpoints).
    int grid_count = 0;
    int axis_count = 0;
    for (const LineVertex& v : out) {
        if (v.color.w == doctest::Approx(0.18f)) {
            ++grid_count;
        } else if (v.color.w == doctest::Approx(0.9f)) {
            ++axis_count;
        }
    }
    CHECK(grid_count == 48);
    CHECK(axis_count == 6);
}

TEST_CASE("append_tangent_frame: tangent basis is always orthonormal, both branches of the |n.y| pick") {
    auto check_orthonormal_basis = [](simd_float3 n) {
        CAPTURE(n.x);
        CAPTURE(n.y);
        CAPTURE(n.z);
        const simd_float3 ref =
            (std::fabs(n.y) < 0.99f) ? simd_float3{0.0f, 1.0f, 0.0f} : simd_float3{1.0f, 0.0f, 0.0f};
        const simd_float3 u = simd_normalize(simd_cross(n, ref));
        const simd_float3 v = simd_cross(n, u);

        CHECK(simd_length(u) == doctest::Approx(1.0f));
        CHECK(simd_length(v) == doctest::Approx(1.0f));
        CHECK(std::fabs(simd_dot(u, n)) < 1e-5f);
        CHECK(std::fabs(simd_dot(v, n)) < 1e-5f);
        CHECK(std::fabs(simd_dot(u, v)) < 1e-5f);
    };

    // n = {0,1,0}: |n.y| = 1.0 >= 0.99, takes the {1,0,0} reference branch.
    check_orthonormal_basis(simd_float3{0.0f, 1.0f, 0.0f});
    // n = normalize({1,0,1}): |n.y| = 0 < 0.99, takes the {0,1,0} reference branch.
    check_orthonormal_basis(simd_normalize(simd_float3{1.0f, 0.0f, 1.0f}));
}

TEST_CASE("append_tangent_frame: off-origin, tilted normal — every non-stub vertex is coplanar "
          "through origin") {
    const simd_float3 origin = {1.0f, 2.0f, 3.0f};
    const simd_float3 normal = simd_normalize(simd_float3{1.0f, 1.0f, 0.0f});

    std::vector<LineVertex> out;
    append_tangent_frame(out, origin, normal, 2.0f, 12);

    REQUIRE(out.size() == 54);
    for (size_t i = 0; i < out.size() - 1; ++i) { // every vertex except the stub's far endpoint (last)
        CAPTURE(i);
        const simd_float3 p = out[i].pos.xyz;
        CHECK(std::fabs(simd_dot(normal, p - origin)) < 1e-5f);
    }
}
