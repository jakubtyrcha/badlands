#include <doctest.h>

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
