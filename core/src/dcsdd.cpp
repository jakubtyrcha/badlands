#include "dcsdd.h"

#include <cassert>
#include <cmath>
#include <unordered_map>

namespace sq {

HermitePoint hermite_crossing(float s_a, float s_b, simd_float3 u_a, simd_float3 u_b) {
    const float denom = std::fabs(s_a) + std::fabs(s_b);
    // Guard: both samples land exactly on the surface -> no magnitude
    // information to weight the interpolation by; fall back to the edge
    // midpoint.
    const float t = (denom == 0.0f) ? 0.5f : std::fabs(s_a) / denom;
    return HermitePoint{(1.0f - t) * u_a + t * u_b, t};
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

} // namespace sq
