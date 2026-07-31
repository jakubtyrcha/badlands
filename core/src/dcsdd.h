#pragma once
#include <simd/simd.h>
#include <cstdint>
#include <vector>

#include "sdf.h"

namespace sq {

// DCSDD (Carrera et al., "Dual Contouring of Signed Distance Data",
// SIGGRAPH '26) mesh reconstruction: the initialization stage only (paper
// §3.1) — interesting edges/cells, Hermite data estimation (Eqs. 1-2), and
// centroid vertex init (Eq. 3). Later milestones add the per-cell/global
// optimization loops that refine these initial estimates; not built here.
//
// Grid conventions (binding for all DCSDD milestones):
// - Samples (x,y,z), 0 <= x,y,z < n, world pos = origin + spacing*(x,y,z),
//   flat index x + n*(y + n*z) — SampleGrid's own convention (sdf.h).
// - Cells (i,j,k), 0 <= i,j,k < n-1, spanning samples
//   [i..i+1] x [j..j+1] x [k..k+1].
// - Edges: identified by (axis, x, y, z), axis in {0,1,2} (x/y/z), connecting
//   sample (x,y,z) to the sample one step along axis. Valid iff the base
//   sample's axis-coordinate is < n-1.
// - Sign convention: a sample is inside when its value is < 0 (an exact 0
//   counts as outside); an edge is *interesting* when its two samples'
//   insideness differs.

// Eq. 1: Hermite crossing point of an interesting edge. `s_a`/`s_b` are the
// sample values at the edge's two ends, `u_a`/`u_b` their world positions.
// `t = |s_a| / (|s_a| + |s_b|)`, `p = lerp(u_a, u_b, t)`. Guard: if
// `|s_a| + |s_b| == 0` (both samples land exactly on the surface), there is
// no magnitude information to weight the interpolation by, so `t = 0.5`
// (the edge midpoint).
//
// Exposed standalone (mirrors the sd_box/sd_ellipsoid precedent in sdf.h) so
// tests can pin the guard/endpoint cases directly, independent of whether the
// grid-scanning pipeline below would ever classify such an edge as
// "interesting" (a both-zero edge never is, by the sign convention above).
struct HermitePoint {
    simd_float3 p;
    float t;
};
HermitePoint hermite_crossing(float s_a, float s_b, simd_float3 u_a, simd_float3 u_b);

// Output of the init stage: flat, POD, index-based (GPU-ready). Hash maps
// are used only inside dcsdd_init's implementation, never stored here.
struct DcsddInit {
    // Interesting edges, one entry per edge, in deterministic order
    // (axis-major, then z,y,x ascending over the base sample coordinate).
    std::vector<int32_t> edge_axis;       // 0/1/2
    std::vector<int32_t> edge_base;       // base sample's flat index (SampleGrid convention)
    std::vector<simd_float3> hermite_p;   // Eq. 1
    std::vector<simd_float3> hermite_n;   // Eq. 2, normalized

    // Interesting cells (>= 1 interesting edge), one entry per cell, in
    // deterministic order (z,y,x ascending).
    std::vector<int32_t> cell_id;         // i + (n-1)*(j + (n-1)*k)
    std::vector<simd_float3> cell_vertex; // Eq. 3 centroid

    // CSR: cell_edge_indices[cell_edge_offsets[c] .. cell_edge_offsets[c+1])
    // are indices into the edge_*/hermite_* arrays above, for the cell at
    // dense index c (same order as cell_id/cell_vertex). Size
    // cell_edge_offsets == cell_id.size() + 1.
    std::vector<int32_t> cell_edge_offsets;
    std::vector<int32_t> cell_edge_indices;
};

// Builds the DCSDD init stage from a sampled grid: interesting edges +
// Hermite data (Eqs. 1-2), interesting cells + centroid vertex (Eq. 3), and
// the cell -> edge CSR. `grid.n` must be >= 2.
DcsddInit dcsdd_init(const SampleGrid& grid);

} // namespace sq
