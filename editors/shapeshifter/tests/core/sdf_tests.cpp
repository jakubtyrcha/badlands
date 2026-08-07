#include <doctest.h>

#include <cmath>
#include <string>
#include <vector>

#include <shapeshifter/ShapeshifterCore.h>

#include "sdf.h"
#include "scene.h"

using namespace sq;

namespace {

// Parameters are `const`: doctest's CHECK() binds the compared sub-expression
// to a reference, and Clang only allows that for *const* accesses of
// ext_vector_type components (e.g. `simd_float3.x`) — matches the pattern
// already used in scene_tests.cpp/picking_tests.cpp.
void check_float3_approx(const simd_float3 actual, const simd_float3 expected) {
    CHECK(actual.x == doctest::Approx(expected.x));
    CHECK(actual.y == doctest::Approx(expected.y));
    CHECK(actual.z == doctest::Approx(expected.z));
}

// Flat-index convention documented on SampleGrid::values: index = x + n*(y + n*z).
size_t flat_index(int32_t x, int32_t y, int32_t z, int32_t n) {
    const size_t un = static_cast<size_t>(n);
    return static_cast<size_t>(x) + un * (static_cast<size_t>(y) + un * static_cast<size_t>(z));
}

} // namespace

// All pinned literals below are derived independently with numpy, reproducing
// only the *formulas from the brief* (never by running this codebase's C++).
// Shared derivation, reusable for every case in this file:
//
//   python3 -c "
//   import numpy as np
//   def sd_box(q, b):
//       q, b = np.array(q, dtype=np.float64), np.array(b, dtype=np.float64)
//       d = np.abs(q) - b
//       return np.linalg.norm(np.maximum(d, 0.0)) + min(np.max(d), 0.0)
//   def sd_ellipsoid(q, r):
//       q, r = np.array(q, dtype=np.float64), np.array(r, dtype=np.float64)
//       k0 = np.linalg.norm(q / r)
//       k1 = np.linalg.norm(q / (r * r))
//       return -np.min(r) if k1 == 0.0 else k0 * (k0 - 1.0) / k1
//   "
//
// Per-case invocations are given as short comments right above each assertion.

// --- sd_box ------------------------------------------------------------------

TEST_CASE("sd_box: nonuniform half-extents, pinned against numpy sd_box(q, b)") {
    // b = (1, 2, 3) for every case below.
    const simd_float3 b = {1.0f, 2.0f, 3.0f};

    SUBCASE("inside -> negative, no length(...) contribution (max(d,0) all zero)") {
        // sd_box((0.5,1.0,-1.5), (1,2,3)) -> -0.5
        CHECK(sd_box(simd_float3{0.5f, 1.0f, -1.5f}, b) == doctest::Approx(-0.5f));
    }

    SUBCASE("outside face-region: exactly one axis exceeds its half-extent") {
        // sd_box((2.0,0.5,-1.0), (1,2,3)) -> 1.0
        CHECK(sd_box(simd_float3{2.0f, 0.5f, -1.0f}, b) == doctest::Approx(1.0f));
    }

    SUBCASE("outside edge-region: two axes exceed (both max(...) branches engaged)") {
        // sd_box((2.0,3.0,0.5), (1,2,3)) -> sqrt(2) = 1.4142135623730951
        CHECK(sd_box(simd_float3{2.0f, 3.0f, 0.5f}, b) == doctest::Approx(1.4142135623730951f).epsilon(1e-5));
    }

    SUBCASE("outside corner-region: all three axes exceed") {
        // sd_box((2.0,3.0,4.0), (1,2,3)) -> sqrt(3) = 1.7320508075688772
        CHECK(sd_box(simd_float3{2.0f, 3.0f, 4.0f}, b) == doctest::Approx(1.7320508075688772f).epsilon(1e-5));
    }

    SUBCASE("on-surface: face center along x -> exactly 0") {
        // sd_box((1.0,0.0,0.0), (1,2,3)) -> 0.0
        CHECK(sd_box(simd_float3{1.0f, 0.0f, 0.0f}, b) == doctest::Approx(0.0f));
    }
}

// --- sd_ellipsoid --------------------------------------------------------------

TEST_CASE("sd_ellipsoid: uniform radii reduces exactly to length(q) - r") {
    // r = (2, 2, 2) for every case below; iq's formula is proven algebraically
    // to collapse to length(q) - r when r is uniform (k0 = L/r, k1 = L/r^2,
    // d = k0(k0-1)/k1 = r*(L/r - 1) = L - r) — asserted here against both the
    // iq-formula numpy evaluation and the closed form, which numpy confirms agree.
    const simd_float3 r = {2.0f, 2.0f, 2.0f};

    SUBCASE("3-4-5 triangle: q=(3,4,0) -> length 5 -> d=3") {
        CHECK(sd_ellipsoid(simd_float3{3.0f, 4.0f, 0.0f}, r) == doctest::Approx(3.0f));
    }
    SUBCASE("axis-aligned: q=(0,0,-6) -> length 6 -> d=4") {
        CHECK(sd_ellipsoid(simd_float3{0.0f, 0.0f, -6.0f}, r) == doctest::Approx(4.0f));
    }
    SUBCASE("diagonal: q=(1,1,1) -> length sqrt(3) -> d=sqrt(3)-2 = -0.2679491924311228") {
        CHECK(sd_ellipsoid(simd_float3{1.0f, 1.0f, 1.0f}, r) == doctest::Approx(-0.2679491924311228f).epsilon(1e-5));
    }
}

TEST_CASE("sd_ellipsoid: nonuniform radii pinned against numpy's iq-formula evaluation") {
    // r = (1, 2, 3) for both cases below.
    const simd_float3 r = {1.0f, 2.0f, 3.0f};

    SUBCASE("q=(0.5,1.0,1.5) -> sd_ellipsoid(q,r) = -0.19890069220189488") {
        CHECK(sd_ellipsoid(simd_float3{0.5f, 1.0f, 1.5f}, r) == doctest::Approx(-0.19890069220189488f).epsilon(1e-5));
    }
    SUBCASE("q=(3.0,0.0,0.0) (single-axis, clean sanity check) -> sd_ellipsoid(q,r) = 2.0") {
        CHECK(sd_ellipsoid(simd_float3{3.0f, 0.0f, 0.0f}, r) == doctest::Approx(2.0f));
    }
}

TEST_CASE("sd_ellipsoid: origin guard (q == 0, k1 == 0) returns -min_component(r)") {
    // sd_ellipsoid((0,0,0), (1,2,3)) -> -1.0 (min(1,2,3) = 1)
    CHECK(sd_ellipsoid(simd_float3{0.0f, 0.0f, 0.0f}, simd_float3{1.0f, 2.0f, 3.0f}) == doctest::Approx(-1.0f));
}

// --- evaluate_scene_sdf: CSG combine -------------------------------------------

TEST_CASE("evaluate_scene_sdf: two overlapping Add spheres combine via min") {
    // Sphere A: position (0,0,0), scale (2,2,2) -> r=1 (sd = length(q)-1).
    // Sphere B: position (1,0,0), scale (2,2,2) -> r=1.
    // python3: dA=sd_ellipsoid(p-(0,0,0), (1,1,1)); dB=sd_ellipsoid(p-(1,0,0), (1,1,1))
    SceneDocument doc;
    Node a;
    a.id = 1;
    a.shape = Shape::Sphere;
    a.op = Op::Add;
    a.position = {0.0f, 0.0f, 0.0f};
    a.scale = {2.0f, 2.0f, 2.0f};
    doc.add(a);

    Node b;
    b.id = 2;
    b.shape = Shape::Sphere;
    b.op = Op::Add;
    b.position = {1.0f, 0.0f, 0.0f};
    b.scale = {2.0f, 2.0f, 2.0f};
    doc.add(b);

    SUBCASE("closer to A: p=(-0.3,0,0) -> dA=-0.7, dB=0.3 -> min=-0.7") {
        const auto d = evaluate_scene_sdf(doc, simd_float3{-0.3f, 0.0f, 0.0f});
        REQUIRE(d.has_value());
        CHECK(*d == doctest::Approx(-0.7f));
    }
    SUBCASE("closer to B: p=(1.3,0,0) -> dA=0.3, dB=-0.7 -> min=-0.7") {
        const auto d = evaluate_scene_sdf(doc, simd_float3{1.3f, 0.0f, 0.0f});
        REQUIRE(d.has_value());
        CHECK(*d == doctest::Approx(-0.7f));
    }
    SUBCASE("far outside both: p=(5,0,0) -> dA=4, dB=3 -> min=3") {
        const auto d = evaluate_scene_sdf(doc, simd_float3{5.0f, 0.0f, 0.0f});
        REQUIRE(d.has_value());
        CHECK(*d == doctest::Approx(3.0f));
    }
}

TEST_CASE("evaluate_scene_sdf: Add sphere then Subtract box carves the box out; "
          "op order matters (Subtract-before-Add differs from Add-before-Subtract)") {
    // Sphere: position (0,0,0), scale (4,4,4) -> r=2. sd_ellipsoid((1,0,0),(2,2,2)) = -1.0
    // Box: position (1,0,0), scale (2,2,2) -> half=(1,1,1). Probe (1,0,0) is the box
    // center, so q_box=(0,0,0): sd_box((0,0,0),(1,1,1)) = -1.0 (numpy, same formula as
    // sd_box above).
    //
    // Node-order accumulation (brief: d starts +FLT_MAX, iterated in document order):
    //   Add-then-Subtract: d=min(FLT_MAX,-1)=-1; d=max(-1,-(-1))=max(-1,1)=1.
    //   Subtract-then-Add: d=max(FLT_MAX,-(-1))=max(FLT_MAX,1)=FLT_MAX (unaffected —
    //     nothing has been added yet to subtract from); d=min(FLT_MAX,-1)=-1.
    const simd_float3 probe = {1.0f, 0.0f, 0.0f};

    Node sphere;
    sphere.id = 1;
    sphere.shape = Shape::Sphere;
    sphere.op = Op::Add;
    sphere.position = {0.0f, 0.0f, 0.0f};
    sphere.scale = {4.0f, 4.0f, 4.0f};

    Node box;
    box.id = 2;
    box.shape = Shape::Cube;
    box.op = Op::Subtract;
    box.position = {1.0f, 0.0f, 0.0f};
    box.scale = {2.0f, 2.0f, 2.0f};

    SUBCASE("Add sphere, then Subtract box: carved region reads positive (outside)") {
        SceneDocument doc;
        doc.add(sphere);
        doc.add(box);
        const auto d = evaluate_scene_sdf(doc, probe);
        REQUIRE(d.has_value());
        CHECK(*d == doctest::Approx(1.0f));
    }

    SUBCASE("Subtract box, then Add sphere: box op is a no-op (nothing to subtract from "
            "yet); result is just the sphere") {
        SceneDocument doc;
        doc.add(box);
        doc.add(sphere);
        const auto d = evaluate_scene_sdf(doc, probe);
        REQUIRE(d.has_value());
        CHECK(*d == doctest::Approx(-1.0f));
    }
}

// --- evaluate_scene_sdf: node rotation -----------------------------------------
//
// These go through evaluate_scene_sdf rather than sdf_eval_node directly, on
// purpose: that path runs pack_scene too, so a rotation packed the wrong way
// round (the rotation instead of its conjugate) fails here as well. Every
// expectation below is paired with the value a rotation-BLIND implementation
// would return, so the case is self-evidently discriminating rather than merely
// passing -- one of them even differs in sign.
//
// Derivation extends this file's shared numpy snippet with a Rodrigues rotation
// and applies it INVERSELY to the world offset, which is what evaluating in the
// node's own frame means:
//
//   def rot(axis, ang, v):
//       a = np.array(axis, float); a /= np.linalg.norm(a); v = np.array(v, float)
//       c, s = np.cos(ang), np.sin(ang)
//       return v*c + np.cross(a, v)*s + a*np.dot(a, v)*(1-c)
//   q = rot(axis, -angle, p - position);  sd_box(q, half)   # or sd_ellipsoid(q, radii)

TEST_CASE("evaluate_scene_sdf: a rotated box is measured in its own frame") {
    // Cube at the origin, scale (2, 1, 6) -> half = (1, 0.5, 3), turned 45 deg
    // about +Y. The long axis is local z, so after the turn it runs diagonally
    // through world xz -- which is why an unrotated evaluation is wrong by a
    // lot at every probe below rather than only near the corners.
    SceneDocument doc;
    Node box;
    box.id = 1;
    box.shape = Shape::Cube;
    box.op = Op::Add;
    box.position = {0.0f, 0.0f, 0.0f};
    box.scale = {2.0f, 1.0f, 6.0f};
    box.rotation = simd_quaternion(static_cast<float>(M_PI_4), simd_float3{0.0f, 1.0f, 0.0f});
    doc.add(box);

    struct Case { simd_float3 p; float expected; float blind; };
    const Case cases[] = {
        // q = (1.41421356, 0, 1.41421356)
        {{2.0f, 0.0f, 0.0f}, 0.414213562f, 1.0f},
        // q = (-1.41421356, 0, 1.41421356) -- blind evaluation puts this INSIDE
        {{0.0f, 0.0f, 2.0f}, 0.414213562f, -0.5f},
        // q = (1.76776695, 0.25, 0.35355339)
        {{1.5f, 0.25f, -1.0f}, 0.767766953f, 0.5f},
    };
    for (const Case& c : cases) {
        INFO("probe: (" << c.p.x << ", " << c.p.y << ", " << c.p.z << ")");
        const auto d = evaluate_scene_sdf(doc, c.p);
        REQUIRE(d.has_value());
        CHECK(*d == doctest::Approx(c.expected));
        // The guard that makes this case worth having: the rotated answer is
        // nowhere near the unrotated one.
        CHECK(*d != doctest::Approx(c.blind));
    }
}

TEST_CASE("evaluate_scene_sdf: a rotated ellipsoid is measured in its own frame") {
    // Oblique axis and non-uniform radii together, so neither the axis nor the
    // shape can accidentally be symmetric about the rotation.
    SceneDocument doc;
    Node ellipsoid;
    ellipsoid.id = 1;
    ellipsoid.shape = Shape::Sphere;
    ellipsoid.op = Op::Add;
    ellipsoid.position = {0.0f, 0.0f, 0.0f};
    ellipsoid.scale = {1.0f, 3.0f, 2.0f}; // radii = (0.5, 1.5, 1.0)
    ellipsoid.rotation =
        simd_quaternion(0.7f, simd_normalize(simd_float3{1.0f, 2.0f, 3.0f}));
    doc.add(ellipsoid);

    struct Case { simd_float3 p; float expected; float blind; };
    const Case cases[] = {
        {{1.0f, 0.5f, -0.75f}, 0.797427161f, 0.616266097f},
        {{-0.3f, 1.2f, 0.4f}, 0.038813708f, 0.060438534f},
    };
    for (const Case& c : cases) {
        INFO("probe: (" << c.p.x << ", " << c.p.y << ", " << c.p.z << ")");
        const auto d = evaluate_scene_sdf(doc, c.p);
        REQUIRE(d.has_value());
        CHECK(*d == doctest::Approx(c.expected));
        CHECK(*d != doctest::Approx(c.blind));
    }
}

TEST_CASE("evaluate_scene_sdf: identity rotation is exactly the unrotated result") {
    // The regression guard for every pinned expectation in this file: Node's
    // default rotation is identity, so if the new rotation term were wrong in a
    // way that survives at identity, everything above would have moved too.
    SceneDocument rotated;
    SceneDocument plain;
    Node n;
    n.id = 1;
    n.shape = Shape::Cube;
    n.position = {0.5f, -1.0f, 2.0f};
    n.scale = {2.0f, 4.0f, 6.0f};
    plain.add(n);
    n.rotation = simd_quaternion(0.0f, simd_float3{0.0f, 1.0f, 0.0f}); // explicit identity
    rotated.add(n);

    const simd_float3 probe = {1.25f, 0.5f, 3.0f};
    const auto a = evaluate_scene_sdf(plain, probe);
    const auto b = evaluate_scene_sdf(rotated, probe);
    REQUIRE(a.has_value());
    REQUIRE(b.has_value());
    CHECK(*a == *b); // bitwise, not Approx: identity must be a true no-op
}

// --- evaluate_scene_sdf: no node cap (wave-B fix 2) ----------------------------

TEST_CASE("evaluate_scene_sdf: a 129-node document evaluates ALL nodes, including "
          "the 129th -- the old 128-node pack_scene cap used to make this node "
          "invisible to the field") {
    SceneDocument doc;
    for (int32_t i = 0; i < 129; ++i) {
        Node n;
        n.id = i + 1;
        n.shape = Shape::Cube;
        n.op = Op::Add;
        // Spaced 10 apart (half-extent 0.5 each): no two nodes' influence
        // overlaps, so a probe at one node's center is unambiguously inside
        // ONLY that node.
        n.position = {static_cast<float>(i) * 10.0f, 0.0f, 0.0f};
        n.scale = {1.0f, 1.0f, 1.0f};
        doc.add(n);
    }
    REQUIRE(doc.nodes().size() == 129u);

    // Node #129 (index 128) is centered at x = 1280 -- under the old 128-node
    // cap, pack_scene drops it entirely, so the folded field at this point
    // would be the empty-fold sentinel (FLT_MAX, positive/outside); with the
    // cap removed, node #129 is present and the probe (its own center) reads
    // negative (inside).
    const simd_float3 p = {1280.0f, 0.0f, 0.0f};
    const auto d = evaluate_scene_sdf(doc, p);
    REQUIRE(d.has_value());
    CHECK(*d < 0.0f);
}

// --- evaluate_scene_sdf / sample_scene: empty scene ----------------------------

TEST_CASE("evaluate_scene_sdf: empty scene returns nullopt") {
    SceneDocument doc;
    CHECK_FALSE(evaluate_scene_sdf(doc, simd_float3{0.0f, 0.0f, 0.0f}).has_value());
}

TEST_CASE("sample_scene: empty scene returns an empty grid (no samples)") {
    SceneDocument doc;
    const SampleGrid grid = sample_scene(doc, 8);
    CHECK(grid.values.empty());
    CHECK(grid.n == 0);
}

// --- sample_scene: domain ------------------------------------------------------

TEST_CASE("sample_scene: pinned two-node scene -> expected AABB-derived cube side, "
          "origin, spacing") {
    // Node 1: position (0,0,0), scale (2,2,2) -> half=(1,1,1) -> AABB [-1,-1,-1]..[1,1,1]
    // Node 2: position (3,1,0), scale (1,4,1) -> half=(0.5,2,0.5)
    //         -> AABB [2.5,-1,-0.5]..[3.5,3,0.5]
    // python3 -c "
    //   import numpy as np
    //   mn = np.minimum([-1,-1,-1], [2.5,-1,-0.5])   # -> [-1, -1, -1]
    //   mx = np.maximum([1,1,1], [3.5,3,0.5])        # -> [3.5, 3, 1]
    //   extent = mx - mn                              # -> [4.5, 4, 2]
    //   side = max(extent.max(), 0.1) * 1.1           # -> 4.95
    //   center = (mn + mx) * 0.5                       # -> [1.25, 1.0, 0.0]
    //   origin = center - side / 2                     # -> [-1.225, -1.475, -2.475]
    //   spacing = side / (4 - 1)                        # -> 1.6500000000000001
    // "
    SceneDocument doc;
    Node n1;
    n1.id = 1;
    n1.shape = Shape::Cube;
    n1.position = {0.0f, 0.0f, 0.0f};
    n1.scale = {2.0f, 2.0f, 2.0f};
    doc.add(n1);

    Node n2;
    n2.id = 2;
    n2.shape = Shape::Sphere;
    n2.position = {3.0f, 1.0f, 0.0f};
    n2.scale = {1.0f, 4.0f, 1.0f};
    doc.add(n2);

    const SampleGrid grid = sample_scene(doc, 4);

    CHECK(grid.n == 4);
    check_float3_approx(grid.origin, simd_float3{-1.225f, -1.475f, -2.475f});
    CHECK(grid.spacing == doctest::Approx(1.65f));
    // side = spacing * (n - 1) = 4.95, recovering the AABB-inflation literal above.
    CHECK(grid.spacing * static_cast<float>(grid.n - 1) == doctest::Approx(4.95f));
    CHECK(grid.values.size() == 4u * 4u * 4u);
}

// --- sample_scene: flat indexing -----------------------------------------------

TEST_CASE("sample_scene: flat index<->(x,y,z) is the documented x-fastest bijection, "
          "and values land where expected") {
    // Round-trip a handful of coordinates through the documented formula
    // (index = x + n*(y + n*z)) purely as a bijection check (no production code
    // involved — this pins the *documented contract* independent of the
    // implementation).
    const int32_t n = 5;
    struct Coord { int32_t x, y, z; };
    const Coord coords[] = {{0, 0, 0}, {4, 0, 0}, {0, 4, 0}, {0, 0, 4}, {2, 3, 1}, {4, 4, 4}};
    for (const Coord& c : coords) {
        const size_t idx = flat_index(c.x, c.y, c.z, n);
        const int32_t x = static_cast<int32_t>(idx % n);
        const int32_t y = static_cast<int32_t>((idx / n) % n);
        const int32_t z = static_cast<int32_t>(idx / (n * n));
        CHECK(x == c.x);
        CHECK(y == c.y);
        CHECK(z == c.z);
    }

    // Single off-center small sphere: position (1,0,0), scale (1,1,1) -> r=0.5.
    // python3 -c "
    //   import numpy as np
    //   pos, scale = np.array([1.,0.,0.]), np.array([1.,1.,1.])
    //   mn, mx = pos - scale*0.5, pos + scale*0.5      # [0.5,-0.5,-0.5]..[1.5,0.5,0.5]
    //   side = max((mx-mn).max(), 0.1) * 1.1            # -> 1.1
    //   center = (mn+mx)*0.5                             # -> [1,0,0]
    //   origin = center - side/2                          # -> [0.45,-0.55,-0.55]
    //   n = 5; spacing = side/(n-1)                        # -> 0.275
    //   # sample (2,2,2) -> origin + (2,2,2)*spacing = (1,0,0) == the sphere center
    //   #   -> q=0 guard: d = -min_component(r) = -0.5
    //   # sample (0,0,0) -> origin = (0.45,-0.55,-0.55)
    //   #   -> sd_ellipsoid(origin-pos, (0.5,0.5,0.5)) = 0.45262794416288266
    // "
    SceneDocument doc;
    Node node;
    node.id = 1;
    node.shape = Shape::Sphere;
    node.position = {1.0f, 0.0f, 0.0f};
    node.scale = {1.0f, 1.0f, 1.0f};
    doc.add(node);

    const SampleGrid grid = sample_scene(doc, n);
    REQUIRE(grid.n == n);
    check_float3_approx(grid.origin, simd_float3{0.45f, -0.55f, -0.55f});
    CHECK(grid.spacing == doctest::Approx(0.275f));

    const size_t near_idx = flat_index(2, 2, 2, n); // = 62, the sample nearest the center
    CHECK(near_idx == 62u);
    CHECK(grid.values[near_idx] == doctest::Approx(-0.5f));

    const size_t far_idx = flat_index(0, 0, 0, n); // = 0, the sample farthest away
    CHECK(far_idx == 0u);
    CHECK(grid.values[far_idx] == doctest::Approx(0.45262794416288266f).epsilon(1e-5));
    CHECK(grid.values[near_idx] < 0.0f);
    CHECK(grid.values[far_idx] > 0.0f);
}

// =============================================================================
// The six shapes added on top of cube and sphere.
//
// Literals below are derived with an independent numpy transcription of the
// same published formulas -- never by running this codebase's C++ -- following
// the convention the sd_box/sd_ellipsoid cases above set. The reusable snippet:
//
//   python3 -c "
//   import numpy as np
//   def sd_capped_cone(p, h, r1, r2):
//       q = np.array([np.hypot(p[0], p[2]), p[1]])
//       k1, k2 = np.array([r2, h]), np.array([r2 - r1, 2.0 * h])
//       ca = np.array([q[0] - min(q[0], r1 if q[1] < 0 else r2), abs(q[1]) - h])
//       cb = q - k1 + k2 * np.clip(np.dot(k1 - q, k2) / np.dot(k2, k2), 0.0, 1.0)
//       s = -1.0 if (cb[0] < 0.0 and ca[1] < 0.0) else 1.0
//       return s * np.sqrt(min(np.dot(ca, ca), np.dot(cb, cb)))
//   def sd_rounded_cylinder(p, r, h, rb):
//       d = np.array([np.hypot(p[0], p[2]) - (r - rb), abs(p[1]) - (h - rb)])
//       return min(max(d[0], d[1]), 0.0) + np.linalg.norm(np.maximum(d, 0.0)) - rb
//   # ... octahedron, regular polygon, vesica and square frustum likewise
//   "
//
// The GEOMETRIC cases (zero set, extent, endpoint equivalences, Lipschitz) are
// worth more than the literals and are derived from the shapes' definitions
// rather than from any formula, so they would survive swapping an evaluator.

namespace {

// One node of the given shape at the origin, evaluated through the full
// pack -> fold path: this exercises the shape id packing, params.x and the
// cross-section contraction, not just the raw primitive.
float eval_shape(Shape shape, float param, simd_float3 scale, simd_float3 p) {
    SceneDocument doc;
    Node node;
    node.id = 1;
    node.shape = shape;
    node.shape_param = param;
    node.scale = scale;
    doc.add(node);
    const std::optional<float> d = evaluate_scene_sdf(doc, p);
    REQUIRE(d.has_value());
    return *d;
}

// Deterministic, self-contained, and not std::mt19937: a fixed sequence keeps a
// failure reproducible, and the distribution does not matter here -- the
// Lipschitz claim is universal, so any spread of points is a valid probe.
struct Lcg {
    uint32_t state;
    float next(float lo, float hi) {
        state = state * 1664525u + 1013904223u;
        return lo + (hi - lo) * (static_cast<float>(state >> 8) / 16777216.0f);
    }
    simd_float3 next3(float lo, float hi) {
        const float x = next(lo, hi), y = next(lo, hi), z = next(lo, hi);
        return simd_float3{x, y, z};
    }
};

} // namespace

TEST_CASE("new shape primitives, pinned against numpy") {
    // Cone: h=2, base radius 1, sharp tip.
    CHECK(sdf_sd_capped_cone(simd_float3{0, 0, 0}, 2.0f, 1.0f, 0.0f)
          == doctest::Approx(-0.485071250f));
    CHECK(sdf_sd_capped_cone(simd_float3{2, 1, 0}, 2.0f, 1.0f, 0.0f)
          == doctest::Approx(1.697749375f));
    // Rounded cylinder: r=1, h=3. rb=0 is a flat cylinder, rb=1 a capsule.
    CHECK(sdf_sd_rounded_cylinder(simd_float3{2, 4, 0}, 1.0f, 3.0f, 0.0f)
          == doctest::Approx(1.414213562f));
    CHECK(sdf_sd_rounded_cylinder(simd_float3{0, 0, 0}, 1.0f, 3.0f, 1.0f)
          == doctest::Approx(-1.0f));
    CHECK(sdf_sd_octahedron(simd_float3{1, 1, 1}, 1.0f) == doctest::Approx(1.154700540f));
    // Prism centre -> minus the apothem, r*cos(pi/n): the distance to the
    // NEAREST edge, which is what pins the circumradius reading of `r`.
    CHECK(sdf_sd_prism(simd_float3{0, 0, 0}, 1.0f, 1.0f, 6)
          == doctest::Approx(-0.866025404f));
    CHECK(sdf_sd_prism(simd_float3{0, 0, 0}, 1.0f, 1.0f, 4)
          == doctest::Approx(-0.707106781f));
    CHECK(sdf_sd_prism(simd_float3{2, 0, 0}, 1.0f, 1.0f, 6)
          == doctest::Approx(1.133974596f));
    CHECK(sdf_sd_vesica(simd_float3{0, 0, 0}, 2.0f, 1.0f) == doctest::Approx(-1.0f));
    CHECK(sdf_sd_vesica(simd_float3{0, 3, 0}, 2.0f, 1.0f) == doctest::Approx(1.0f));
    CHECK(sdf_sd_square_frustum(simd_float3{0, 0, 0}, 1.0f, 0.0f, 1.0f)
          == doctest::Approx(-0.447213595f));
    CHECK(sdf_sd_square_frustum(simd_float3{2, 0, 0}, 1.0f, 0.5f, 1.0f)
          == doctest::Approx(1.212678125f));
}

TEST_CASE("the prism's `r` is its CIRCUMRADIUS, with a vertex facing +z") {
    // The hazard this shape was flagged for: reading iq's parameter as the
    // apothem instead puts the vertices at r/cos(pi/n) -- outside the node's own
    // box, by a factor of two for a triangle. Both readings are pinned, since
    // either one alone can be satisfied by the wrong convention.
    for (const int n : {3, 4, 6, 12}) {
        INFO("n = " << n);
        const float an = static_cast<float>(M_PI) / static_cast<float>(n);
        // A vertex sits on +z at exactly r ...
        CHECK(sdf_sd_prism(simd_float3{0, 0, 1}, 1.0f, 1.0f, n) == doctest::Approx(0.0f));
        // ... and the adjacent edge midpoint at pi/n, at only r*cos(pi/n).
        const float a = std::cos(an);
        CHECK(sdf_sd_prism(simd_float3{a * std::sin(an), 0.0f, a * std::cos(an)}, 1.0f, 1.0f, n)
              == doctest::Approx(0.0f));
        // Discriminating: the apothem reading would put a vertex out here.
        CHECK(sdf_sd_prism(simd_float3{0, 0, 1.0f / a}, 1.0f, 1.0f, n) > 0.01f);
    }
}

TEST_CASE("every new shape's zero set lands where its box says it should") {
    // Points derived from each shape's DEFINITION against half-extents
    // (r, hy, r) = (1, 2, 1), not from any formula -- so these survive swapping
    // an evaluator for a different one that claims the same shape.
    const simd_float3 scale = {2.0f, 4.0f, 2.0f}; // -> h = (1, 2, 1), r = 1, hy = 2

    struct Case { const char* label; Shape shape; float param; simd_float3 surface; };
    const Case cases[] = {
        {"cone apex",             Shape::Cone,       0.0f, {0, 2, 0}},
        {"cone base rim",         Shape::Cone,       0.0f, {1, -2, 0}},
        {"cone base centre",      Shape::Cone,       0.0f, {0, -2, 0}},
        {"truncated cone top",    Shape::Cone,       0.5f, {0.5f, 2, 0}},
        {"cylinder side",         Shape::Capsule,    0.0f, {1, 0, 0}},
        {"cylinder rim corner",   Shape::Capsule,    0.0f, {1, 2, 0}},
        {"capsule side",          Shape::Capsule,    1.0f, {1, 0, 0}},
        {"capsule pole",          Shape::Capsule,    1.0f, {0, 2, 0}},
        {"octahedron +x vertex",  Shape::Octahedron, 0.0f, {1, 0, 0}},
        {"octahedron +y vertex",  Shape::Octahedron, 0.0f, {0, 2, 0}},
        {"pyramid apex",          Shape::Pyramid,    0.0f, {0, 2, 0}},
        {"pyramid base corner",   Shape::Pyramid,    0.0f, {1, -2, 1}},
        {"frustum top corner",    Shape::Pyramid,    0.5f, {0.5f, 2, 0.5f}},
        {"prism vertex, +z",      Shape::Prism,      6.0f, {0, 0, 1}},
        {"prism top edge",        Shape::Prism,      6.0f, {0, 2, 1}},
        {"vesica pole",           Shape::Vesica,     0.0f, {0, 2, 0}},
        {"vesica equator",        Shape::Vesica,     0.0f, {1, 0, 0}},
    };

    for (const Case& c : cases) {
        INFO("case: " << c.label);
        CHECK(eval_shape(c.shape, c.param, scale, c.surface) == doctest::Approx(0.0f).epsilon(1e-4));
        // Discriminating: nudging outward along the same ray must read positive,
        // so a degenerate evaluator returning ~0 everywhere cannot pass.
        CHECK(eval_shape(c.shape, c.param, scale, c.surface * 1.2f) > 0.01f);
    }
}

TEST_CASE("no shape escapes its own bounding box, and each one reaches it") {
    // What sdf.cpp's scene_aabb and navigation.cpp's node_bounding_radius both
    // assume: every shape is inscribed in position +- scale*0.5. Anisotropic on
    // purpose -- this is also the cross-section contraction's containment claim.
    //
    // BOTH ASPECTS, and that is the point. An earlier version of this case ran
    // only the tall box, and a vesica wider than it is tall escaped in y for as
    // long as that was true: past w = h its profile arc's centre crosses the
    // axis, which lifts the arc's topmost point into the revolved region and
    // pushes the solid out to (w^2 + h^2) / 2w. One shape, one aspect ratio, no
    // test.
    const simd_float3 aspects[] = {
        {3.0f, 5.0f, 2.0f},  // taller than wide
        {5.0f, 1.5f, 4.0f},  // wider than tall -- the case that used to escape
        {2.0f, 2.0f, 2.0f},  // cube-shaped, where every contraction is identity
    };
    for (const simd_float3& scale : aspects) {
    const simd_float3 h = scale * 0.5f;
    INFO("scale: (" << scale.x << ", " << scale.y << ", " << scale.z << ")");

    // widest_y: the height at which the cross-section reaches its full radius,
    // as a fraction of h.y. Zero for the shapes with a waist at mid-height, but
    // -1 (the base) for the two tapered ones -- a cone at mid-height is at HALF
    // its base radius, so probing there would assert something false about a
    // correct shape.
    struct Case { const char* label; Shape shape; float param; float widest_y; };
    const Case cases[] = {
        {"cone", Shape::Cone, 0.35f, -1.0f},      {"capsule", Shape::Capsule, 0.6f, 0.0f},
        {"octahedron", Shape::Octahedron, 0.0f, 0.0f}, {"pyramid", Shape::Pyramid, 0.25f, -1.0f},
        {"prism", Shape::Prism, 5.0f, 0.0f},      {"vesica", Shape::Vesica, 0.0f, 0.0f},
    };

    Lcg rng{20260806u};
    for (const Case& c : cases) {
        INFO("case: " << std::string(c.label));
        // Nothing outside the box may be inside the shape.
        for (int i = 0; i < 400; ++i) {
            const simd_float3 dir = simd_normalize(rng.next3(-1.0f, 1.0f) + simd_float3{1e-3f, 0, 0});
            const simd_float3 outside = dir * (simd_length(h) * 1.05f + 0.01f);
            CHECK(eval_shape(c.shape, c.param, scale, outside) > 0.0f);
        }
        // And the shape genuinely fills the box's height and cross-section
        // rather than shrinking inside it -- the failure mode a contraction
        // written with the wrong ratio would produce.
        const float r = std::fmin(h.x, h.z);
        CHECK(eval_shape(c.shape, c.param, scale, simd_float3{0, h.y, 0}) <= 1e-4f);
        CHECK(eval_shape(c.shape, c.param, scale, simd_float3{0, -h.y, 0}) <= 1e-4f);
        CHECK(eval_shape(c.shape, c.param, scale, simd_float3{0, c.widest_y * h.y, r}) <= 1e-4f);
    }
    }
}

TEST_CASE("every shape's field is 1-Lipschitz, which is what the sphere trace rests on") {
    // THE contract of sdf_eval_node's cross-section scheme. A field that grows
    // faster than distance overestimates, and an overestimating field makes the
    // sphere trace step THROUGH surfaces -- so this is the assertion that fails
    // if anyone rewrites the contraction as a division by half-extents (which
    // dilates the two smaller axes instead of shrinking the larger ones).
    //
    // Deliberately includes strongly anisotropic half-extents, since that is the
    // only regime where the contraction is not the identity.
    struct Case { const char* label; Shape shape; float param; };
    const Case cases[] = {
        {"cone, sharp", Shape::Cone, 0.0f},        {"cone, truncated", Shape::Cone, 0.4f},
        {"cone, cylinder", Shape::Cone, 1.0f},     {"capsule, flat", Shape::Capsule, 0.0f},
        {"capsule, round", Shape::Capsule, 1.0f},  {"octahedron", Shape::Octahedron, 0.0f},
        {"pyramid, sharp", Shape::Pyramid, 0.0f},  {"pyramid, frustum", Shape::Pyramid, 0.6f},
        {"prism, 3", Shape::Prism, 3.0f},          {"prism, 12", Shape::Prism, 12.0f},
        {"vesica, spindle", Shape::Vesica, 0.0f},
    };

    Lcg rng{7u};
    for (const Case& c : cases) {
        INFO("case: " << c.label);
        float worst = 0.0f;
        for (int i = 0; i < 600; ++i) {
            const simd_float3 scale = rng.next3(0.3f, 6.0f);
            const simd_float3 a = rng.next3(-5.0f, 5.0f);
            const simd_float3 b = rng.next3(-5.0f, 5.0f);
            const float dist = simd_length(a - b);
            if (dist < 1e-3f) {
                continue;
            }
            const float da = eval_shape(c.shape, c.param, scale, a);
            const float db = eval_shape(c.shape, c.param, scale, b);
            worst = std::fmax(worst, std::fabs(da - db) / dist);
        }
        INFO("worst ratio: " << worst);
        CHECK(worst <= 1.0f + 1e-4f);
    }
}

TEST_CASE("a shape at a parameter endpoint becomes a shape we already trust") {
    // Cross-checks that cost nothing and catch a parameter wired to the wrong
    // argument -- each one is a different evaluator arriving at the same answer.
    SUBCASE("vesica with width == half-length is exactly a sphere") {
        for (const simd_float3 p : {simd_float3{0.4f, 0.3f, -0.2f}, simd_float3{2.0f, 0, 0},
                                    simd_float3{0, -1.7f, 0.9f}}) {
            CHECK(sdf_sd_vesica(p, 1.0f, 1.0f)
                  == doctest::Approx(simd_length(p) - 1.0f).epsilon(1e-5));
        }
    }
    SUBCASE("capsule with full roundness on a cube-shaped box is exactly a sphere") {
        const simd_float3 scale = {2.0f, 2.0f, 2.0f}; // h = (1,1,1) -> r = hy = 1
        for (const simd_float3 p : {simd_float3{0.4f, 0.3f, -0.2f}, simd_float3{2.0f, 0, 0},
                                    simd_float3{0, -1.7f, 0.9f}}) {
            CHECK(eval_shape(Shape::Capsule, 1.0f, scale, p)
                  == doctest::Approx(simd_length(p) - 1.0f).epsilon(1e-5));
        }
    }
    SUBCASE("cone with tip ratio 1 is exactly a flat-capped cylinder") {
        for (const simd_float3 p : {simd_float3{0.5f, 1.0f, 0}, simd_float3{2.0f, 0, 0},
                                    simd_float3{0, 4.0f, 0}, simd_float3{1.4f, 3.5f, 0}}) {
            CHECK(sdf_sd_capped_cone(p, 3.0f, 1.0f, 1.0f)
                  == doctest::Approx(sdf_sd_rounded_cylinder(p, 1.0f, 3.0f, 0.0f)).epsilon(1e-5));
        }
    }
    SUBCASE("pyramid with tip ratio 1 has a box's zero set (its field is weaker)") {
        // Only the SIGN is compared: the frustum is a max-of-planes, so away
        // from the surface it underestimates where sd_box is exact. The zero set
        // is what has to agree, and it does.
        const simd_float3 scale = {2.0f, 4.0f, 2.0f};
        Lcg rng{99u};
        for (int i = 0; i < 300; ++i) {
            const simd_float3 p = rng.next3(-3.0f, 3.0f);
            const float pyramid = eval_shape(Shape::Pyramid, 1.0f, scale, p);
            const float box = eval_shape(Shape::Cube, 0.0f, scale, p);
            CHECK((pyramid < 0.0f) == (box < 0.0f));
            // ... and never overestimates the exact answer.
            CHECK(std::fabs(pyramid) <= std::fabs(box) + 1e-4f);
        }
    }
}

TEST_CASE("the cross-section contraction is the identity when hx == hz") {
    // The claim that makes the scheme worth having: an axially stretched capsule
    // or cone -- the common case in this domain -- is measured EXACTLY, not
    // conservatively. h = (1, 3, 1), so r = 1 and the contraction's two factors
    // are both exactly 1.
    const simd_float3 scale = {2.0f, 6.0f, 2.0f};
    Lcg rng{31u};
    for (int i = 0; i < 200; ++i) {
        const simd_float3 p = rng.next3(-5.0f, 5.0f);
        CHECK(eval_shape(Shape::Cone, 0.0f, scale, p)
              == doctest::Approx(sdf_sd_capped_cone(p, 3.0f, 1.0f, 0.0f)).epsilon(1e-5));
        CHECK(eval_shape(Shape::Capsule, 1.0f, scale, p)
              == doctest::Approx(sdf_sd_rounded_cylinder(p, 1.0f, 3.0f, 1.0f)).epsilon(1e-5));
        CHECK(eval_shape(Shape::Vesica, 0.0f, scale, p)
              == doctest::Approx(sdf_sd_vesica(p, 3.0f, 1.0f)).epsilon(1e-5));
    }
}

TEST_CASE("roundness rounds a shape without letting it leave its box") {
    // The dial four shapes share: shrink by rb, then subtract rb. It is exact
    // and bbox-tight for any convex shape, and these are the cases that would
    // fail if the shrink and the offset ever stopped matching.
    struct Case { const char* label; Shape shape; };
    const Case cases[] = {
        {"cube", Shape::Cube}, {"capsule", Shape::Capsule},
        {"octahedron", Shape::Octahedron}, {"vesica", Shape::Vesica},
    };
    const simd_float3 scale = {2.0f, 5.0f, 2.0f}; // h = (1, 2.5, 1)
    const simd_float3 h = scale * 0.5f;

    Lcg rng{4242u};
    for (const Case& c : cases) {
        INFO("case: " << std::string(c.label));
        for (const float roundness : {0.0f, 0.25f, 0.5f, 0.75f, 1.0f}) {
            INFO("roundness: " << roundness);
            // Still inside the box at every setting...
            for (int i = 0; i < 200; ++i) {
                const simd_float3 dir =
                    simd_normalize(rng.next3(-1.0f, 1.0f) + simd_float3{1e-3f, 0, 0});
                CHECK(eval_shape(c.shape, roundness, scale, dir * (simd_length(h) * 1.05f)) > 0.0f);
            }
            // ... and still reaching the box's two ends, which is what makes
            // this rounding rather than shrinking.
            CHECK(eval_shape(c.shape, roundness, scale, simd_float3{0, h.y, 0})
                  == doctest::Approx(0.0f).epsilon(1e-3));
            CHECK(eval_shape(c.shape, roundness, scale, simd_float3{h.x, 0, 0})
                  == doctest::Approx(0.0f).epsilon(1e-3));
        }
    }

    SUBCASE("a fully rounded cube on a cube-shaped box is exactly a sphere") {
        const simd_float3 cubic = {2.0f, 2.0f, 2.0f};
        for (const simd_float3 p : {simd_float3{0.4f, 0.3f, -0.2f}, simd_float3{2.0f, 0, 0},
                                    simd_float3{0, -1.7f, 0.9f}}) {
            CHECK(eval_shape(Shape::Cube, 1.0f, cubic, p)
                  == doctest::Approx(simd_length(p) - 1.0f).epsilon(1e-5));
        }
    }

    SUBCASE("a fully rounded octahedron on a cube-shaped box is exactly a sphere") {
        const simd_float3 cubic = {2.0f, 2.0f, 2.0f};
        for (const simd_float3 p : {simd_float3{0.4f, 0.3f, -0.2f}, simd_float3{2.0f, 0, 0},
                                    simd_float3{0, -1.7f, 0.9f}}) {
            CHECK(eval_shape(Shape::Octahedron, 1.0f, cubic, p)
                  == doctest::Approx(simd_length(p) - 1.0f).epsilon(1e-5));
        }
    }

    SUBCASE("roundness 0 leaves the sharp shape untouched") {
        // The regression guard for every pinned expectation elsewhere in this
        // file: the default is 0, so if rounding leaked in at zero, the whole
        // suite would have moved.
        const simd_float3 s = {2.0f, 3.0f, 4.0f};
        Lcg probe{8u};
        for (int i = 0; i < 100; ++i) {
            const simd_float3 p = probe.next3(-4.0f, 4.0f);
            CHECK(eval_shape(Shape::Cube, 0.0f, s, p)
                  == doctest::Approx(sd_box(p, s * 0.5f)).epsilon(1e-5));
        }
    }
}

TEST_CASE("every shape mirrors under a negative scale rather than vanishing") {
    // A negative scale component should MIRROR a shape, not delete it. Cube and
    // Sphere are the two branches that bypass sdf_safe_half_extents, so they
    // were the two that took the half-extents unsigned -- and a negative extent
    // makes sd_box's `abs(q) - half` positive everywhere, i.e. empty space.
    //
    // The wireframe builder already assumes otherwise: append_node_wireframe
    // builds its matrix from simd_abs(node.scale) and cites "the evaluator
    // measures against abs(half_extents)" as the reason. That was true of six
    // shapes out of eight. A mirrored node drew a full wireframe around
    // nothing rendered and nothing pickable.
    for (const Shape shape : {Shape::Cube, Shape::Sphere, Shape::Prism,
                              Shape::Octahedron, Shape::Vesica, Shape::Cone,
                              Shape::Capsule, Shape::Pyramid}) {
        CAPTURE(static_cast<int>(shape));
        SceneDocument doc;
        Node node;
        node.id = 1;
        node.shape = shape;
        node.scale = {-1.0f, 1.0f, 1.0f};
        doc.add(node);

        // The centre of a solid is inside it, whichever way its axes point.
        const auto d = evaluate_scene_sdf(doc, simd_float3{0.0f, 0.0f, 0.0f});
        REQUIRE(d.has_value());
        CHECK(*d < 0.0f);
    }
}

TEST_CASE("no shape evaluates to NaN at a degenerate scale") {
    // The sibling of the mirror sweep above, for the OTHER degenerate scale a
    // SceneDocument::add can carry that the UI cannot produce.
    //
    // ASSERTED ON sdf_eval_node, NOT through evaluate_scene_sdf, and that is
    // the whole reason this test can see anything. sdf_fold reduces with
    // std::fmin, which RETURNS THE OTHER OPERAND when one is NaN -- so a NaN
    // node folds to FLT_MAX and the scene query comes back finite, tidy and
    // wrong, with the shape simply absent. picking.cpp calls sdf_eval_node
    // directly and gets the NaN unlaundered, which is where it does damage:
    // every comparison against it is false, so the trace neither advances nor
    // reports a hit and the node is unpickable.
    for (const Shape shape : {Shape::Cube, Shape::Sphere, Shape::Prism,
                              Shape::Octahedron, Shape::Vesica, Shape::Cone,
                              Shape::Capsule, Shape::Pyramid}) {
        CAPTURE(static_cast<int>(shape));
        Node node;
        node.id = 1;
        node.shape = shape;
        node.scale = {1.0f, 0.0f, 1.0f}; // flat: zero half-extent on y
        const SdfNode sn = local_sdf_node(node);

        // Both sides of the collapsed axis, and the axis itself: a division by
        // a zero extent gives inf off it and 0/0 exactly on it.
        for (const simd_float3 p : {simd_float3{0.0f, 0.0f, 0.0f},
                                    simd_float3{0.3f, 0.0f, 0.2f},
                                    simd_float3{0.0f, 0.7f, 0.0f},
                                    simd_float3{1.5f, -2.0f, 0.4f}}) {
            CAPTURE(p.x); CAPTURE(p.y); CAPTURE(p.z);
            CHECK(std::isfinite(sdf_eval_node(sn, p)));
        }
    }
}
