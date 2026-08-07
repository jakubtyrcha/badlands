#include <doctest.h>

#include <cmath>

#include "frame.h"
#include "math_util.h" // trs_matrix

using namespace sq;

namespace {

// Parameters are `const`: doctest's CHECK() binds the compared sub-expression
// to a reference, and Clang only allows that for *const* accesses of
// ext_vector_type components -- matches scene_tests.cpp/lines_tests.cpp.
void check_float3_approx(const simd_float3 actual, const simd_float3 expected) {
    CHECK(actual.x == doctest::Approx(expected.x));
    CHECK(actual.y == doctest::Approx(expected.y));
    CHECK(actual.z == doctest::Approx(expected.z));
}

// Quaternions q and -q are the same rotation, so compare by what they DO rather
// than by their components -- otherwise a sign flip inside compose would read as
// a failure when nothing rotated differently.
void check_rotation_approx(const simd_quatf actual, const simd_quatf expected) {
    const simd_float3 probes[] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}, {0.3f, -0.7f, 0.5f}};
    for (const simd_float3 p : probes) {
        check_float3_approx(simd_act(actual, p), simd_act(expected, p));
    }
}

void check_frame_approx(const Frame& actual, const Frame& expected) {
    check_float3_approx(actual.position, expected.position);
    check_rotation_approx(actual.rotation, expected.rotation);
    CHECK(actual.uniform_scale == doctest::Approx(expected.uniform_scale));
}

Frame make_frame(simd_float3 position, simd_quatf rotation, float uniform_scale) {
    Frame f;
    f.position = position;
    f.rotation = rotation;
    f.uniform_scale = uniform_scale;
    return f;
}

} // namespace

// --- compose ---------------------------------------------------------------

TEST_CASE("compose with an identity parent returns the local frame") {
    const Frame local = make_frame(simd_float3{1, 2, 3}, simd_quaternion(0.7f, simd_float3{0, 1, 0}), 2.0f);
    check_frame_approx(compose(Frame{}, local), local);
}

TEST_CASE("compose with an identity local returns the parent frame") {
    const Frame parent = make_frame(simd_float3{-4, 1, 0}, simd_quaternion(1.1f, simd_float3{1, 0, 0}), 3.0f);
    check_frame_approx(compose(parent, Frame{}), parent);
}

TEST_CASE("compose is associative") {
    const Frame a = make_frame(simd_float3{1, 0, 0}, simd_quaternion(0.4f, simd_normalize(simd_float3{1, 1, 0})), 2.0f);
    const Frame b = make_frame(simd_float3{0, 2, 0}, simd_quaternion(-0.9f, simd_float3{0, 1, 0}), 0.5f);
    const Frame c = make_frame(simd_float3{0, 0, 3}, simd_quaternion(1.3f, simd_normalize(simd_float3{0, 1, 1})), 1.5f);

    check_frame_approx(compose(compose(a, b), c), compose(a, compose(b, c)));
}

// The Group rule: a parent's uniform scale reaches its child's OFFSET, not just
// its size. Scaling an assembly has to move its parts apart, or it only inflates
// them in place.
TEST_CASE("a parent's uniform scale scales the child's offset") {
    const Frame parent = make_frame(simd_float3{0, 0, 0}, simd_quaternion(0.f, 0.f, 0.f, 1.f), 2.0f);
    const Frame local = make_frame(simd_float3{1, 0, 0}, simd_quaternion(0.f, 0.f, 0.f, 1.f), 1.0f);

    const Frame out = compose(parent, local);
    check_float3_approx(out.position, simd_float3{2, 0, 0});
    CHECK(out.uniform_scale == doctest::Approx(2.0f));
}

TEST_CASE("uniform scale composes multiplicatively through a chain") {
    const Frame a = make_frame(simd_float3{0, 0, 0}, simd_quaternion(0.f, 0.f, 0.f, 1.f), 2.0f);
    const Frame b = make_frame(simd_float3{0, 0, 0}, simd_quaternion(0.f, 0.f, 0.f, 1.f), 3.0f);
    const Frame c = make_frame(simd_float3{0, 0, 0}, simd_quaternion(0.f, 0.f, 0.f, 1.f), 1.0f);

    CHECK(compose(compose(a, b), c).uniform_scale == doctest::Approx(6.0f));
}

// Right-handed, Y up: rotating +90 degrees about +Y sends +X to -Z.
TEST_CASE("a parent's rotation swings the child's offset") {
    const Frame parent =
        make_frame(simd_float3{0, 0, 0}, simd_quaternion(float(M_PI_2), simd_float3{0, 1, 0}), 1.0f);
    const Frame local = make_frame(simd_float3{1, 0, 0}, simd_quaternion(0.f, 0.f, 0.f, 1.f), 1.0f);

    check_float3_approx(compose(parent, local).position, simd_float3{0, 0, -1});
}

// pack_scene packs the CONJUGATE as the inverse, and those agree only for a unit
// quaternion. compose is where a chain's drift is stopped, so a denormalized
// input must come back normalized rather than being trusted.
TEST_CASE("compose renormalizes its rotation") {
    Frame local;
    local.rotation = simd_quaternion(simd_float4{0.0f, 0.6f, 0.0f, 0.8f} * 1.05f);
    REQUIRE(simd_length(local.rotation.vector) > 1.04f); // denormalized on the way in

    const Frame out = compose(Frame{}, local);
    CHECK(simd_length(out.rotation.vector) == doctest::Approx(1.0f));
}

TEST_CASE("compose renormalizes through a long chain") {
    Frame drifting;
    drifting.rotation = simd_quaternion(simd_float4{0.0f, 0.6f, 0.0f, 0.8f} * 1.01f);

    Frame acc;
    for (int i = 0; i < 64; ++i) {
        acc = compose(acc, drifting);
    }
    CHECK(simd_length(acc.rotation.vector) == doctest::Approx(1.0f));
}

// --- relative_to -----------------------------------------------------------

TEST_CASE("relative_to inverts compose") {
    const Frame parent = make_frame(simd_float3{3, -1, 2}, simd_quaternion(0.8f, simd_normalize(simd_float3{1, 2, 3})), 2.0f);
    const Frame world = make_frame(simd_float3{-1, 4, 0}, simd_quaternion(-0.5f, simd_normalize(simd_float3{0, 1, 1})), 3.0f);

    check_frame_approx(compose(parent, relative_to(parent, world)), world);
}

TEST_CASE("relative_to of a frame against itself is the identity") {
    const Frame f = make_frame(simd_float3{1, 2, 3}, simd_quaternion(0.3f, simd_float3{0, 0, 1}), 2.0f);

    const Frame local = relative_to(f, f);
    check_float3_approx(local.position, simd_float3{0, 0, 0});
    CHECK(local.uniform_scale == doctest::Approx(1.0f));
}

TEST_CASE("relative_to treats a degenerate parent scale as 1") {
    const Frame parent = make_frame(simd_float3{0, 0, 0}, simd_quaternion(0.f, 0.f, 0.f, 1.f), 0.0f);
    const Frame world = make_frame(simd_float3{2, 0, 0}, simd_quaternion(0.f, 0.f, 0.f, 1.f), 1.0f);

    const Frame local = relative_to(parent, world);
    check_float3_approx(local.position, simd_float3{2, 0, 0}); // not inf, not NaN
    CHECK(std::isfinite(local.uniform_scale));
}

// --- frame_from_matrix: the sanitization boundary --------------------------

TEST_CASE("frame_from_matrix recovers a rigid transform exactly") {
    const simd_float3 position{1, -2, 3};
    const simd_quatf rotation = simd_quaternion(0.9f, simd_normalize(simd_float3{1, 1, 0}));
    const simd_float4x4 m = trs_matrix(position, rotation, simd_float3{1, 1, 1});

    const Frame f = frame_from_matrix(m);
    check_float3_approx(f.position, position);
    check_rotation_approx(f.rotation, rotation);
    CHECK(f.uniform_scale == doctest::Approx(1.0f));
}

TEST_CASE("frame_from_matrix recovers a uniform scale exactly") {
    const simd_quatf rotation = simd_quaternion(0.4f, simd_float3{0, 1, 0});
    const simd_float4x4 m = trs_matrix(simd_float3{0, 0, 0},rotation, simd_float3{2.5f, 2.5f, 2.5f});

    const Frame f = frame_from_matrix(m);
    check_rotation_approx(f.rotation, rotation);
    CHECK(f.uniform_scale == doctest::Approx(2.5f));
}

// Non-uniform scale has no home in a Frame, so it collapses to the mean of the
// basis vectors' lengths. Lossy AND deliberate: SdfNode cannot hold the rest.
TEST_CASE("frame_from_matrix collapses non-uniform scale to the mean") {
    const simd_float4x4 m = trs_matrix(simd_float3{0, 0, 0},simd_quaternion(0.f, 0.f, 0.f, 1.f),
                                       simd_float3{2, 4, 8});

    const Frame f = frame_from_matrix(m);
    CHECK(f.uniform_scale == doctest::Approx((2.0f + 4.0f + 8.0f) / 3.0f));
}

// The case the seam exists for: a joint matrix carrying shear must not be able
// to put shear into the document. What comes back is a rotation -- orthonormal,
// by construction.
TEST_CASE("frame_from_matrix discards shear") {
    simd_float4x4 sheared = matrix_identity_float4x4;
    sheared.columns[1] = simd_float4{1, 1, 0, 0}; // Y leans into X: not orthogonal

    const Frame f = frame_from_matrix(sheared);

    const simd_float3x3 r = simd_matrix3x3(f.rotation);
    const simd_float3 u = r.columns[0], v = r.columns[1], n = r.columns[2];
    CHECK(simd_length(u) == doctest::Approx(1.0f));
    CHECK(simd_length(v) == doctest::Approx(1.0f));
    CHECK(simd_length(n) == doctest::Approx(1.0f));
    CHECK(simd_dot(u, v) == doctest::Approx(0.0f).epsilon(1e-4));
    CHECK(simd_dot(u, n) == doctest::Approx(0.0f).epsilon(1e-4));
    CHECK(simd_dot(v, n) == doctest::Approx(0.0f).epsilon(1e-4));
}

TEST_CASE("frame_from_matrix keeps translation when discarding shear") {
    simd_float4x4 sheared = matrix_identity_float4x4;
    sheared.columns[1] = simd_float4{1, 1, 0, 0};
    sheared.columns[3] = simd_float4{5, 6, 7, 1};

    check_float3_approx(frame_from_matrix(sheared).position, simd_float3{5, 6, 7});
}

// A NaN here would propagate silently into every descendant's frame, so a
// degenerate basis degrades to identity rather than to infinities.
TEST_CASE("frame_from_matrix survives a degenerate matrix") {
    simd_float4x4 collapsed = matrix_identity_float4x4;
    collapsed.columns[0] = simd_float4{0, 0, 0, 0};
    collapsed.columns[1] = simd_float4{0, 0, 0, 0};
    collapsed.columns[2] = simd_float4{0, 0, 0, 0};
    collapsed.columns[3] = simd_float4{1, 2, 3, 1};

    const Frame f = frame_from_matrix(collapsed);
    check_float3_approx(f.position, simd_float3{1, 2, 3});
    CHECK(f.uniform_scale == doctest::Approx(1.0f));
    CHECK(simd_length(f.rotation.vector) == doctest::Approx(1.0f));
    check_float3_approx(simd_act(f.rotation, simd_float3{1, 0, 0}), simd_float3{1, 0, 0});
}
