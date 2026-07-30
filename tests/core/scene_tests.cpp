#include <doctest.h>

#include <string>

#include "scene.h"

using namespace sq;

namespace {

// Parameters are `const`: doctest's CHECK() binds the compared sub-expression
// to a reference, and Clang only allows that for *const* accesses of
// ext_vector_type components (e.g. `simd_float3.x`) — matches the pattern
// already used in lines_tests.cpp/picking_tests.cpp.
void check_float3_approx(const simd_float3 actual, const simd_float3 expected) {
    CHECK(actual.x == doctest::Approx(expected.x));
    CHECK(actual.y == doctest::Approx(expected.y));
    CHECK(actual.z == doctest::Approx(expected.z));
}

} // namespace

// --- naming / ids ----------------------------------------------------------

TEST_CASE("SceneDocument::spawn_unsnapped: auto-names by per-shape counter, ids by spawn order") {
    SceneDocument doc;

    const int32_t cube1 = doc.spawn_unsnapped(Shape::Cube, Op::Add, {0.0f, 0.0f, 0.0f});
    const int32_t sphere1 = doc.spawn_unsnapped(Shape::Sphere, Op::Subtract, {1.0f, 0.0f, 0.0f});
    const int32_t cube2 = doc.spawn_unsnapped(Shape::Cube, Op::Subtract, {2.0f, 0.0f, 0.0f});

    CHECK(cube1 == 1);
    CHECK(sphere1 == 2);
    CHECK(cube2 == 3);

    REQUIRE(doc.find(cube1) != nullptr);
    REQUIRE(doc.find(sphere1) != nullptr);
    REQUIRE(doc.find(cube2) != nullptr);

    CHECK(doc.find(cube1)->name == "Cube 1");
    CHECK(doc.find(sphere1)->name == "Sphere 1");
    CHECK(doc.find(cube2)->name == "Cube 2");

    CHECK(doc.find(cube1)->op == Op::Add);
    CHECK(doc.find(sphere1)->op == Op::Subtract);
    CHECK(doc.find(cube2)->op == Op::Subtract);
}

TEST_CASE("SceneDocument: per-shape name counters are independent of node ids") {
    // No removal API exists yet, so "independent of ids" is exercised by
    // interleaving shapes: the id sequence is contiguous (1,2,3) while each
    // shape's own counter only advances on its own spawns (Cube: 1,2;
    // Sphere: 1).
    SceneDocument doc;

    const int32_t a = doc.spawn_unsnapped(Shape::Cube, Op::Add, {0.0f, 0.0f, 0.0f});
    const int32_t b = doc.spawn_unsnapped(Shape::Cube, Op::Add, {0.0f, 0.0f, 0.0f});
    const int32_t c = doc.spawn_unsnapped(Shape::Sphere, Op::Add, {0.0f, 0.0f, 0.0f});

    CHECK(a == 1);
    CHECK(b == 2);
    CHECK(c == 3);

    CHECK(doc.find(a)->name == "Cube 1");
    CHECK(doc.find(b)->name == "Cube 2");
    CHECK(doc.find(c)->name == "Sphere 1");
}

// --- spawn_snapped -----------------------------------------------------------

TEST_CASE("SceneDocument::spawn_snapped: axis-aligned normal places the node on the surface") {
    SceneDocument doc;

    const simd_float3 hit = {1.0f, 2.0f, 3.0f};
    const simd_float3 normal = {0.0f, 1.0f, 0.0f};
    const int32_t id = doc.spawn_snapped(Shape::Cube, Op::Add, hit, normal, 7);

    const Node* node = doc.find(id);
    REQUIRE(node != nullptr);

    // position = hit + unit_normal * 0.5 -> {1, 2.5, 3}
    check_float3_approx(node->position, simd_float3{1.0f, 2.5f, 3.0f});
    CHECK(node->snapped == true);
    check_float3_approx(node->snap_point, hit);
    check_float3_approx(node->snap_normal, normal);
    CHECK(node->snap_parent == 7);
}

TEST_CASE("SceneDocument::spawn_snapped: non-axis normal offsets by 0.5 along the normal") {
    SceneDocument doc;

    const simd_float3 hit = {4.0f, 5.0f, 6.0f};
    const simd_float3 normal = simd_normalize(simd_float3{1.0f, 1.0f, 0.0f}); // (~0.7071, ~0.7071, 0)
    const int32_t id = doc.spawn_snapped(Shape::Sphere, Op::Subtract, hit, normal, 3);

    const Node* node = doc.find(id);
    REQUIRE(node != nullptr);

    const simd_float3 expected_position = hit + normal * 0.5f;
    check_float3_approx(node->position, expected_position);
    CHECK(node->snapped == true);
    check_float3_approx(node->snap_point, hit);
    check_float3_approx(node->snap_normal, normal);
    CHECK(node->snap_parent == 3);
}

// --- spawn_unsnapped ---------------------------------------------------------

TEST_CASE("SceneDocument::spawn_unsnapped: position passed through unmodified, not snapped") {
    SceneDocument doc;

    const simd_float3 position = {-2.0f, 4.5f, 8.0f};
    const int32_t id = doc.spawn_unsnapped(Shape::Cube, Op::Add, position);

    const Node* node = doc.find(id);
    REQUIRE(node != nullptr);

    check_float3_approx(node->position, position);
    CHECK(node->snapped == false);
    CHECK(node->snap_parent == kInvalidNode);
}
