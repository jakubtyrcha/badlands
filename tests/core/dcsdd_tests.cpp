#include <doctest.h>

#include <vector>

#include <shapeshifter/ShapeshifterCore.h>

#include "dcsdd.h"
#include "sdf.h"

using namespace sq;

namespace {

// Parameters are `const`: doctest's CHECK() binds the compared sub-expression
// to a reference, and Clang only allows that for *const* accesses of
// ext_vector_type components — matches sdf_tests.cpp's check_float3_approx.
void check_float3_approx(const simd_float3 actual, const simd_float3 expected) {
    CHECK(actual.x == doctest::Approx(expected.x));
    CHECK(actual.y == doctest::Approx(expected.y));
    CHECK(actual.z == doctest::Approx(expected.z));
}

// Flat-index convention documented on SampleGrid::values / dcsdd.h's edge
// convention: index = x + n*(y + n*z).
size_t flat_index(int32_t x, int32_t y, int32_t z, int32_t n) {
    const size_t un = static_cast<size_t>(n);
    return static_cast<size_t>(x) + un * (static_cast<size_t>(y) + un * static_cast<size_t>(z));
}

// Decodes a flat sample index back to (x,y,z), the inverse of flat_index
// above — used to re-derive edge coordinates from DcsddInit::edge_base for
// CSR-integrity checks (test-only; not production code under test).
struct Coord3 { int32_t x, y, z; };
Coord3 unflatten(int32_t flat, int32_t n) {
    return Coord3{flat % n, (flat / n) % n, flat / (n * n)};
}

// --- Shared grid builders ------------------------------------------------
//
// Every grid below is built directly here (never via sample_scene), with
// values computed by the same formula independently evaluated in numpy (see
// the derivation comments on each TEST_CASE). Reference numpy implementation
// (written from the brief's prose, not from this codebase) lived at
// /private/tmp/.../scratchpad/dcsdd_ref.py during development.

// n=4 grid, spacing 1, origin (-1.5,-1.5,-1.5) (samples at world coords
// -1.5/-0.5/0.5/1.5 per axis), values = |p| - 1.3 (sphere centered at the
// world origin, radius 1.3). Rich structure: 24 interesting edges, 26
// interesting cells (every cell except the fully-enclosed center cell
// (1,1,1), whose 8 corners are all inside).
SampleGrid centered_sphere_grid() {
    SampleGrid grid;
    grid.n = 4;
    grid.spacing = 1.0f;
    grid.origin = simd_float3{-1.5f, -1.5f, -1.5f};
    grid.values.resize(4u * 4u * 4u);
    const float r = 1.3f;
    for (int32_t z = 0; z < grid.n; ++z) {
        for (int32_t y = 0; y < grid.n; ++y) {
            for (int32_t x = 0; x < grid.n; ++x) {
                const simd_float3 p =
                    grid.origin + grid.spacing * simd_float3{float(x), float(y), float(z)};
                grid.values[flat_index(x, y, z, grid.n)] = simd_length(p) - r;
            }
        }
    }
    return grid;
}

// n=3 grid, spacing 1, origin (-1.5,-1.5,-1.5), sphere centered exactly at
// sample (0,0,0)'s world position with radius 0.7 — small enough that only
// that one corner sample is inside. Produces exactly 3 interesting edges,
// all based at (0,0,0) (one per axis), each with only 1 containing cell
// (the grid corner: both perpendicular coordinates clamp away 3 of the
// usual 4 candidates) — the boundary-edge case for item 4 — and exactly one
// interesting cell (id 0) with those same 3 edges — the ">=3 edges" case for
// item 5's centroid (Eq. 3).
SampleGrid corner_sphere_grid() {
    SampleGrid grid;
    grid.n = 3;
    grid.spacing = 1.0f;
    grid.origin = simd_float3{-1.5f, -1.5f, -1.5f};
    grid.values.resize(3u * 3u * 3u);
    const simd_float3 center = grid.origin; // == world position of sample (0,0,0)
    const float r = 0.7f;
    for (int32_t z = 0; z < grid.n; ++z) {
        for (int32_t y = 0; y < grid.n; ++y) {
            for (int32_t x = 0; x < grid.n; ++x) {
                const simd_float3 p =
                    grid.origin + grid.spacing * simd_float3{float(x), float(y), float(z)};
                grid.values[flat_index(x, y, z, grid.n)] = simd_length(p - center) - r;
            }
        }
    }
    return grid;
}

} // namespace

// All pinned literals below are derived independently with a from-scratch
// numpy reference implementation of the brief's grid conventions and Eqs.
// 1-3 (never by running this codebase's C++). Per-case invocations/values
// are given in comments above each assertion.

// --- Item 1: tiny grid, exact interesting-edge/cell set -----------------------

TEST_CASE("dcsdd_init: tiny grid — exact interesting-edge set (all 3 axes), in the "
          "documented order; a cell with no sign change produces no entry") {
    // n=3, spacing 1, origin (0,0,0). s(x,y,z) = -1 if x==0 else +1, except
    // s(0,0,1) is overridden to +1. This is inside (s<0) only on the x=0
    // face, minus the one overridden sample — a hand-checked (and
    // numpy-cross-checked) sign pattern, not an SDF.
    //
    // python3 (dcsdd_ref.dcsdd_init on this exact value array) enumerates:
    //   axis0 (x=0 base, interesting for all (y,z) except (y=0,z=1)):
    //     (0,0,0,0) (0,0,1,0) (0,0,2,0) (0,0,1,1) (0,0,2,1)
    //     (0,0,0,2) (0,0,1,2) (0,0,2,2)               [8 edges]
    //   axis1: (1,0,0,1)                                [1 edge]
    //   axis2: (2,0,0,0) (2,0,0,1)                       [2 edges]
    //   -> 11 edges total, axis-major/z,y,x-ascending order confirmed by the
    //   reference script.
    //   Interesting cells (z,y,x ascending): id 0 (0,0,0), id 2 (0,1,0),
    //   id 4 (0,0,1), id 6 (0,1,1). Cell id 1 = (1,0,0) — entirely on the
    //   x in {1,2} slab, all 8 corners == +1 — has no entry.
    SampleGrid grid;
    grid.n = 3;
    grid.spacing = 1.0f;
    grid.origin = simd_float3{0.0f, 0.0f, 0.0f};
    grid.values.resize(3u * 3u * 3u);
    for (int32_t z = 0; z < 3; ++z) {
        for (int32_t y = 0; y < 3; ++y) {
            for (int32_t x = 0; x < 3; ++x) {
                grid.values[flat_index(x, y, z, 3)] = (x == 0) ? -1.0f : 1.0f;
            }
        }
    }
    grid.values[flat_index(0, 0, 1, 3)] = 1.0f;

    const DcsddInit init = dcsdd_init(grid);

    struct ExpectedEdge { int32_t axis, x, y, z; };
    const ExpectedEdge expected[] = {
        {0, 0, 0, 0}, {0, 0, 1, 0}, {0, 0, 2, 0}, {0, 0, 1, 1}, {0, 0, 2, 1},
        {0, 0, 0, 2}, {0, 0, 1, 2}, {0, 0, 2, 2},
        {1, 0, 0, 1},
        {2, 0, 0, 0}, {2, 0, 0, 1},
    };
    REQUIRE(init.edge_axis.size() == 11u);
    REQUIRE(init.edge_base.size() == 11u);
    for (size_t i = 0; i < 11; ++i) {
        CHECK(init.edge_axis[i] == expected[i].axis);
        CHECK(init.edge_base[i] ==
              static_cast<int32_t>(flat_index(expected[i].x, expected[i].y, expected[i].z, 3)));
    }

    REQUIRE(init.cell_id.size() == 4u);
    CHECK(init.cell_id[0] == 0);
    CHECK(init.cell_id[1] == 2);
    CHECK(init.cell_id[2] == 4);
    CHECK(init.cell_id[3] == 6);

    // Cell (1,0,0) -> id 1 + 2*(0+2*0) = 1: not present.
    for (int32_t id : init.cell_id) {
        CHECK(id != 1);
    }
}

// --- Item 2: Eq. 1 crossings (hermite_crossing) --------------------------------

TEST_CASE("hermite_crossing: Eq. 1 — asymmetric magnitudes, the s==0 endpoint case, "
          "and the both-zero guard (t=0.5)") {
    SUBCASE("asymmetric |s_a|=1, |s_b|=3 -> t=0.25, interpolated point") {
        // python3: hermite_crossing(-1.0, 3.0, (1,2,3), (5,-2,7)) -> (array([2.,1.,4.]), 0.25)
        const HermitePoint hp =
            hermite_crossing(-1.0f, 3.0f, simd_float3{1.0f, 2.0f, 3.0f}, simd_float3{5.0f, -2.0f, 7.0f});
        CHECK(hp.t == doctest::Approx(0.25f));
        check_float3_approx(hp.p, simd_float3{2.0f, 1.0f, 4.0f});
    }

    SUBCASE("s_a == 0 endpoint -> t=0, point lands exactly on u_a") {
        // python3: hermite_crossing(0.0, -2.0, (1,1,1), (9,9,9)) -> (array([1.,1.,1.]), 0.0)
        const HermitePoint hp =
            hermite_crossing(0.0f, -2.0f, simd_float3{1.0f, 1.0f, 1.0f}, simd_float3{9.0f, 9.0f, 9.0f});
        CHECK(hp.t == doctest::Approx(0.0f));
        check_float3_approx(hp.p, simd_float3{1.0f, 1.0f, 1.0f});
    }

    SUBCASE("both-zero guard: |s_a|+|s_b|==0 -> t=0.5, edge midpoint") {
        // python3: hermite_crossing(0.0, 0.0, (0,0,0), (10,0,0)) -> (array([5.,0.,0.]), 0.5)
        // Note: a real edge can never have both samples exactly 0 and still
        // be "interesting" (0 counts as outside on both ends -> same sign,
        // never scanned by dcsdd_init) — this guard is only reachable via
        // hermite_crossing called directly, which is exactly why it's
        // exposed standalone.
        const HermitePoint hp =
            hermite_crossing(0.0f, 0.0f, simd_float3{0.0f, 0.0f, 0.0f}, simd_float3{10.0f, 0.0f, 0.0f});
        CHECK(hp.t == doctest::Approx(0.5f));
        check_float3_approx(hp.p, simd_float3{5.0f, 0.0f, 0.0f});
    }
}

// --- Item 3: Eq. 2 normals ------------------------------------------------------

TEST_CASE("dcsdd_init: Eq. 2 — linear field s=dot(g,p)+c makes the trilinear gradient "
          "exactly g everywhere, so every hermite_n equals normalize(g)") {
    // n=3, spacing 1, origin (0,0,0), g=(2,-1,3), c=-3. Trilinear
    // interpolation reproduces affine functions exactly (no mixed-partial
    // error term), so the analytic gradient of every cell's interpolant is
    // g at every point, and the Eq. 2 sum-then-normalize collapses to
    // normalize(g) regardless of how many cells contain the edge.
    // python3 (dcsdd_ref.dcsdd_init on dot(g,p)+c over this grid) -> 13
    // interesting edges, all with n == normalize(g); normalize((2,-1,3)) =
    // (0.5345224838248488, -0.2672612419124244, 0.8017837257372732)
    // (|g| = sqrt(14) = 3.7416573867739413).
    const simd_float3 g = {2.0f, -1.0f, 3.0f};
    const float c = -3.0f;

    SampleGrid grid;
    grid.n = 3;
    grid.spacing = 1.0f;
    grid.origin = simd_float3{0.0f, 0.0f, 0.0f};
    grid.values.resize(3u * 3u * 3u);
    for (int32_t z = 0; z < 3; ++z) {
        for (int32_t y = 0; y < 3; ++y) {
            for (int32_t x = 0; x < 3; ++x) {
                const simd_float3 p =
                    grid.origin + grid.spacing * simd_float3{float(x), float(y), float(z)};
                grid.values[flat_index(x, y, z, 3)] = simd_dot(g, p) + c;
            }
        }
    }

    const DcsddInit init = dcsdd_init(grid);
    REQUIRE(init.hermite_n.size() == 13u); // non-vacuous: the field does cross zero

    const simd_float3 expected_n = {0.5345224838248488f, -0.2672612419124244f, 0.8017837257372732f};
    for (const simd_float3& n : init.hermite_n) {
        check_float3_approx(n, expected_n);
    }
}

TEST_CASE("dcsdd_init: Eq. 2 — sphere field, pinned normals at 3 interior edges (one "
          "per axis, each with all 4 containing cells) against numpy-evaluated "
          "trilinear gradients, distinct from the analytic sphere normal") {
    // centered_sphere_grid(): |p| - 1.3 on n=4/spacing=1/origin=(-1.5,-1.5,-1.5).
    // python3 dcsdd_ref.dcsdd_init picks out (edge list index, axis, base):
    //   idx 0,  axis 0, base (0,1,1): t=0.45225076149186344
    //     p=(-1.0477492385081366, -0.5, -0.5)
    //     n=(-0.8670739231966981, -0.3522661009182452, -0.3522661009182452)
    //   idx 8,  axis 1, base (1,0,1): t=0.45225076149186344
    //     p=(-0.5, -1.0477492385081366, -0.5)
    //     n=(-0.3522661009182452, -0.8670739231966981, -0.3522661009182452)
    //   idx 16, axis 2, base (1,1,0): t=0.45225076149186344
    //     p=(-0.5, -0.5, -1.0477492385081366)
    //     n=(-0.3522661009182452, -0.3522661009182452, -0.867073923196698)
    // All three have 4 containing cells (a genuine multi-cell Eq. 2 sum, not
    // the degenerate 1-cell case) — and all three normals are visibly NOT
    // the analytic sphere normal at their hermite point (e.g. edge idx 0's
    // point is ~(-1.05,-0.5,-0.5), whose analytic sphere normal would be
    // normalize(-1.05,-0.5,-0.5) =~ (-0.88,-0.42,-0.42), not
    // (-0.867,-0.352,-0.352) — the pinned value is what the trilinear
    // interpolant's gradient gives, per the brief.
    const SampleGrid grid = centered_sphere_grid();
    const DcsddInit init = dcsdd_init(grid);
    REQUIRE(init.edge_axis.size() == 24u);

    struct Pinned { size_t idx; int32_t axis; simd_float3 p; simd_float3 n; };
    const Pinned pinned[] = {
        {0, 0,
         {-1.0477492385081366f, -0.5f, -0.5f},
         {-0.8670739231966981f, -0.3522661009182452f, -0.3522661009182452f}},
        {8, 1,
         {-0.5f, -1.0477492385081366f, -0.5f},
         {-0.3522661009182452f, -0.8670739231966981f, -0.3522661009182452f}},
        {16, 2,
         {-0.5f, -0.5f, -1.0477492385081366f},
         {-0.3522661009182452f, -0.3522661009182452f, -0.867073923196698f}},
    };
    for (const Pinned& e : pinned) {
        CAPTURE(e.idx);
        REQUIRE(e.idx < init.edge_axis.size());
        CHECK(init.edge_axis[e.idx] == e.axis);
        check_float3_approx(init.hermite_p[e.idx], e.p);
        check_float3_approx(init.hermite_n[e.idx], e.n);
    }
}

// --- Item 4: boundary edge still gets a normal ---------------------------------

TEST_CASE("dcsdd_init: boundary edge (only 1 containing cell) still gets a pinned normal") {
    // corner_sphere_grid(): the 3 interesting edges are all based at sample
    // (0,0,0), the grid's own corner, so each has only 1 containing cell
    // (both perpendicular coordinates clamp away 3 of the usual 4
    // candidates). python3 dcsdd_ref.dcsdd_init:
    //   axis 0: t=0.7, p=(-0.8,-1.5,-1.5),
    //     n=(0.7678506031835753, 0.4529930745555717, 0.4529930745555717)
    //   axis 1: t=0.7, p=(-1.5,-0.8,-1.5),
    //     n=(0.4529930745555717, 0.7678506031835753, 0.4529930745555717)
    //   axis 2: t=0.7, p=(-1.5,-1.5,-0.8),
    //     n=(0.4529930745555717, 0.4529930745555717, 0.7678506031835753)
    const SampleGrid grid = corner_sphere_grid();
    const DcsddInit init = dcsdd_init(grid);

    REQUIRE(init.edge_axis.size() == 3u);
    const simd_float3 expected_n[3] = {
        {0.7678506031835753f, 0.4529930745555717f, 0.4529930745555717f},
        {0.4529930745555717f, 0.7678506031835753f, 0.4529930745555717f},
        {0.4529930745555717f, 0.4529930745555717f, 0.7678506031835753f},
    };
    const simd_float3 expected_p[3] = {
        {-0.8f, -1.5f, -1.5f},
        {-1.5f, -0.8f, -1.5f},
        {-1.5f, -1.5f, -0.8f},
    };
    for (size_t i = 0; i < 3; ++i) {
        CAPTURE(i);
        CHECK(init.edge_axis[i] == static_cast<int32_t>(i));
        CHECK(init.edge_base[i] == 0); // all based at sample (0,0,0)
        CHECK(init.hermite_p[i].x == doctest::Approx(expected_p[i].x));
        CHECK(init.hermite_p[i].y == doctest::Approx(expected_p[i].y));
        CHECK(init.hermite_p[i].z == doctest::Approx(expected_p[i].z));
        check_float3_approx(init.hermite_n[i], expected_n[i]);
    }
}

// --- Item 5: Eq. 3 centroid ------------------------------------------------------

TEST_CASE("dcsdd_init: Eq. 3 — centroid vertex for a cell with 3 interesting edges") {
    // corner_sphere_grid(): the sole interesting cell (id 0, ijk (0,0,0))
    // has exactly the 3 edges pinned in the boundary-edge test above.
    // python3: mean of (-0.8,-1.5,-1.5), (-1.5,-0.8,-1.5), (-1.5,-1.5,-0.8)
    //   -> (-1.2666666666666666, -1.2666666666666666, -1.2666666666666666)
    const SampleGrid grid = corner_sphere_grid();
    const DcsddInit init = dcsdd_init(grid);

    REQUIRE(init.cell_id.size() == 1u);
    CHECK(init.cell_id[0] == 0);
    check_float3_approx(init.cell_vertex[0],
                         simd_float3{-1.2666666666666666f, -1.2666666666666666f, -1.2666666666666666f});
}

// --- Item 6: CSR integrity -------------------------------------------------------

TEST_CASE("dcsdd_init: CSR integrity — monotone offsets, sizes match, every "
          "referenced edge lies in its referencing cell, deterministic across runs") {
    const SampleGrid grid = centered_sphere_grid();
    const DcsddInit a = dcsdd_init(grid);
    const DcsddInit b = dcsdd_init(grid);

    REQUIRE(a.cell_edge_offsets.size() == a.cell_id.size() + 1);
    CHECK(a.cell_edge_offsets.front() == 0);
    CHECK(a.cell_edge_offsets.back() == static_cast<int32_t>(a.cell_edge_indices.size()));
    for (size_t c = 1; c < a.cell_edge_offsets.size(); ++c) {
        CHECK(a.cell_edge_offsets[c] >= a.cell_edge_offsets[c - 1]);
    }

    const int32_t cells_per_axis = grid.n - 1;
    for (size_t c = 0; c < a.cell_id.size(); ++c) {
        const int32_t id = a.cell_id[c];
        const int32_t i = id % cells_per_axis;
        const int32_t j = (id / cells_per_axis) % cells_per_axis;
        const int32_t k = id / (cells_per_axis * cells_per_axis);
        for (int32_t e = a.cell_edge_offsets[c]; e < a.cell_edge_offsets[c + 1]; ++e) {
            const int32_t edge = a.cell_edge_indices[e];
            REQUIRE(edge >= 0);
            REQUIRE(static_cast<size_t>(edge) < a.edge_axis.size());
            const int32_t axis = a.edge_axis[edge];
            const Coord3 base = unflatten(a.edge_base[edge], grid.n);
            CAPTURE(c);
            CAPTURE(edge);
            if (axis == 0) {
                CHECK(base.x == i);
                CHECK((base.y == j || base.y == j + 1));
                CHECK((base.z == k || base.z == k + 1));
            } else if (axis == 1) {
                CHECK(base.y == j);
                CHECK((base.x == i || base.x == i + 1));
                CHECK((base.z == k || base.z == k + 1));
            } else {
                CHECK(base.z == k);
                CHECK((base.x == i || base.x == i + 1));
                CHECK((base.y == j || base.y == j + 1));
            }
        }
    }

    // Determinism: identical inputs -> bit-identical outputs (no hash-map
    // iteration order or other nondeterminism leaks into the result; the
    // build only uses a hash map for O(1) lookup, never to drive iteration
    // order of the output arrays).
    CHECK(a.edge_axis == b.edge_axis);
    CHECK(a.edge_base == b.edge_base);
    CHECK(a.cell_id == b.cell_id);
    CHECK(a.cell_edge_offsets == b.cell_edge_offsets);
    CHECK(a.cell_edge_indices == b.cell_edge_indices);
    REQUIRE(a.hermite_p.size() == b.hermite_p.size());
    for (size_t i = 0; i < a.hermite_p.size(); ++i) {
        check_float3_approx(a.hermite_p[i], b.hermite_p[i]);
        check_float3_approx(a.hermite_n[i], b.hermite_n[i]);
    }
    REQUIRE(a.cell_vertex.size() == b.cell_vertex.size());
    for (size_t i = 0; i < a.cell_vertex.size(); ++i) {
        check_float3_approx(a.cell_vertex[i], b.cell_vertex[i]);
    }
}
