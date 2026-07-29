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

TEST_CASE("append_sphere_circles: identity transform emits 192 vertices on the unit-radius sphere") {
    std::vector<LineVertex> out;
    append_sphere_circles(out, matrix_identity_float4x4, kColorSubtract);

    REQUIRE(out.size() == 192);

    for (const LineVertex& v : out) {
        const simd_float3 p = v.pos.xyz;
        CHECK(simd_length(p) == doctest::Approx(0.5f));
    }

    // Circles are emitted axis X, then Y, then Z, 64 vertices (32 segments * 2
    // endpoints) each; axis X's circle lies in the local x=0 plane, etc.
    for (size_t i = 0; i < 64; ++i) {
        const LineVertex& v = out[i];
        CHECK(v.pos.x == doctest::Approx(0.0f));
    }
    for (size_t i = 64; i < 128; ++i) {
        const LineVertex& v = out[i];
        CHECK(v.pos.y == doctest::Approx(0.0f));
    }
    for (size_t i = 128; i < 192; ++i) {
        const LineVertex& v = out[i];
        CHECK(v.pos.z == doctest::Approx(0.0f));
    }
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
        const std::vector<LineVertex> lines = build_scene_lines(doc, kInvalidNode);
        REQUIRE(lines.size() == 216); // 24 cube + 192 sphere

        for (size_t i = 0; i < 24; ++i) {
            CAPTURE(i);
            check_color_approx(lines[i].color, kColorAdd);
        }
        for (size_t i = 24; i < 216; ++i) {
            CAPTURE(i);
            check_color_approx(lines[i].color, kColorSubtract);
        }
    }

    SUBCASE("sphere selected") {
        const std::vector<LineVertex> lines = build_scene_lines(doc, sphere.id);
        REQUIRE(lines.size() == 216);

        for (size_t i = 0; i < 24; ++i) {
            CAPTURE(i);
            check_color_approx(lines[i].color, kColorAdd);
        }
        for (size_t i = 24; i < 216; ++i) {
            CAPTURE(i);
            check_color_approx(lines[i].color, kColorSelected);
        }
    }
}
