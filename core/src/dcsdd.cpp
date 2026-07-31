#include "dcsdd.h"

#include <algorithm>
#include <array>
#include <cassert>
#include <cfloat>
#include <cmath>
#include <thread>
#include <unordered_map>
#include <utility>

namespace sq {

HermitePoint hermite_crossing(float s_a, float s_b, simd_float3 u_a, simd_float3 u_b) {
    const float denom = std::fabs(s_a) + std::fabs(s_b);
    // Guard: both samples land exactly on the surface -> no magnitude
    // information to weight the interpolation by; fall back to the edge
    // midpoint.
    const float t = (denom == 0.0f) ? 0.5f : std::fabs(s_a) / denom;
    return HermitePoint{(1.0f - t) * u_a + t * u_b, t};
}

simd_float3 closest_point_on_triangle(simd_float3 p, simd_float3 a, simd_float3 b, simd_float3 c) {
    // Ericson, "Real-Time Collision Detection" §5.1.5: check each vertex's
    // and edge's Voronoi region in turn (via the standard barycentric sign
    // tests), falling through to the face interior if none matched.
    const simd_float3 ab = b - a;
    const simd_float3 ac = c - a;
    const simd_float3 ap = p - a;
    const float d1 = simd_dot(ab, ap);
    const float d2 = simd_dot(ac, ap);
    if (d1 <= 0.0f && d2 <= 0.0f) return a; // vertex region a

    const simd_float3 bp = p - b;
    const float d3 = simd_dot(ab, bp);
    const float d4 = simd_dot(ac, bp);
    if (d3 >= 0.0f && d4 <= d3) return b; // vertex region b

    const float vc = d1 * d4 - d3 * d2;
    if (vc <= 0.0f && d1 >= 0.0f && d3 <= 0.0f) {
        const float v = d1 / (d1 - d3);
        return a + v * ab; // edge region ab
    }

    const simd_float3 cp = p - c;
    const float d5 = simd_dot(ab, cp);
    const float d6 = simd_dot(ac, cp);
    if (d6 >= 0.0f && d5 <= d6) return c; // vertex region c

    const float vb_ = d5 * d2 - d1 * d6;
    if (vb_ <= 0.0f && d2 >= 0.0f && d6 <= 0.0f) {
        const float w = d2 / (d2 - d6);
        return a + w * ac; // edge region ac
    }

    const float va_ = d3 * d6 - d5 * d4;
    if (va_ <= 0.0f && (d4 - d3) >= 0.0f && (d5 - d6) >= 0.0f) {
        const float w = (d4 - d3) / ((d4 - d3) + (d5 - d6));
        return b + w * (c - b); // edge region bc
    }

    // Face interior: barycentric coords from the three Voronoi determinants.
    const float denom = 1.0f / (va_ + vb_ + vc);
    const float v = vb_ * denom;
    const float w = vc * denom;
    return a + ab * v + ac * w;
}

TriangleBarycentric closest_point_on_triangle_barycentric(simd_float3 p, simd_float3 a, simd_float3 b,
                                                            simd_float3 c) {
    // Identical Voronoi-region derivation to closest_point_on_triangle above,
    // additionally reporting the barycentric weights at each region.
    const simd_float3 ab = b - a;
    const simd_float3 ac = c - a;
    const simd_float3 ap = p - a;
    const float d1 = simd_dot(ab, ap);
    const float d2 = simd_dot(ac, ap);
    if (d1 <= 0.0f && d2 <= 0.0f) return TriangleBarycentric{a, 1.0f, 0.0f, 0.0f}; // vertex region a

    const simd_float3 bp = p - b;
    const float d3 = simd_dot(ab, bp);
    const float d4 = simd_dot(ac, bp);
    if (d3 >= 0.0f && d4 <= d3) return TriangleBarycentric{b, 0.0f, 1.0f, 0.0f}; // vertex region b

    const float vc = d1 * d4 - d3 * d2;
    if (vc <= 0.0f && d1 >= 0.0f && d3 <= 0.0f) {
        const float v = d1 / (d1 - d3);
        return TriangleBarycentric{a + v * ab, 1.0f - v, v, 0.0f}; // edge region ab
    }

    const simd_float3 cp = p - c;
    const float d5 = simd_dot(ab, cp);
    const float d6 = simd_dot(ac, cp);
    if (d6 >= 0.0f && d5 <= d6) return TriangleBarycentric{c, 0.0f, 0.0f, 1.0f}; // vertex region c

    const float vb_ = d5 * d2 - d1 * d6;
    if (vb_ <= 0.0f && d2 >= 0.0f && d6 <= 0.0f) {
        const float w = d2 / (d2 - d6);
        return TriangleBarycentric{a + w * ac, 1.0f - w, 0.0f, w}; // edge region ac
    }

    const float va_ = d3 * d6 - d5 * d4;
    if (va_ <= 0.0f && (d4 - d3) >= 0.0f && (d5 - d6) >= 0.0f) {
        const float w = (d4 - d3) / ((d4 - d3) + (d5 - d6));
        return TriangleBarycentric{b + w * (c - b), 0.0f, 1.0f - w, w}; // edge region bc
    }

    // Face interior: barycentric coords from the three Voronoi determinants.
    const float denom = 1.0f / (va_ + vb_ + vc);
    const float v = vb_ * denom;
    const float w = vc * denom;
    return TriangleBarycentric{a + ab * v + ac * w, 1.0f - v - w, v, w};
}

namespace {

// Flat sample index, matching SampleGrid::values' documented convention.
size_t flat_index(int32_t x, int32_t y, int32_t z, int32_t n) {
    const size_t un = static_cast<size_t>(n);
    return static_cast<size_t>(x) + un * (static_cast<size_t>(y) + un * static_cast<size_t>(z));
}

// World position of sample (x,y,z).
simd_float3 sample_pos(const SampleGrid& grid, int32_t x, int32_t y, int32_t z) {
    return grid.origin + grid.spacing * simd_float3{float(x), float(y), float(z)};
}

// A sample is inside when its value is < 0 (exact 0 counts as outside).
bool inside(float s) {
    return s < 0.0f;
}

// Build-local key identifying an edge (axis, x, y, z), for the hash-map
// lookup used only inside dcsdd_init (never stored in DcsddInit).
int64_t edge_key(int32_t axis, int32_t x, int32_t y, int32_t z, int32_t n) {
    const int64_t n3 = static_cast<int64_t>(n) * n * n;
    return static_cast<int64_t>(axis) * n3 + static_cast<int64_t>(flat_index(x, y, z, n));
}

struct CellCoord {
    int32_t i, j, k;
};

// Cells adjacent around an edge (axis, x, y, z): the coordinate along `axis`
// is fixed at the base sample's coordinate (edge validity already
// guarantees it's a valid cell index); the other two coordinates each range
// over {coord-1, coord}, clamped to the valid cell range [0, n-2] (boundary
// edges yield 2 or 1 cells instead of the interior 4).
std::vector<CellCoord> containing_cells(int32_t axis, int32_t x, int32_t y, int32_t z, int32_t n) {
    std::vector<CellCoord> cells;
    cells.reserve(4);
    const int32_t cell_max = n - 2;
    auto in_range = [cell_max](int32_t v) { return v >= 0 && v <= cell_max; };
    if (axis == 0) {
        for (int32_t jj : {y - 1, y}) {
            if (!in_range(jj)) continue;
            for (int32_t kk : {z - 1, z}) {
                if (!in_range(kk)) continue;
                cells.push_back({x, jj, kk});
            }
        }
    } else if (axis == 1) {
        for (int32_t ii : {x - 1, x}) {
            if (!in_range(ii)) continue;
            for (int32_t kk : {z - 1, z}) {
                if (!in_range(kk)) continue;
                cells.push_back({ii, y, kk});
            }
        }
    } else {
        for (int32_t ii : {x - 1, x}) {
            if (!in_range(ii)) continue;
            for (int32_t jj : {y - 1, y}) {
                if (!in_range(jj)) continue;
                cells.push_back({ii, jj, z});
            }
        }
    }
    return cells;
}

// Analytic gradient (w.r.t. world coordinates) of `cell`'s trilinear
// interpolant at world point `p`. Local coords (u,v,w) = (p -
// cell_min_corner)/spacing; the gradient w.r.t. (u,v,w) is differentiated
// directly from the trilinear basis, then divided by `spacing` for the
// world->local change of variables (constant factor, cancels under
// normalization in hermite_normal below, but kept for correctness).
simd_float3 trilinear_gradient(const SampleGrid& grid, CellCoord cell, simd_float3 p) {
    const int32_t n = grid.n;
    auto corner = [&](int32_t di, int32_t dj, int32_t dk) {
        return grid.values[flat_index(cell.i + di, cell.j + dj, cell.k + dk, n)];
    };
    const float c000 = corner(0, 0, 0), c100 = corner(1, 0, 0), c010 = corner(0, 1, 0), c110 = corner(1, 1, 0);
    const float c001 = corner(0, 0, 1), c101 = corner(1, 0, 1), c011 = corner(0, 1, 1), c111 = corner(1, 1, 1);

    const simd_float3 cell_min =
        grid.origin + grid.spacing * simd_float3{float(cell.i), float(cell.j), float(cell.k)};
    const simd_float3 local = (p - cell_min) / grid.spacing; // (u,v,w)
    const float u = local.x, v = local.y, w = local.z;

    const float dfdu = (1 - v) * (1 - w) * (c100 - c000) + v * (1 - w) * (c110 - c010) +
                        (1 - v) * w * (c101 - c001) + v * w * (c111 - c011);
    const float dfdv = (1 - u) * (1 - w) * (c010 - c000) + u * (1 - w) * (c110 - c100) +
                        (1 - u) * w * (c011 - c001) + u * w * (c111 - c101);
    const float dfdw = (1 - u) * (1 - v) * (c001 - c000) + u * (1 - v) * (c101 - c100) +
                        (1 - u) * v * (c011 - c010) + u * v * (c111 - c110);

    return simd_float3{dfdu, dfdv, dfdw} / grid.spacing;
}

// Eq. 2: sum the containing cells' trilinear-gradient evaluations at h0 and
// normalize. Guard: if the sum is (near-)zero length — the containing
// cells' gradients cancel — fall back to the edge direction, signed from
// the inside sample to the outside one.
simd_float3 hermite_normal(const SampleGrid& grid, int32_t axis, int32_t x, int32_t y, int32_t z, simd_float3 h0,
                            float s_a, simd_float3 u_a, simd_float3 u_b) {
    const std::vector<CellCoord> cells = containing_cells(axis, x, y, z, grid.n);
    simd_float3 sum = {0.0f, 0.0f, 0.0f};
    for (const CellCoord& cell : cells) {
        sum += trilinear_gradient(grid, cell, h0);
    }
    const float len = simd_length(sum);
    if (len > 1e-8f) {
        return sum / len;
    }
    const bool a_inside = s_a < 0.0f;
    const simd_float3 dir = a_inside ? (u_b - u_a) : (u_a - u_b);
    return simd_normalize(dir);
}

// --- D3 helpers: global mesh + sample assignment (build-local only) -------

// Decodes a flat sample index back to (x,y,z) — the inverse of flat_index
// above, used to recover an interesting edge's base-sample coordinates from
// `DcsddInit::edge_base`.
struct Coord3 {
    int32_t x, y, z;
};
Coord3 unflatten(int32_t flat, int32_t n) {
    return Coord3{flat % n, (flat / n) % n, flat / (n * n)};
}

// Cell id encoding, matching dcsdd_init's cell_id convention
// (i + cells_per_axis*(j + cells_per_axis*k)).
int32_t encode_cell_id(CellCoord c, int32_t cells_per_axis) {
    return c.i + cells_per_axis * (c.j + cells_per_axis * c.k);
}

int32_t cell_component(CellCoord c, int32_t axis) {
    return axis == 0 ? c.i : (axis == 1 ? c.j : c.k);
}

// The brief's binding cyclic order for the 4 cells around an interesting
// edge (axis, x, y, z) — NOT yet reversed for outsideness. For an edge along
// axis `a` with base sample (x,y,z), enumerate cyclically in the plane of
// the other two axes (b,c) — (a,b,c) a right-handed permutation
// (x->(y,z), y->(z,x), z->(x,y)) — as (b-1,c-1) -> (b,c-1) -> (b,c) ->
// (b-1,c), where b,c are the base sample's coordinates on those axes.
std::array<CellCoord, 4> cyclic_quad_cells(int32_t axis, int32_t x, int32_t y, int32_t z) {
    if (axis == 0) {
        const int32_t b = y, c = z;
        return {CellCoord{x, b - 1, c - 1}, CellCoord{x, b, c - 1}, CellCoord{x, b, c}, CellCoord{x, b - 1, c}};
    } else if (axis == 1) {
        const int32_t b = z, c = x;
        return {CellCoord{c - 1, y, b - 1}, CellCoord{c - 1, y, b}, CellCoord{c, y, b}, CellCoord{c, y, b - 1}};
    } else {
        const int32_t b = x, c = y;
        return {CellCoord{b - 1, c - 1, z}, CellCoord{b, c - 1, z}, CellCoord{b, c, z}, CellCoord{b - 1, c, z}};
    }
}

// Uniform-grid triangle binning (assign_samples' acceleration structure,
// build-local only — never stored in SampleAssignment): CSR over ALL grid
// cells (cells_per_axis^3, not just interesting ones), each triangle binned
// into every cell its AABB overlaps (triangles are ~cell-sized, so this is a
// handful of cells each).
struct TriangleBins {
    std::vector<int32_t> offsets; // size cells_per_axis^3 + 1
    std::vector<int32_t> indices; // triangle indices (into mesh.tri_cells, /3)
};

TriangleBins build_triangle_bins(const SampleGrid& grid, const GlobalMesh& mesh,
                                  const std::vector<simd_float3>& cell_vertices) {
    const int32_t cells_per_axis = grid.n - 1;
    const size_t total_cells = static_cast<size_t>(cells_per_axis) * cells_per_axis * cells_per_axis;
    const size_t num_tris = mesh.tri_cells.size() / 3;

    auto clamp_idx = [cells_per_axis](int32_t v) { return std::clamp(v, 0, cells_per_axis - 1); };
    auto cell_of = [&](float world_coord, int32_t axis) {
        return clamp_idx(static_cast<int32_t>(std::floor((world_coord - grid.origin[axis]) / grid.spacing)));
    };
    auto cell_id_of = [cells_per_axis](int32_t i, int32_t j, int32_t k) {
        const size_t cpa = static_cast<size_t>(cells_per_axis);
        return static_cast<size_t>(i) + cpa * (static_cast<size_t>(j) + cpa * static_cast<size_t>(k));
    };

    struct Range {
        int32_t lo[3], hi[3];
    };
    std::vector<Range> ranges(num_tris);
    std::vector<int32_t> counts(total_cells + 1, 0);
    for (size_t t = 0; t < num_tris; ++t) {
        const simd_float3 a = cell_vertices[mesh.tri_cells[3 * t + 0]];
        const simd_float3 b = cell_vertices[mesh.tri_cells[3 * t + 1]];
        const simd_float3 c = cell_vertices[mesh.tri_cells[3 * t + 2]];
        const simd_float3 lo = simd_min(simd_min(a, b), c);
        const simd_float3 hi = simd_max(simd_max(a, b), c);
        Range r;
        for (int32_t axis = 0; axis < 3; ++axis) {
            r.lo[axis] = cell_of(lo[axis], axis);
            r.hi[axis] = cell_of(hi[axis], axis);
        }
        ranges[t] = r;
        for (int32_t kk = r.lo[2]; kk <= r.hi[2]; ++kk) {
            for (int32_t jj = r.lo[1]; jj <= r.hi[1]; ++jj) {
                for (int32_t ii = r.lo[0]; ii <= r.hi[0]; ++ii) {
                    counts[cell_id_of(ii, jj, kk) + 1] += 1;
                }
            }
        }
    }
    for (size_t c = 0; c < total_cells; ++c) counts[c + 1] += counts[c];

    TriangleBins bins;
    bins.offsets = counts; // prefix-summed in place above -> now the CSR offsets
    bins.indices.resize(static_cast<size_t>(bins.offsets.back()));
    std::vector<int32_t> cursor(total_cells, 0);
    for (size_t t = 0; t < num_tris; ++t) {
        const Range& r = ranges[t];
        for (int32_t kk = r.lo[2]; kk <= r.hi[2]; ++kk) {
            for (int32_t jj = r.lo[1]; jj <= r.hi[1]; ++jj) {
                for (int32_t ii = r.lo[0]; ii <= r.hi[0]; ++ii) {
                    const size_t cid = cell_id_of(ii, jj, kk);
                    bins.indices[static_cast<size_t>(bins.offsets[cid]) + cursor[cid]] = static_cast<int32_t>(t);
                    cursor[cid] += 1;
                }
            }
        }
    }
    return bins;
}

} // namespace

DcsddInit dcsdd_init(const SampleGrid& grid) {
    assert(grid.n >= 2 && "dcsdd_init: grid.n must be >= 2");
    const int32_t n = grid.n;
    DcsddInit out;

    // Build-local lookup: edge key -> dense index into the edge_*/hermite_*
    // arrays below. Hash maps are used only here, never stored in DcsddInit.
    std::unordered_map<int64_t, int32_t> edge_index;

    // --- Interesting edges: axis-major, then z,y,x ascending. ----------------
    for (int32_t axis = 0; axis < 3; ++axis) {
        for (int32_t z = 0; z < n; ++z) {
            for (int32_t y = 0; y < n; ++y) {
                for (int32_t x = 0; x < n; ++x) {
                    const int32_t base_coord = (axis == 0) ? x : (axis == 1) ? y : z;
                    if (base_coord >= n - 1) continue; // edge would run past the grid

                    const int32_t x2 = x + (axis == 0 ? 1 : 0);
                    const int32_t y2 = y + (axis == 1 ? 1 : 0);
                    const int32_t z2 = z + (axis == 2 ? 1 : 0);

                    const float s_a = grid.values[flat_index(x, y, z, n)];
                    const float s_b = grid.values[flat_index(x2, y2, z2, n)];
                    if (inside(s_a) == inside(s_b)) continue; // not interesting

                    const simd_float3 u_a = sample_pos(grid, x, y, z);
                    const simd_float3 u_b = sample_pos(grid, x2, y2, z2);
                    const HermitePoint hp = hermite_crossing(s_a, s_b, u_a, u_b);
                    const simd_float3 normal = hermite_normal(grid, axis, x, y, z, hp.p, s_a, u_a, u_b);

                    const int32_t idx = static_cast<int32_t>(out.edge_axis.size());
                    out.edge_axis.push_back(axis);
                    out.edge_base.push_back(static_cast<int32_t>(flat_index(x, y, z, n)));
                    out.hermite_p.push_back(hp.p);
                    out.hermite_n.push_back(normal);
                    edge_index[edge_key(axis, x, y, z, n)] = idx;
                }
            }
        }
    }

    // --- Interesting cells + CSR: z,y,x ascending. ----------------------------
    out.cell_edge_offsets.push_back(0);
    const int32_t cells_per_axis = n - 1;
    for (int32_t k = 0; k < cells_per_axis; ++k) {
        for (int32_t j = 0; j < cells_per_axis; ++j) {
            for (int32_t i = 0; i < cells_per_axis; ++i) {
                // The cell's 12 candidate edges, axis-major.
                std::vector<int32_t> refs;
                refs.reserve(12);
                for (int32_t dz : {0, 1}) {
                    for (int32_t dy : {0, 1}) {
                        const auto it = edge_index.find(edge_key(0, i, j + dy, k + dz, n));
                        if (it != edge_index.end()) refs.push_back(it->second);
                    }
                }
                for (int32_t dz : {0, 1}) {
                    for (int32_t dx : {0, 1}) {
                        const auto it = edge_index.find(edge_key(1, i + dx, j, k + dz, n));
                        if (it != edge_index.end()) refs.push_back(it->second);
                    }
                }
                for (int32_t dy : {0, 1}) {
                    for (int32_t dx : {0, 1}) {
                        const auto it = edge_index.find(edge_key(2, i + dx, j + dy, k, n));
                        if (it != edge_index.end()) refs.push_back(it->second);
                    }
                }
                if (refs.empty()) continue; // no sign change anywhere on this cell

                // Eq. 3: centroid of the interesting edges' Hermite points.
                simd_float3 centroid = {0.0f, 0.0f, 0.0f};
                for (int32_t e : refs) centroid += out.hermite_p[e];
                centroid /= float(refs.size());

                out.cell_id.push_back(i + cells_per_axis * (j + cells_per_axis * k));
                out.cell_vertex.push_back(centroid);
                out.cell_edge_indices.insert(out.cell_edge_indices.end(), refs.begin(), refs.end());
                out.cell_edge_offsets.push_back(static_cast<int32_t>(out.cell_edge_indices.size()));
            }
        }
    }

    return out;
}

GlobalMesh build_global_mesh(const DcsddInit& init, const SampleGrid& grid,
                              const std::vector<simd_float3>& cell_vertices) {
    const int32_t n = grid.n;
    const int32_t cells_per_axis = n - 1;
    const int32_t cell_max = n - 2;

    // Build-local lookup: interesting-cell id -> dense index (position in
    // cell_vertices / DcsddInit::cell_id/cell_vertex), never stored in
    // GlobalMesh.
    std::unordered_map<int32_t, int32_t> id_to_dense;
    id_to_dense.reserve(init.cell_id.size());
    for (size_t d = 0; d < init.cell_id.size(); ++d) {
        id_to_dense[init.cell_id[d]] = static_cast<int32_t>(d);
    }

    const auto in_range = [cell_max](int32_t v) { return v >= 0 && v <= cell_max; };

    GlobalMesh out;
    for (size_t ei = 0; ei < init.edge_axis.size(); ++ei) {
        const int32_t axis = init.edge_axis[ei];
        const Coord3 base = unflatten(init.edge_base[ei], n);

        std::array<CellCoord, 4> cells4 = cyclic_quad_cells(axis, base.x, base.y, base.z);
        bool all_in_range = true;
        for (const CellCoord& cc : cells4) {
            if (!in_range(cc.i) || !in_range(cc.j) || !in_range(cc.k)) {
                all_in_range = false;
                break;
            }
        }
        // Grid-boundary edge: fewer than 4 containing cells -> no quad. The
        // scene's +10% sampling margin (D1) makes these rare in practice.
        if (!all_in_range) continue;

        // Reverse when the base sample is outside (s >= 0) so quads wind
        // consistently outward — the sphere acceptance test confirms this
        // direction wins outright (every triangle normal points away from
        // the sphere center); no flip needed.
        const float s_base = grid.values[flat_index(base.x, base.y, base.z, n)];
        if (s_base >= 0.0f) {
            std::reverse(cells4.begin(), cells4.end());
        }

        std::array<int32_t, 4> dense{};
        for (int32_t i = 0; i < 4; ++i) {
            const int32_t id = encode_cell_id(cells4[i], cells_per_axis);
            // Must exist: all_in_range guarantees a valid cell index, and
            // every valid cell touched by an interesting edge is itself
            // interesting (the edge is one of its 12 candidate edges).
            dense[i] = id_to_dense.at(id);
        }

        out.quad_edge.push_back(static_cast<int32_t>(ei));
        for (int32_t d : dense) out.quad_cells.push_back(d);

        // Triangulation by first diagonal: (v0,v1,v2), (v0,v2,v3).
        out.tri_cells.push_back(dense[0]);
        out.tri_cells.push_back(dense[1]);
        out.tri_cells.push_back(dense[2]);
        out.tri_cells.push_back(dense[0]);
        out.tri_cells.push_back(dense[2]);
        out.tri_cells.push_back(dense[3]);

        // Face intersection points (paper §3.2): for each of the quad's 4
        // consecutive cell pairs, the mesh edge between their two cell
        // vertices crosses the axis-aligned plane of their shared grid face.
        for (int32_t i = 0; i < 4; ++i) {
            const CellCoord ca = cells4[i];
            const CellCoord cb = cells4[(i + 1) % 4];
            // Consecutive cells in the cycle are face-adjacent: they differ
            // along exactly one axis.
            int32_t plane_axis = (ca.i != cb.i) ? 0 : ((ca.j != cb.j) ? 1 : 2);
            assert(cell_component(ca, plane_axis) != cell_component(cb, plane_axis));

            const int32_t higher = std::max(cell_component(ca, plane_axis), cell_component(cb, plane_axis));
            const float plane = grid.origin[plane_axis] + grid.spacing * float(higher);

            const simd_float3 va = cell_vertices[dense[i]];
            const simd_float3 vb = cell_vertices[dense[(i + 1) % 4]];
            const float denom = vb[plane_axis] - va[plane_axis];

            // Guards: cell vertices may escape their cells (paper Fig. 15),
            // so the segment may not actually cross the plane — clamp t to
            // [0,1]; if the segment runs (near-)parallel to the plane
            // (denom ~ 0), there's no meaningful crossing, so fall back to
            // the midpoint.
            float t;
            if (std::fabs(denom) < 1e-9f) {
                t = 0.5f;
            } else {
                t = (plane - va[plane_axis]) / denom;
                t = std::clamp(t, 0.0f, 1.0f);
            }
            out.face_points.push_back(va + t * (vb - va));
        }
    }
    return out;
}

SampleAssignment assign_samples(const SampleGrid& grid, const DcsddInit& init,
                                 const std::vector<simd_float3>& cell_vertices, const GlobalMesh& mesh) {
    const int32_t n = grid.n;
    const int32_t cells_per_axis = n - 1;
    const float cell_diag = grid.spacing * std::sqrt(3.0f);

    std::unordered_map<int32_t, int32_t> id_to_dense;
    id_to_dense.reserve(init.cell_id.size());
    for (size_t d = 0; d < init.cell_id.size(); ++d) {
        id_to_dense[init.cell_id[d]] = static_cast<int32_t>(d);
    }

    const TriangleBins bins = build_triangle_bins(grid, mesh, cell_vertices);
    const auto clamp_idx = [cells_per_axis](int32_t v) { return std::clamp(v, 0, cells_per_axis - 1); };
    const auto cell_of = [&](simd_float3 p) {
        return std::array<int32_t, 3>{
            clamp_idx(static_cast<int32_t>(std::floor((p.x - grid.origin.x) / grid.spacing))),
            clamp_idx(static_cast<int32_t>(std::floor((p.y - grid.origin.y) / grid.spacing))),
            clamp_idx(static_cast<int32_t>(std::floor((p.z - grid.origin.z) / grid.spacing))),
        };
    };
    const auto cell_id_of = [cells_per_axis](int32_t i, int32_t j, int32_t k) {
        const size_t cpa = static_cast<size_t>(cells_per_axis);
        return static_cast<size_t>(i) + cpa * (static_cast<size_t>(j) + cpa * static_cast<size_t>(k));
    };

    std::vector<std::vector<int32_t>> buckets(init.cell_id.size());

    for (int32_t z = 0; z < n; ++z) {
        for (int32_t y = 0; y < n; ++y) {
            for (int32_t x = 0; x < n; ++x) {
                const int32_t flat = static_cast<int32_t>(flat_index(x, y, z, n));
                const float s = grid.values[flat];

                // Narrow band (deviation, binding): only samples with
                // |s| < 2*cell_diagonal participate.
                if (std::fabs(s) >= 2.0f * cell_diag) continue;

                const simd_float3 p = sample_pos(grid, x, y, z);
                const std::array<int32_t, 3> own = cell_of(p);
                const float outlier_bound = cell_diag + std::fabs(s);

                float best_dist = FLT_MAX;
                simd_float3 best_pt{0.0f, 0.0f, 0.0f};

                // Expanding Chebyshev-ring search from the sample's own
                // cell, tracking the best hit. Two independent stopping
                // conditions, both sufficient on their own (checked before
                // examining ring `ring_r`, ring_r > 0):
                //  - (ring_r-1)*spacing > best_dist: every point in ring
                //    ring_r (and any farther ring) is at least
                //    (ring_r-1)*spacing away from ANY point in the sample's
                //    own cell (Chebyshev distance ring_r cells away, each
                //    cell `spacing` wide) — so no triangle out there can
                //    beat the best hit already found.
                //  - ring_r*spacing > outlier_bound + spacing: even the
                //    nearest possible point in ring_r (at least
                //    (ring_r-1)*spacing away, i.e. > outlier_bound once
                //    ring_r*spacing exceeds outlier_bound+spacing) can't
                //    produce a distance within the outlier bound, so
                //    searching farther can't rescue this sample from being
                //    dropped as an outlier either way.
                // Hard cap at max_ring (the farthest any cell in the grid
                // can be from `own`) guarantees termination even for an
                // empty mesh (best_dist never improves, neither bound ever
                // trips because best_dist stays FLT_MAX — the cap is what
                // stops the loop in that case).
                const int32_t max_ring = std::max({own[0], cells_per_axis - 1 - own[0], own[1],
                                                     cells_per_axis - 1 - own[1], own[2],
                                                     cells_per_axis - 1 - own[2]});
                for (int32_t ring_r = 0; ring_r <= max_ring; ++ring_r) {
                    if (ring_r > 0) {
                        if (float(ring_r - 1) * grid.spacing > best_dist) break;
                        if (float(ring_r) * grid.spacing > outlier_bound + grid.spacing) break;
                    }

                    const int32_t lo_i = clamp_idx(own[0] - ring_r), hi_i = clamp_idx(own[0] + ring_r);
                    const int32_t lo_j = clamp_idx(own[1] - ring_r), hi_j = clamp_idx(own[1] + ring_r);
                    const int32_t lo_k = clamp_idx(own[2] - ring_r), hi_k = clamp_idx(own[2] + ring_r);
                    for (int32_t kk = lo_k; kk <= hi_k; ++kk) {
                        for (int32_t jj = lo_j; jj <= hi_j; ++jj) {
                            for (int32_t ii = lo_i; ii <= hi_i; ++ii) {
                                const int32_t cheby = std::max(
                                    {std::abs(ii - own[0]), std::abs(jj - own[1]), std::abs(kk - own[2])});
                                if (cheby != ring_r) continue; // only this ring's shell, not the whole cube
                                const size_t cid = cell_id_of(ii, jj, kk);
                                for (int32_t bi = bins.offsets[cid]; bi < bins.offsets[cid + 1]; ++bi) {
                                    const int32_t t = bins.indices[bi];
                                    const simd_float3 va = cell_vertices[mesh.tri_cells[3 * t + 0]];
                                    const simd_float3 vb = cell_vertices[mesh.tri_cells[3 * t + 1]];
                                    const simd_float3 vc = cell_vertices[mesh.tri_cells[3 * t + 2]];
                                    const simd_float3 cp = closest_point_on_triangle(p, va, vb, vc);
                                    const float d = simd_length(p - cp);
                                    if (d < best_dist) {
                                        best_dist = d;
                                        best_pt = cp;
                                    }
                                }
                            }
                        }
                    }
                }

                if (best_dist == FLT_MAX) continue; // empty mesh: nothing to assign to

                // Outlier rejection (paper): too far from the current
                // surface estimate to be trustworthy.
                if (best_dist > outlier_bound) continue;

                const std::array<int32_t, 3> ci = cell_of(best_pt);
                const int32_t cid = ci[0] + cells_per_axis * (ci[1] + cells_per_axis * ci[2]);
                const auto it = id_to_dense.find(cid);
                if (it == id_to_dense.end()) continue; // not an interesting cell: no vertex to optimize against
                buckets[it->second].push_back(flat);
            }
        }
    }

    SampleAssignment out;
    out.cell_sample_offsets.reserve(buckets.size() + 1);
    out.cell_sample_offsets.push_back(0);
    for (const std::vector<int32_t>& b : buckets) {
        out.cell_sample_indices.insert(out.cell_sample_indices.end(), b.begin(), b.end());
        out.cell_sample_offsets.push_back(static_cast<int32_t>(out.cell_sample_indices.size()));
    }
    return out;
}

// --- D4: per-cell local optimization (inner loop) --------------------------

SolveRow build_sample_row(float alpha, float beta, float gamma, simd_float3 d, simd_float3 q, simd_float3 h,
                           simd_float3 p, float eps) {
    // alpha==0 rule (supplementary §C, verbatim): the closest point lies
    // entirely on the triangle's fixed p-h edge, independent of x -- the
    // naive row would carry no information about x (coeff would be zero),
    // "locking" x in place along d. Substituting alpha=1,beta=0,gamma=0
    // turns the row into d^T x = q.d, pulling x directly towards/away from
    // the sample's sphere instead.
    if (std::fabs(alpha) < eps) {
        alpha = 1.0f;
        beta = 0.0f;
        gamma = 0.0f;
    }
    const simd_float3 coeff = alpha * d;
    const float rhs = simd_dot(q, d) - beta * simd_dot(h, d) - gamma * simd_dot(p, d);
    return SolveRow{coeff, rhs};
}

SolveRow build_hermite_row(simd_float3 n, simd_float3 h, float w_hermite) {
    return SolveRow{w_hermite * n, w_hermite * simd_dot(n, h)};
}

simd_float3 solve_weighted_normal_equations(const std::vector<SolveRow>& rows, float mu, simd_float3 x_prev) {
    // Accumulate QtQ/Qtc directly (no matrix library): QtQ = sum coeff*coeffT,
    // Qtc = sum coeff*rhs.
    simd_float3x3 QtQ = {simd_float3{0, 0, 0}, simd_float3{0, 0, 0}, simd_float3{0, 0, 0}};
    simd_float3 Qtc = {0.0f, 0.0f, 0.0f};
    for (const SolveRow& row : rows) {
        const simd_float3& c = row.coeff;
        QtQ.columns[0] += c.x * c;
        QtQ.columns[1] += c.y * c;
        QtQ.columns[2] += c.z * c;
        Qtc += c * row.rhs;
    }
    // Regularizer (Eq. 11): mu*I toward x_prev. mu > 0 makes QtQ + mu*I
    // strictly positive definite regardless of how degenerate `rows` is
    // (QtQ alone is only PSD -- e.g. zero rows, or all rows parallel, would
    // leave it singular), so this 3x3 is always invertible.
    QtQ.columns[0].x += mu;
    QtQ.columns[1].y += mu;
    QtQ.columns[2].z += mu;
    Qtc += mu * x_prev;

    return simd_mul(simd_inverse(QtQ), Qtc);
}

namespace {

// One inner-loop iteration (paper §3.3 + supplementary §C): builds every
// sample row (searching the cell's fan triangles for each assigned sample's
// closest point, via closest_point_on_triangle_barycentric) and every
// Hermite row for `dense_cell`, then solves. `x` is the current iterate
// (both the moving fan vertex and the regularizer's target). Factored out of
// optimize_cell_vertex so a single iteration is independently callable.
simd_float3 optimize_cell_vertex_step(int32_t dense_cell, simd_float3 x, const DcsddInit& init,
                                       const GlobalMesh& mesh, const CellFans& fans,
                                       const SampleAssignment& assignment, const SampleGrid& grid,
                                       const DcsddConfig& config) {
    std::vector<SolveRow> rows;

    const int32_t fan_begin = fans.cell_fan_offsets[dense_cell];
    const int32_t fan_end = fans.cell_fan_offsets[dense_cell + 1];

    const int32_t sample_begin = assignment.cell_sample_offsets[dense_cell];
    const int32_t sample_end = assignment.cell_sample_offsets[dense_cell + 1];
    for (int32_t si = sample_begin; si < sample_end; ++si) {
        const int32_t flat = assignment.cell_sample_indices[si];
        const Coord3 coord = unflatten(flat, grid.n);
        const simd_float3 u = sample_pos(grid, coord.x, coord.y, coord.z);
        const float s = grid.values[static_cast<size_t>(flat)];

        // Closest point over all of this cell's fan triangles (p, h, x);
        // keep the winning triangle's own (p, h) alongside its barycentric
        // result so the row below doesn't need to re-derive which triangle won.
        bool have_best = false;
        float best_dist = 0.0f;
        TriangleBarycentric best{};
        simd_float3 best_p{}, best_h{};
        for (int32_t fi = fan_begin; fi < fan_end; ++fi) {
            const simd_float3 p = mesh.face_points[static_cast<size_t>(fans.fan_face_point[fi])];
            const simd_float3 h = init.hermite_p[static_cast<size_t>(fans.fan_edge[fi])];
            const TriangleBarycentric r = closest_point_on_triangle_barycentric(u, p, h, x);
            const float dist = simd_length(u - r.point);
            if (!have_best || dist < best_dist) {
                have_best = true;
                best_dist = dist;
                best = r;
                best_p = p;
                best_h = h;
            }
        }
        if (!have_best) continue; // no fan triangles for this cell: nothing to project against

        // t_j = best.point = gamma*p + beta*h + alpha*x (u=gamma, v=beta, w=alpha).
        const simd_float3 t = best.point;
        const simd_float3 diff = t - u;
        const float diff_len = simd_length(diff);
        if (diff_len < 1e-9f) continue; // degenerate: t_j == u_j, no direction to derive -- skip row

        const simd_float3 d = diff / diff_len;
        const simd_float3 q = u + std::fabs(s) * d;

        rows.push_back(build_sample_row(best.w, best.v, best.u, d, q, best_h, best_p));
    }

    const int32_t edge_begin = init.cell_edge_offsets[dense_cell];
    const int32_t edge_end = init.cell_edge_offsets[dense_cell + 1];
    for (int32_t ei = edge_begin; ei < edge_end; ++ei) {
        const int32_t edge = init.cell_edge_indices[ei];
        rows.push_back(build_hermite_row(init.hermite_n[static_cast<size_t>(edge)],
                                          init.hermite_p[static_cast<size_t>(edge)], config.w_hermite));
    }

    return solve_weighted_normal_equations(rows, config.mu, x);
}

} // namespace

CellFans build_cell_fans(const DcsddInit& init, const GlobalMesh& mesh) {
    const size_t num_cells = init.cell_id.size();
    const size_t num_quads = mesh.quad_edge.size();

    // Bucket by dense cell first (build-local only, never stored), then
    // flatten to CSR -- same two-pass pattern as assign_samples.
    std::vector<std::vector<std::pair<int32_t, int32_t>>> buckets(num_cells); // (face_point, edge)
    for (size_t q = 0; q < num_quads; ++q) {
        const int32_t edge = mesh.quad_edge[q];
        for (int32_t i = 0; i < 4; ++i) {
            const int32_t cell = mesh.quad_cells[4 * q + static_cast<size_t>(i)];
            const int32_t fp_forward = static_cast<int32_t>(4 * q) + i;               // mesh edge c_i -> c_{i+1}
            const int32_t fp_backward = static_cast<int32_t>(4 * q) + (i + 3) % 4;     // mesh edge c_{i-1} -> c_i
            buckets[static_cast<size_t>(cell)].emplace_back(fp_forward, edge);
            buckets[static_cast<size_t>(cell)].emplace_back(fp_backward, edge);
        }
    }

    CellFans out;
    out.cell_fan_offsets.reserve(num_cells + 1);
    out.cell_fan_offsets.push_back(0);
    for (const auto& bucket : buckets) {
        for (const auto& [fp, edge] : bucket) {
            out.fan_face_point.push_back(fp);
            out.fan_edge.push_back(edge);
        }
        out.cell_fan_offsets.push_back(static_cast<int32_t>(out.fan_face_point.size()));
    }
    return out;
}

simd_float3 optimize_cell_vertex(int32_t dense_cell, simd_float3 x_start, const DcsddInit& init,
                                  const GlobalMesh& mesh, const CellFans& fans, const SampleAssignment& assignment,
                                  const SampleGrid& grid, const DcsddConfig& config) {
    simd_float3 x = x_start;
    for (int32_t r = 0; r < config.inner_iters; ++r) {
        const simd_float3 x_new = optimize_cell_vertex_step(dense_cell, x, init, mesh, fans, assignment, grid, config);
        const float delta = simd_length(x_new - x);
        x = x_new;
        if (delta < config.inner_tol) break;
    }
    return x;
}

// --- D5: Hermite update (Eq. 7) + outer loop + full pipeline ---------------

namespace {

// Eigenvalues of a real symmetric 3x3 matrix, ascending (smallest, mid,
// largest), via the standard closed-form trigonometric method (see e.g.
// Smith, "Eigenvalues of a symmetric 3x3 matrix", 1961). Guard: if every
// off-diagonal entry is (near-)zero, `m` is already diagonal -- its
// eigenvalues are just the diagonal entries, sorted (the general formula
// below divides by `p`, which would be ~0 in this case).
struct Eigenvalues3 {
    float smallest, mid, largest;
};
Eigenvalues3 eigenvalues_symmetric3x3(simd_float3x3 m) {
    const float m01 = m.columns[1][0], m02 = m.columns[2][0], m12 = m.columns[2][1];
    const float p1 = m01 * m01 + m02 * m02 + m12 * m12;
    if (p1 < 1e-12f) {
        float d[3] = {m.columns[0][0], m.columns[1][1], m.columns[2][2]};
        std::sort(d, d + 3);
        return Eigenvalues3{d[0], d[1], d[2]};
    }
    const float q = (m.columns[0][0] + m.columns[1][1] + m.columns[2][2]) / 3.0f;
    const float d00 = m.columns[0][0] - q, d11 = m.columns[1][1] - q, d22 = m.columns[2][2] - q;
    const float p2 = d00 * d00 + d11 * d11 + d22 * d22 + 2.0f * p1;
    const float p = std::sqrt(p2 / 6.0f);
    // B = (1/p) * (m - q*I); r = det(B)/2, clamped to [-1,1] for acos below
    // (round-off can push it just outside that range).
    const simd_float3x3 B = {simd_float3{d00, m01, m02} / p, simd_float3{m01, d11, m12} / p,
                              simd_float3{m02, m12, d22} / p};
    // simd_float3x3 is column-major (columns[c][r]); det via the standard
    // 3x3 cofactor expansion over columns.
    const simd_float3& c0 = B.columns[0];
    const simd_float3& c1 = B.columns[1];
    const simd_float3& c2 = B.columns[2];
    const float det = simd_dot(c0, simd_cross(c1, c2));
    const float r = std::clamp(det / 2.0f, -1.0f, 1.0f);
    const float phi = std::acos(r) / 3.0f;
    constexpr float kTwoPiOverThree = 2.0943951023931953f; // 2*pi/3
    const float eig_largest = q + 2.0f * p * std::cos(phi);
    const float eig_smallest = q + 2.0f * p * std::cos(phi + kTwoPiOverThree);
    const float eig_mid = 3.0f * q - eig_largest - eig_smallest;
    return Eigenvalues3{eig_smallest, eig_mid, eig_largest};
}

} // namespace

simd_float3 smallest_eigenvector_symmetric3x3(simd_float3x3 m) {
    const Eigenvalues3 eig = eigenvalues_symmetric3x3(m);
    // M = m - lambda_min*I, rows as simd_float3 (m is column-major, so row r
    // is (m.columns[0][r], m.columns[1][r], m.columns[2][r])).
    simd_float3x3 shifted = m;
    shifted.columns[0].x -= eig.smallest;
    shifted.columns[1].y -= eig.smallest;
    shifted.columns[2].z -= eig.smallest;
    const simd_float3 row0 = {shifted.columns[0].x, shifted.columns[1].x, shifted.columns[2].x};
    const simd_float3 row1 = {shifted.columns[0].y, shifted.columns[1].y, shifted.columns[2].y};
    const simd_float3 row2 = {shifted.columns[0].z, shifted.columns[1].z, shifted.columns[2].z};

    // Cross product of two rows of the (rank-deficient) shifted matrix spans
    // its null space -- i.e. the eigenvector for lambda_min -- as long as the
    // chosen pair isn't itself (near-)parallel. The brief's prescription is
    // "try row0 x row1, fall back to another pair when degenerate" -- but an
    // absolute-magnitude threshold on the FIRST pair alone isn't reliable: a
    // TDD failure surfaced a case (see the degenerate-PCA test) where
    // lambda_min's float32 residual (true value exactly 0, computed as
    // ~-2.65e-5) leaves row1 not-quite-zero, and row0's much larger
    // magnitude (~5) amplifies that residual so row0 x row1's norm clears
    // any reasonable fixed epsilon while still pointing the WRONG direction
    // (it's dominated by noise, not signal). Since all three row-pairs of a
    // rank-deficient matrix are mathematically parallel to the same true
    // null-space vector (assuming a non-repeated smallest eigenvalue), the
    // fix is to pick whichever pair's cross product has the LARGEST norm --
    // the best-conditioned of the three, not just the first one that clears
    // a threshold. This generalizes the brief's "fallback row pair" to all
    // 3 candidates rather than a fixed try-order.
    const simd_float3 c01 = simd_cross(row0, row1);
    const simd_float3 c02 = simd_cross(row0, row2);
    const simd_float3 c12 = simd_cross(row1, row2);
    const float len01 = simd_length_squared(c01);
    const float len02 = simd_length_squared(c02);
    const float len12 = simd_length_squared(c12);
    simd_float3 c = c01;
    float best = len01;
    if (len02 > best) { c = c02; best = len02; }
    if (len12 > best) { c = c12; best = len12; }
    // Exact-zero guard (e.g. 4 bit-for-bit coincident points, or any input
    // whose shifted matrix is the exact zero matrix): all three row-pair
    // cross products are then exactly (0,0,0), and simd_normalize(0,0,0) is
    // an exact 0/0 that silently returns NaN, poisoning every downstream
    // consumer (pca_best_fit_plane's normal, then Hermite state). Deliberately
    // an EXACT equality check, not a small-magnitude epsilon: a tiny-but-
    // nonzero `best` (e.g. ~1e-13) still normalizes to a perfectly finite --
    // if noisy -- unit vector (no float32 underflow risk at that scale), and
    // is exactly the "pick the best-conditioned of the three" case the
    // comment above already handles; a review pass that first tried a loose
    // epsilon here (~1e-12) measurably hurt the Fig. 16 box acceptance test
    // by overriding those still-valid-but-small normals with the fallback
    // below, so only the true zero needs one. Return the zero vector as a
    // documented sentinel: it threads through cleanly with no extra check
    // needed anywhere else -- a zero plane_normal makes intersect_plane_edge's
    // own denom~0 guard trip (dot(0, axis_dir) == 0 exactly), so
    // update_edge_hermite already keeps the caller's old Hermite data via its
    // existing near-parallel path.
    if (best == 0.0f) {
        return simd_float3{0.0f, 0.0f, 0.0f};
    }
    return simd_normalize(c);
}

PcaPlane pca_best_fit_plane(const std::array<simd_float3, 4>& points) {
    simd_float3 centroid = {0.0f, 0.0f, 0.0f};
    for (const simd_float3& p : points) centroid += p;
    centroid /= 4.0f;

    simd_float3x3 cov = {simd_float3{0, 0, 0}, simd_float3{0, 0, 0}, simd_float3{0, 0, 0}};
    for (const simd_float3& p : points) {
        const simd_float3 d = p - centroid;
        cov.columns[0] += d.x * d;
        cov.columns[1] += d.y * d;
        cov.columns[2] += d.z * d;
    }
    return PcaPlane{centroid, smallest_eigenvector_symmetric3x3(cov)};
}

simd_float3 disambiguate_normal_sign(simd_float3 n, simd_float3 n_old) {
    return (simd_dot(n, n_old) < 0.0f) ? -n : n;
}

EdgeIntersection intersect_plane_edge(simd_float3 plane_point, simd_float3 plane_normal, simd_float3 edge_base,
                                       simd_float3 axis_dir, float spacing) {
    const float denom = simd_dot(plane_normal, axis_dir);
    if (std::fabs(denom) < 1e-9f) {
        return EdgeIntersection{false, simd_float3{0.0f, 0.0f, 0.0f}};
    }
    const float t = simd_dot(plane_normal, plane_point - edge_base) / denom;
    // Robustness deviation from the paper's letter: clamp t to [0, spacing]
    // so y stays on the physical edge segment. Cell vertices may wander far
    // outside their cells (paper Fig. 15), so the best-fit plane's true
    // intersection with the edge's infinite line can fall well outside the
    // segment; clamping keeps the Hermite point physically meaningful.
    const float t_clamped = std::clamp(t, 0.0f, spacing);
    return EdgeIntersection{true, edge_base + t_clamped * axis_dir};
}

HermiteUpdate hermite_eq7_blend(simd_float3 h_old, simd_float3 n_old, simd_float3 y, simd_float3 n_new, float w_u) {
    // Eq. 7, page 6, transcribed exactly (see task-D5-report.md):
    //   h^{k+1} = h^k + w_u*(y - h^k)
    //   n^{k+1} = normalize(n_new + w_u*n_old)   -- NOT (1-w_u)*n_old + w_u*n_new;
    //   the fresh PCA normal n_new carries weight 1, w_u weights n_old.
    const simd_float3 h = h_old + w_u * (y - h_old);
    const simd_float3 n = simd_normalize(n_new + w_u * n_old);
    return HermiteUpdate{h, n};
}

HermiteUpdate update_edge_hermite(const std::array<simd_float3, 4>& cell_vertices, simd_float3 h_old,
                                   simd_float3 n_old, simd_float3 edge_base, simd_float3 axis_dir, float spacing,
                                   float w_u) {
    const PcaPlane plane = pca_best_fit_plane(cell_vertices);
    const simd_float3 n_new = disambiguate_normal_sign(plane.normal, n_old);
    const EdgeIntersection isect = intersect_plane_edge(plane.centroid, n_new, edge_base, axis_dir, spacing);
    if (!isect.valid) {
        // Near-parallel plane/edge: no reliable intersection -- keep the old
        // Hermite data untouched rather than blend towards a meaningless y.
        return HermiteUpdate{h_old, n_old};
    }
    return hermite_eq7_blend(h_old, n_old, isect.y, n_new, w_u);
}

namespace {

// Runs fn(i) for every i in [0, n), split into std::thread::hardware_
// concurrency() contiguous ranges, one std::thread per range (joined before
// returning). Deterministic by construction: callers must ensure each index
// writes only its own slot of any output (see reconstruct's per-cell
// optimization phase below) -- no reductions, no shared mutable state across
// indices, so which thread executes which index never affects the result.
// File-local: an implementation detail of reconstruct's parallel phase, not
// part of the public API.
template <typename Fn>
void parallel_for(int32_t n, Fn&& fn) {
    if (n <= 0) return;
    const unsigned hw = std::max(1u, std::thread::hardware_concurrency());
    const int32_t num_threads = std::min(static_cast<int32_t>(hw), n);
    const int32_t chunk = (n + num_threads - 1) / num_threads;

    std::vector<std::thread> threads;
    threads.reserve(static_cast<size_t>(num_threads));
    for (int32_t t = 0; t < num_threads; ++t) {
        const int32_t begin = t * chunk;
        const int32_t end = std::min(n, begin + chunk);
        if (begin >= end) continue;
        threads.emplace_back([begin, end, &fn]() {
            for (int32_t i = begin; i < end; ++i) fn(i);
        });
    }
    for (std::thread& th : threads) th.join();
}

} // namespace

TriangleMesh reconstruct(const SampleGrid& grid, const DcsddConfig& config) {
    TriangleMesh out;

    // Mutable copy of the init state: its hermite_p/hermite_n evolve across
    // outer iterations (the Hermite update below writes back into `state`,
    // not a separate array) so that the NEXT iteration's optimize_cell_vertex
    // calls -- which read Hermite data via `state` (unchanged D2-D4
    // signatures) -- see the updated values. No changes to D2-D4 internals:
    // this is orchestration only.
    DcsddInit state = dcsdd_init(grid);
    if (state.cell_id.empty()) {
        return out; // no interesting cells -> empty mesh
    }

    std::vector<simd_float3> vertices = state.cell_vertex; // Eq. 3 centroid initial values

    // CellFans' topology (which face points / edges flank each cell) depends
    // only on GlobalMesh's quad_edge/quad_cells, which depend only on grid
    // values + state's edge/cell topology -- NOT on the vertices' actual
    // positions (those only feed face_points, recomputed fresh every
    // iteration below). So it's genuinely invariant across outer iterations;
    // built once here rather than every iteration (the brief permits either
    // choice when the code separates cleanly -- it does, since
    // build_global_mesh computes quad_edge/quad_cells/tri_cells from
    // `state`/`grid` alone, only touching `cell_vertices` for face_points).
    // Also seeds `mesh` below (holding the centroid-vertex quads) so the
    // final triangulation still has valid topology to work with even if
    // config.outer_iters == 0 (no optimization iterations at all).
    GlobalMesh mesh = build_global_mesh(state, grid, vertices);
    const CellFans fans = build_cell_fans(state, mesh);

    const int32_t num_cells = static_cast<int32_t>(state.cell_id.size());
    for (int32_t outer = 0; outer < config.outer_iters; ++outer) {
        // Rebuilt every iteration for fresh face_points (the vertices moved
        // last iteration); topology (quad_edge/quad_cells/tri_cells) comes
        // out identical to the invariant one above every time -- recomputing
        // it is cheap relative to the per-cell optimization below, and one
        // code path is simpler than threading a "topology-only" variant
        // through assign_samples too.
        mesh = build_global_mesh(state, grid, vertices);
        const SampleAssignment assignment = assign_samples(grid, state, vertices, mesh);

        // Parallel phase: every interesting cell's NEW vertex goes into its
        // own slot of a FRESH array, never overwriting `vertices` in place --
        // optimize_cell_vertex only ever reads `vertices[c]` for its OWN
        // cell c (all cross-cell interaction happens through `mesh`/`fans`/
        // `assignment`, all read-only and already frozen for this
        // iteration), so this is race-free and deterministic regardless of
        // thread scheduling.
        std::vector<simd_float3> new_vertices(vertices.size());
        parallel_for(num_cells, [&](int32_t c) {
            const size_t idx = static_cast<size_t>(c);
            new_vertices[idx] = optimize_cell_vertex(c, vertices[idx], state, mesh, fans, assignment, grid, config);
        });
        vertices.swap(new_vertices);

        // Hermite update (Eq. 7), using the NEW vertices: one quad per
        // interesting edge with all 4 containing cells present (mesh.
        // quad_edge/quad_cells already restrict to exactly these); edges
        // without a quad keep their old h/n, simply never visited here.
        const size_t num_quads = mesh.quad_edge.size();
        for (size_t q = 0; q < num_quads; ++q) {
            const int32_t edge = mesh.quad_edge[q];
            std::array<simd_float3, 4> cell_verts;
            for (size_t i = 0; i < 4; ++i) {
                cell_verts[i] = vertices[static_cast<size_t>(mesh.quad_cells[4 * q + i])];
            }
            const Coord3 base = unflatten(state.edge_base[static_cast<size_t>(edge)], grid.n);
            const simd_float3 edge_base_pos = sample_pos(grid, base.x, base.y, base.z);
            const int32_t axis = state.edge_axis[static_cast<size_t>(edge)];
            const simd_float3 axis_dir = {axis == 0 ? 1.0f : 0.0f, axis == 1 ? 1.0f : 0.0f, axis == 2 ? 1.0f : 0.0f};

            const HermiteUpdate upd = update_edge_hermite(
                cell_verts, state.hermite_p[static_cast<size_t>(edge)], state.hermite_n[static_cast<size_t>(edge)],
                edge_base_pos, axis_dir, grid.spacing, config.w_update);
            state.hermite_p[static_cast<size_t>(edge)] = upd.h;
            state.hermite_n[static_cast<size_t>(edge)] = upd.n;
        }
        // No early exit: the paper runs a fixed outer-iteration count
        // (config.outer_iters), and so do we.
    }

    // Final: triangulate each quad (mesh.quad_cells, 4 dense cells in the
    // brief's binding cyclic order) via D3's first-diagonal rule -- (v0,v1,v2),
    // (v0,v2,v3), matching GlobalMesh::tri_cells exactly (letter of the
    // brief's "D3's first-diagonal rule / existing tri_cells" preserved) --
    // with the final vertices; facet normal = normalize(cross(v1-v0,v2-v0))
    // per triangle, oriented against a robust per-quad reference (see below).
    //
    // Orientation robustness note (found via a standalone diagnostic, see
    // task-D5-report.md): after `config.outer_iters` iterations, a quad's 4
    // corners can end up locally non-planar/twisted -- an expected DC-family
    // effect, not a D2-D4 bug -- because the algorithm constrains a vertex
    // strongly along its Hermite normal directions but only weakly
    // tangentially (echoing D4's own report on weakly-constrained
    // directions), so two DIFFERENT cells sharing a quad can converge close
    // together tangentially, shrinking one half of the fixed first-diagonal
    // split into a thin sliver whose own cross-product-derived normal is
    // noise-dominated (near-zero area) and can point either way. Fixing this
    // by ADAPTIVELY PICKING A DIFFERENT DIAGONAL didn't work (tried first,
    // then measured): the tiny quad-boundary edge causing the sliver is
    // shared by ONE triangle under EITHER diagonal choice, so neither
    // avoids it. What works: compute each quad's own normal via Newell's
    // method (an area-weighted sum over all 4 corners -- robust even for a
    // non-planar/near-degenerate quad, since it's dominated by the LARGE,
    // reliable triangle's contribution, not the noisy sliver's) as a
    // reference direction, and flip either resulting triangle's own facet
    // normal to agree with it if they disagree. This does NOT change
    // GlobalMesh::tri_cells or its use anywhere above (assign_samples'
    // closest-point queries don't care about winding) -- only this final
    // OUTPUT step's reported normals.
    const size_t num_quads = mesh.quad_edge.size();
    out.positions.reserve(num_quads * 6);
    out.normals.reserve(num_quads * 6);
    const auto facet_normal = [](simd_float3 a, simd_float3 b, simd_float3 c) -> simd_float3 {
        const simd_float3 n = simd_cross(b - a, c - a);
        const float len = simd_length(n);
        // Degenerate (zero-area) triangle: normalize would divide by ~0.
        // Fallback normal instead of NaN -- filtering degenerates isn't this
        // task's job.
        return (len > 1e-12f) ? (n / len) : simd_float3{0.0f, 0.0f, 1.0f};
    };
    // Newell's method: robust normal for a (possibly non-planar) quad,
    // weighted by enclosed area over all 4 corners.
    const auto newell_normal = [](simd_float3 a, simd_float3 b, simd_float3 c, simd_float3 d) -> simd_float3 {
        const simd_float3 verts[4] = {a, b, c, d};
        simd_float3 n = {0.0f, 0.0f, 0.0f};
        for (int i = 0; i < 4; ++i) {
            const simd_float3& cur = verts[i];
            const simd_float3& nxt = verts[(i + 1) % 4];
            n.x += (cur.y - nxt.y) * (cur.z + nxt.z);
            n.y += (cur.z - nxt.z) * (cur.x + nxt.x);
            n.z += (cur.x - nxt.x) * (cur.y + nxt.y);
        }
        const float len = simd_length(n);
        return (len > 1e-12f) ? (n / len) : simd_float3{0.0f, 0.0f, 1.0f};
    };
    const auto emit = [&](simd_float3 a, simd_float3 b, simd_float3 c, simd_float3 n) {
        out.positions.push_back(a);
        out.positions.push_back(b);
        out.positions.push_back(c);
        out.normals.push_back(n);
        out.normals.push_back(n);
        out.normals.push_back(n);
    };
    for (size_t q = 0; q < num_quads; ++q) {
        const simd_float3 v0 = vertices[static_cast<size_t>(mesh.quad_cells[4 * q + 0])];
        const simd_float3 v1 = vertices[static_cast<size_t>(mesh.quad_cells[4 * q + 1])];
        const simd_float3 v2 = vertices[static_cast<size_t>(mesh.quad_cells[4 * q + 2])];
        const simd_float3 v3 = vertices[static_cast<size_t>(mesh.quad_cells[4 * q + 3])];

        const simd_float3 quad_ref = newell_normal(v0, v1, v2, v3);
        simd_float3 n0 = facet_normal(v0, v1, v2);
        if (simd_dot(n0, quad_ref) < 0.0f) n0 = -n0;
        simd_float3 n1 = facet_normal(v0, v2, v3);
        if (simd_dot(n1, quad_ref) < 0.0f) n1 = -n1;

        emit(v0, v1, v2, n0);
        emit(v0, v2, v3, n1);
    }
    return out;
}

} // namespace sq
