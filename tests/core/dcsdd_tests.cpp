#include <doctest.h>

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <unordered_map>
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

// n=6 grid, spacing 1, origin (-2.5,-2.5,-2.5): the same sphere (r=1.3,
// centered at the world origin) as centered_sphere_grid(), but with an extra
// buffer layer of samples around it. The sphere-crossing region re-lands on
// the identical local topology (24 interesting edges, 26 interesting cells,
// same "missing the fully-enclosed center cell" pattern), just re-indexed
// into the larger cells_per_axis=5 space (interesting cell ids span ijk in
// [1,3]^3 instead of [0,2]^3) -- verified via the from-scratch numpy
// reference (dcsdd_ref.py) producing the same 24/26 counts. Used for D3's
// assignment/ring-search tests: the buffer is generous enough that even the
// grid's corner sample (the largest |s|, 3.0301270189221936) stays inside
// the narrow-band threshold 2*cell_diagonal (3.4641016151377544 at
// spacing=1) -- so in the unmodified fixture EVERY one of the 216 samples is
// narrow-band-eligible and (per python3 dcsdd_ref.assign_samples_ref)
// successfully assigned; the narrow-band-excluded and outlier test cases
// below construct those situations by overriding one sample's *value* only
// (distance-to-mesh is a pure function of position, so a value override
// can't move the mesh -- confirmed by the outlier case's pinned distance
// being bit-identical whether or not the override is applied).
SampleGrid sphere_grid_n6() {
    SampleGrid grid;
    grid.n = 6;
    grid.spacing = 1.0f;
    grid.origin = simd_float3{-2.5f, -2.5f, -2.5f};
    grid.values.resize(6u * 6u * 6u);
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

// =============================================================================
// D3: global mesh + sample assignment. All pinned literals below regenerated
// independently against the D3 brief's algorithm description (never by
// running this codebase's C++) via a from-scratch numpy reference
// (dcsdd_ref.py in the session scratchpad) extending the D2 reference above
// with closest_point_on_triangle / build_global_mesh / build_triangles /
// assign_samples_ref / triangle_bins. Driver: drive_d3.py in the same
// directory; per-case invocations are given in comments below.

// --- D3 item 1: closest_point_on_triangle ------------------------------------

TEST_CASE("closest_point_on_triangle: pinned cases hitting every Voronoi region, "
          "cross-checked against an independent numpy brute-force barycentric-grid "
          "search (steps=4000; exact match at these points -- each closest point "
          "lands exactly on a k/4000 barycentric grid vertex, so there is no "
          "discretization error to tolerate)") {
    // Fixed triangle a=(0,0,0), b=(2,0,0), c=(0,2,0) (right triangle, XY
    // plane). python3: for each p below,
    // dcsdd_ref.closest_point_on_triangle(p, a, b, c) names the Voronoi
    // region hit and returns cp; dcsdd_ref.brute_force_closest(p, a, b, c,
    // steps=4000) independently confirms both cp and distance (brute_err=0.0
    // for every case, printed by drive_d3.py).
    const simd_float3 a = {0.0f, 0.0f, 0.0f};
    const simd_float3 b = {2.0f, 0.0f, 0.0f};
    const simd_float3 c = {0.0f, 2.0f, 0.0f};

    SUBCASE("face interior") {
        // p=(0.5,0.5,1.0) -> cp=(0.5,0.5,0.0), dist=1.0
        const simd_float3 cp = closest_point_on_triangle(simd_float3{0.5f, 0.5f, 1.0f}, a, b, c);
        check_float3_approx(cp, simd_float3{0.5f, 0.5f, 0.0f});
    }
    SUBCASE("vertex_a region (d1<=0 && d2<=0)") {
        // p=(-1,-1,-1) -> cp=a=(0,0,0), dist=sqrt(3)=1.7320508075688772
        const simd_float3 cp = closest_point_on_triangle(simd_float3{-1.0f, -1.0f, -1.0f}, a, b, c);
        check_float3_approx(cp, a);
    }
    SUBCASE("vertex_b region (d3>=0 && d4<=d3)") {
        // p=(3,-1,1) -> cp=b=(2,0,0)
        const simd_float3 cp = closest_point_on_triangle(simd_float3{3.0f, -1.0f, 1.0f}, a, b, c);
        check_float3_approx(cp, b);
    }
    SUBCASE("vertex_c region (d6>=0 && d5<=d6)") {
        // p=(-1,3,1) -> cp=c=(0,2,0)
        const simd_float3 cp = closest_point_on_triangle(simd_float3{-1.0f, 3.0f, 1.0f}, a, b, c);
        check_float3_approx(cp, c);
    }
    SUBCASE("edge_ab region") {
        // p=(1,-1,0.5) -> cp=(1,0,0), dist=sqrt(1.25)=1.118033988749895
        const simd_float3 cp = closest_point_on_triangle(simd_float3{1.0f, -1.0f, 0.5f}, a, b, c);
        check_float3_approx(cp, simd_float3{1.0f, 0.0f, 0.0f});
    }
    SUBCASE("edge_ac region") {
        // p=(-1,1,0.5) -> cp=(0,1,0), dist=1.118033988749895
        const simd_float3 cp = closest_point_on_triangle(simd_float3{-1.0f, 1.0f, 0.5f}, a, b, c);
        check_float3_approx(cp, simd_float3{0.0f, 1.0f, 0.0f});
    }
    SUBCASE("edge_bc region") {
        // p=(1.5,1.5,0.5) -> cp=(1,1,0), dist=sqrt(0.75)=0.8660254037844386
        const simd_float3 cp = closest_point_on_triangle(simd_float3{1.5f, 1.5f, 0.5f}, a, b, c);
        check_float3_approx(cp, simd_float3{1.0f, 1.0f, 0.0f});
    }
}

// --- D3 items 2/3: global mesh — counts, cyclic order, winding -------------------

TEST_CASE("build_global_mesh: quad/triangle counts and the binding cyclic dense-cell "
          "order, one pinned quad per axis, on the D2 centered-sphere fixture") {
    // centered_sphere_grid() (n=4, spacing 1, origin (-1.5,-1.5,-1.5), r=1.3):
    // python3 confirms all 24 interesting edges have exactly 4 containing
    // cells (sorted(set(e['ncells'] for e in edges)) == [4]) -- no
    // boundary-interesting-edges on this fixture (matches the brief's "the
    // +10% sampling margin makes these rare" comment) -- so every edge
    // produces a quad: 24 quads, 48 triangles (2x).
    const SampleGrid grid = centered_sphere_grid();
    const DcsddInit init = dcsdd_init(grid);
    REQUIRE(init.edge_axis.size() == 24u);
    REQUIRE(init.cell_id.size() == 26u);

    const GlobalMesh mesh = build_global_mesh(init, grid, init.cell_vertex);
    CHECK(mesh.quad_edge.size() == 24u);
    CHECK(mesh.quad_cells.size() == 24u * 4u);
    CHECK(mesh.face_points.size() == 24u * 4u);
    CHECK(mesh.tri_cells.size() == 48u * 3u);

    // Since every edge produces a quad here, build_global_mesh's single pass
    // over interesting edges (in DcsddInit order) means quad index == edge
    // index -- verified below alongside each pinned quad.
    //
    // python3 dcsdd_ref.build_global_mesh(..., reverse_when_outside=True),
    // one quad per axis (first one found with that axis):
    struct PinnedQuad {
        int32_t axis;
        int32_t edge_idx;
        int32_t dense[4];
    };
    const PinnedQuad pinned[] = {
        // axis 0, edge_idx 0, base (0,1,1), s_base=0.358312 (>=0, outside ->
        // reversed): ijk cyclic (post-reversal)
        // [(0,0,1),(0,1,1),(0,1,0),(0,0,0)] -> dense [9,12,3,0]
        {0, 0, {9, 12, 3, 0}},
        // axis 1, edge_idx 8, base (1,0,1): dense [1,10,9,0]
        {1, 8, {1, 10, 9, 0}},
        // axis 2, edge_idx 16, base (1,1,0): dense [3,4,1,0]
        {2, 16, {3, 4, 1, 0}},
    };
    for (const PinnedQuad& pq : pinned) {
        CAPTURE(pq.axis);
        REQUIRE(static_cast<size_t>(pq.edge_idx) < init.edge_axis.size());
        CHECK(init.edge_axis[pq.edge_idx] == pq.axis);
        CHECK(mesh.quad_edge[pq.edge_idx] == pq.edge_idx);
        for (int i = 0; i < 4; ++i) {
            CAPTURE(i);
            CHECK(mesh.quad_cells[4 * pq.edge_idx + i] == pq.dense[i]);
        }
    }
}

TEST_CASE("build_global_mesh: winding/orientation acceptance -- every triangle's "
          "geometric normal points away from the sphere center") {
    // python3 cross-check on this fixture: with reverse_when_outside=True
    // (the brief's rule), 0/48 triangles have non-positive
    // dot(normal, centroid-center) (min dot 0.40878666952986425); with
    // reverse_when_outside=False (never reverse), exactly 24/48 fail -- so
    // the brief's rule is the one that wins outright, no flip needed.
    const SampleGrid grid = centered_sphere_grid();
    const DcsddInit init = dcsdd_init(grid);
    const GlobalMesh mesh = build_global_mesh(init, grid, init.cell_vertex);
    REQUIRE(mesh.tri_cells.size() == 48u * 3u);

    const simd_float3 sphere_center = {0.0f, 0.0f, 0.0f};
    const size_t num_tris = mesh.tri_cells.size() / 3;
    for (size_t t = 0; t < num_tris; ++t) {
        const simd_float3 v0 = init.cell_vertex[mesh.tri_cells[3 * t + 0]];
        const simd_float3 v1 = init.cell_vertex[mesh.tri_cells[3 * t + 1]];
        const simd_float3 v2 = init.cell_vertex[mesh.tri_cells[3 * t + 2]];
        const simd_float3 normal = simd_cross(v1 - v0, v2 - v0);
        const simd_float3 centroid = (v0 + v1 + v2) / 3.0f;
        CAPTURE(t);
        CHECK(simd_dot(normal, centroid - sphere_center) > 0.0f);
    }
}

// --- D3 item 4: face intersection points -----------------------------------------

TEST_CASE("build_global_mesh: face intersection points -- straddling case (numpy "
          "t/p), clamp guard (both endpoints on one side, both directions), and the "
          "parallel guard (t=0.5)") {
    // Same fixture/quad as the cyclic-order test above: axis-0 quad,
    // edge_idx 0, base (0,1,1), dense cells [9,12,3,0] -> face_points[0] is
    // the mesh edge between dense 9 and dense 12. Those two cells differ
    // along axis 1 (y); their shared face plane is at world
    // y = origin.y + spacing*max(0,1) = -1.5 + 1*1 = -0.5 (python3:
    // diff_axis=1, plane=-0.5). cell_vertices is a *separate* input from
    // build_global_mesh's edge/cell topology (which comes from `init`/grid
    // values only), so overriding entries 9/12 here changes only the lerp
    // endpoints, not which cells the quad contains or its winding.
    const SampleGrid grid = centered_sphere_grid();
    const DcsddInit init = dcsdd_init(grid);
    REQUIRE(init.edge_axis[0] == 0);

    SUBCASE("straddling: t and p match the numpy lerp") {
        // python3: va=(1,-1.1,2), vb=(3,-0.2,-1) -> denom=0.9,
        // t=0.6666666666666666, p=(2.333333333333333,-0.5,0.0)
        std::vector<simd_float3> verts = init.cell_vertex;
        verts[9] = simd_float3{1.0f, -1.1f, 2.0f};
        verts[12] = simd_float3{3.0f, -0.2f, -1.0f};
        const GlobalMesh mesh = build_global_mesh(init, grid, verts);
        check_float3_approx(mesh.face_points[0], simd_float3{2.333333333333333f, -0.5f, 0.0f});
    }
    SUBCASE("clamp guard: both endpoints below the plane -> t clamps to 1 (== vb)") {
        // python3: va=(0.5,-1.4,-0.5), vb=(0.5,-0.7,-0.5) -- both y<-0.5,
        // denom=0.7>0, unclamped t=(-0.5-(-1.4))/0.7~=1.286>1 -> t=1,
        // p=vb=(0.5,-0.7,-0.5)
        std::vector<simd_float3> verts = init.cell_vertex;
        verts[9] = simd_float3{0.5f, -1.4f, -0.5f};
        verts[12] = simd_float3{0.5f, -0.7f, -0.5f};
        const GlobalMesh mesh = build_global_mesh(init, grid, verts);
        check_float3_approx(mesh.face_points[0], simd_float3{0.5f, -0.7f, -0.5f});
    }
    SUBCASE("clamp guard: both endpoints above the plane -> t clamps to 0 (== va)") {
        // python3: va=(0.5,-0.3,-0.5), vb=(0.5,0.4,-0.5) -- both y>-0.5,
        // denom=0.7>0, unclamped t=(-0.5-(-0.3))/0.7~=-0.286<0 -> t=0,
        // p=va=(0.5,-0.3,-0.5)
        std::vector<simd_float3> verts = init.cell_vertex;
        verts[9] = simd_float3{0.5f, -0.3f, -0.5f};
        verts[12] = simd_float3{0.5f, 0.4f, -0.5f};
        const GlobalMesh mesh = build_global_mesh(init, grid, verts);
        check_float3_approx(mesh.face_points[0], simd_float3{0.5f, -0.3f, -0.5f});
    }
    SUBCASE("parallel guard: denom ~ 0 (equal y) -> t=0.5, midpoint") {
        // python3: va=(1,-0.9,2), vb=(5,-0.9,-3) -- same y, denom=0 -> t=0.5,
        // p=(3,-0.9,-0.5)
        std::vector<simd_float3> verts = init.cell_vertex;
        verts[9] = simd_float3{1.0f, -0.9f, 2.0f};
        verts[12] = simd_float3{5.0f, -0.9f, -3.0f};
        const GlobalMesh mesh = build_global_mesh(init, grid, verts);
        check_float3_approx(mesh.face_points[0], simd_float3{3.0f, -0.9f, -0.5f});
    }
}

// --- D3 item 5: sample assignment -------------------------------------------------

TEST_CASE("assign_samples: pinned cell landing, narrow-band exclusion, outlier "
          "rejection, and CSR integrity, on an n=6 buffered sphere fixture") {
    const SampleGrid grid = sphere_grid_n6();
    const DcsddInit init = dcsdd_init(grid);
    REQUIRE(init.edge_axis.size() == 24u);
    REQUIRE(init.cell_id.size() == 26u);
    const GlobalMesh mesh = build_global_mesh(init, grid, init.cell_vertex);
    REQUIRE(mesh.tri_cells.size() == 48u * 3u);

    const float cell_diag = grid.spacing * std::sqrt(3.0f); // 1.7320508075688772
    const float two_cell_diag = 2.0f * cell_diag;           // 3.4641016151377544

    SUBCASE("specific samples land in their expected dense cells") {
        // python3 assign_samples_ref: sample (0,0,0) [flat 0], s=3.030127018922194
        // -> dense_cell 0, dist 3.1478584448366695. sample (3,3,3)
        // [flat_index(3,3,3,6)=129] -> dense_cell 25.
        const SampleAssignment sa = assign_samples(grid, init, init.cell_vertex, mesh);
        REQUIRE(sa.cell_sample_offsets.size() == init.cell_id.size() + 1);

        bool found0 = false;
        for (int32_t i = sa.cell_sample_offsets[0]; i < sa.cell_sample_offsets[1]; ++i) {
            if (sa.cell_sample_indices[i] == 0) found0 = true;
        }
        CHECK(found0);

        bool found25 = false;
        const int32_t flat333 = static_cast<int32_t>(flat_index(3, 3, 3, 6));
        for (int32_t i = sa.cell_sample_offsets[25]; i < sa.cell_sample_offsets[26]; ++i) {
            if (sa.cell_sample_indices[i] == flat333) found25 = true;
        }
        CHECK(found25);
    }

    SUBCASE("sample outside the narrow band is absent from the CSR (constructed by "
            "overriding one sample's value -- the mesh is unaffected by it)") {
        // python3: overriding sample (5,5,5) [flat 215] to s=10.0 ->
        // abs(s) >= 2*cell_diag (3.4641016151377544) -> narrow_band_excluded.
        SampleGrid overridden = grid;
        const int32_t flat215 = static_cast<int32_t>(flat_index(5, 5, 5, 6));
        overridden.values[flat215] = 10.0f;
        REQUIRE(std::fabs(overridden.values[flat215]) >= two_cell_diag);

        const SampleAssignment sa = assign_samples(overridden, init, init.cell_vertex, mesh);
        for (size_t c = 0; c < init.cell_id.size(); ++c) {
            for (int32_t i = sa.cell_sample_offsets[c]; i < sa.cell_sample_offsets[c + 1]; ++i) {
                CHECK(sa.cell_sample_indices[i] != flat215);
            }
        }
    }

    SUBCASE("outlier: |s| small but the closest point on the mesh is far -> dropped") {
        // python3: overriding sample (0,0,0) [flat 0] to s=0.05 -> distance
        // to mesh 3.1478584448366695 (position-only, unaffected by the
        // value override) > cell_diagonal + |s| = 1.7320508075688772 + 0.05
        // = 1.7820508075688772 -> outlier, dropped (note: this same sample
        // is assigned to dense_cell 0 when *not* overridden -- see above --
        // confirming the mesh geometry itself didn't move).
        SampleGrid overridden = grid;
        overridden.values[0] = 0.05f;

        const SampleAssignment sa = assign_samples(overridden, init, init.cell_vertex, mesh);
        for (size_t c = 0; c < init.cell_id.size(); ++c) {
            for (int32_t i = sa.cell_sample_offsets[c]; i < sa.cell_sample_offsets[c + 1]; ++i) {
                CHECK(sa.cell_sample_indices[i] != 0);
            }
        }
    }

    SUBCASE("CSR integrity: offsets monotone, sizes match, indices unique per cell") {
        const SampleAssignment sa = assign_samples(grid, init, init.cell_vertex, mesh);
        REQUIRE(sa.cell_sample_offsets.size() == init.cell_id.size() + 1);
        CHECK(sa.cell_sample_offsets.front() == 0);
        CHECK(sa.cell_sample_offsets.back() == static_cast<int32_t>(sa.cell_sample_indices.size()));
        for (size_t c = 1; c < sa.cell_sample_offsets.size(); ++c) {
            CHECK(sa.cell_sample_offsets[c] >= sa.cell_sample_offsets[c - 1]);
        }
        for (size_t c = 0; c < init.cell_id.size(); ++c) {
            std::vector<int32_t> seen;
            for (int32_t i = sa.cell_sample_offsets[c]; i < sa.cell_sample_offsets[c + 1]; ++i) {
                const int32_t s = sa.cell_sample_indices[i];
                CAPTURE(c);
                CHECK(std::find(seen.begin(), seen.end(), s) == seen.end());
                seen.push_back(s);
            }
        }
    }
}

// --- D3 item 6: ring search --------------------------------------------------------

TEST_CASE("assign_samples: ring search finds a hit when the sample's own cell has no "
          "triangle but ring 1 does; and matches a brute-force all-triangles scan for "
          "every narrow-band sample in the fixture") {
    const SampleGrid grid = sphere_grid_n6();
    const DcsddInit init = dcsdd_init(grid);
    const GlobalMesh mesh = build_global_mesh(init, grid, init.cell_vertex);

    SUBCASE("own-cell-empty, ring-1-hit: sample (0,0,0)") {
        // python3 (triangle_bins on this fixture): occupied bin cells only
        // span [1,3]^3 (the sphere sits well away from the outer buffer
        // shell) -- sample (0,0,0)'s own cell is (0,0,0)
        // (floor((p-origin)/spacing), clamped), which has zero triangles
        // binned into it, but ring 1 (Chebyshev distance 1 -- e.g. cell
        // (1,1,1)) does. A from-scratch python emulation of the brief's
        // expanding-ring termination rule (mirroring the production
        // algorithm below) confirms assignment still succeeds here (dense
        // cell 0, matching the brute-force reference) with 0 mismatches
        // across all 216 narrow-band samples in this fixture -- see the
        // brute-force SUBCASE below for the same check against the actual
        // C++ implementation.
        const SampleAssignment sa = assign_samples(grid, init, init.cell_vertex, mesh);
        bool found = false;
        for (int32_t i = sa.cell_sample_offsets[0]; i < sa.cell_sample_offsets[1]; ++i) {
            if (sa.cell_sample_indices[i] == 0) found = true;
        }
        CHECK(found);
    }

    SUBCASE("brute-force equivalence: for every narrow-band sample, the assigned cell "
            "(or absence) matches an all-triangles brute-force scan written here, "
            "independent of the production binning/ring-search acceleration") {
        const SampleAssignment sa = assign_samples(grid, init, init.cell_vertex, mesh);

        // dense_cell -> assigned sample flat indices, from production output.
        std::vector<std::vector<int32_t>> produced(init.cell_id.size());
        for (size_t c = 0; c < init.cell_id.size(); ++c) {
            for (int32_t i = sa.cell_sample_offsets[c]; i < sa.cell_sample_offsets[c + 1]; ++i) {
                produced[c].push_back(sa.cell_sample_indices[i]);
            }
        }

        std::unordered_map<int32_t, int32_t> id_to_dense;
        for (size_t c = 0; c < init.cell_id.size(); ++c) {
            id_to_dense[init.cell_id[c]] = static_cast<int32_t>(c);
        }

        const int32_t n = grid.n;
        const int32_t cells_per_axis = n - 1;
        const float cell_diag = grid.spacing * std::sqrt(3.0f);
        const size_t num_tris = mesh.tri_cells.size() / 3;

        for (int32_t z = 0; z < n; ++z) {
            for (int32_t y = 0; y < n; ++y) {
                for (int32_t x = 0; x < n; ++x) {
                    const int32_t flat = static_cast<int32_t>(flat_index(x, y, z, n));
                    const float s = grid.values[flat];
                    if (std::fabs(s) >= 2.0f * cell_diag) continue; // narrow-band excluded

                    const simd_float3 p =
                        grid.origin + grid.spacing * simd_float3{float(x), float(y), float(z)};

                    // Brute-force: scan ALL triangles directly (no
                    // binning/ring search -- that acceleration is
                    // production-only, cross-checked here against this
                    // completely independent scan).
                    float best_dist = FLT_MAX;
                    simd_float3 best_pt{0.0f, 0.0f, 0.0f};
                    for (size_t t = 0; t < num_tris; ++t) {
                        const simd_float3 va = init.cell_vertex[mesh.tri_cells[3 * t + 0]];
                        const simd_float3 vb = init.cell_vertex[mesh.tri_cells[3 * t + 1]];
                        const simd_float3 vc = init.cell_vertex[mesh.tri_cells[3 * t + 2]];
                        const simd_float3 cp = closest_point_on_triangle(p, va, vb, vc);
                        const float d = simd_length(p - cp);
                        if (d < best_dist) {
                            best_dist = d;
                            best_pt = cp;
                        }
                    }

                    int32_t expected_dense = -1; // -1 == not assigned anywhere
                    if (best_dist <= cell_diag + std::fabs(s)) {
                        int32_t ix = static_cast<int32_t>(std::floor((best_pt.x - grid.origin.x) / grid.spacing));
                        int32_t iy = static_cast<int32_t>(std::floor((best_pt.y - grid.origin.y) / grid.spacing));
                        int32_t iz = static_cast<int32_t>(std::floor((best_pt.z - grid.origin.z) / grid.spacing));
                        ix = std::clamp(ix, 0, cells_per_axis - 1);
                        iy = std::clamp(iy, 0, cells_per_axis - 1);
                        iz = std::clamp(iz, 0, cells_per_axis - 1);
                        const int32_t cid = ix + cells_per_axis * (iy + cells_per_axis * iz);
                        const auto it = id_to_dense.find(cid);
                        if (it != id_to_dense.end()) expected_dense = it->second;
                    }

                    int32_t actual_dense = -1;
                    for (size_t c = 0; c < produced.size(); ++c) {
                        for (int32_t v : produced[c]) {
                            if (v == flat) actual_dense = static_cast<int32_t>(c);
                        }
                    }
                    CAPTURE(x);
                    CAPTURE(y);
                    CAPTURE(z);
                    CHECK(actual_dense == expected_dense);
                }
            }
        }
    }
}
