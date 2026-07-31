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

// ---------------------------------------------------------------------------
// D3: per-outer-iteration geometry stage (paper §3.2, Fig. 8) — the global
// quad mesh over the *current* cell vertices, its triangulation, face
// intersection points, and narrow-band sample-to-cell assignment via closest
// point on the global mesh. `cell_vertices` is a separate input (parallel to
// `DcsddInit::cell_id`/`cell_vertex`, same dense-cell order/count) because it
// changes every outer iteration; `DcsddInit::cell_vertex` is only the initial
// (Eq. 3 centroid) value. No optimization loops here (D4/D5).

// Closest point on triangle (a,b,c) to `p`, via the Voronoi-region method
// (Ericson, "Real-Time Collision Detection" §5.1.5): checks each vertex's and
// edge's Voronoi region in turn, falling through to the face interior.
// Exposed standalone (mirrors the hermite_crossing precedent) so tests can
// pin every region directly; also the inner loop of assign_samples' distance
// queries below.
simd_float3 closest_point_on_triangle(simd_float3 p, simd_float3 a, simd_float3 b, simd_float3 c);

// One quad per interesting edge that has all 4 of its containing cells
// present (grid-boundary edges — fewer than 4 containing cells — produce no
// quad; the scene's +10% sampling margin, per D1, makes these rare in
// practice, so boundary-interesting-edges simply don't contribute geometry).
//
// quad_cells[4q..4q+4) are the quad's 4 dense cell indices (indices into
// `cell_vertices`/`DcsddInit::cell_id`), in the brief's binding cyclic order:
// for an edge along axis `a` with base sample (x,y,z), enumerate cyclically
// in the plane of the other two axes (b,c) — (a,b,c) a right-handed
// permutation (x->(y,z), y->(z,x), z->(x,y)) — as
// (b-1,c-1) -> (b,c-1) -> (b,c) -> (b-1,c), where b,c are the base sample's
// coordinates on those axes. Reversed when the base sample is *outside*
// (s >= 0), so quads wind consistently outward (verified by the sphere
// acceptance test: every triangle normal points away from the sphere
// center — no flip needed, this is the winning direction).
//
// tri_cells[3t..3t+3) triangulates each quad (v0,v1,v2,v3) by the first
// diagonal: (v0,v1,v2), (v0,v2,v3) — so tri_cells.size() == 2 * quad count
// (as 3-tuples: num triangles == 2 * num quads).
//
// face_points[4q+i] is the intersection point on quad q's mesh edge between
// consecutive cells quad_cells[4q+i] and quad_cells[4q+(i+1)%4] (binding
// correspondence — D4's local mesh construction relies on it). See paper
// §3.2: the mesh edge between the two cells' vertices crosses the
// axis-aligned plane of their shared grid face; `t` is solved for that
// crossing and clamped/guarded (see build_global_mesh's implementation
// comments) since cell vertices may wander outside their cells (paper
// Fig. 15) and the segment may not actually cross the plane, or may run
// (near-)parallel to it.
struct GlobalMesh {
    std::vector<int32_t> quad_edge;       // per quad: interesting-edge index
    std::vector<int32_t> quad_cells;      // 4 per quad, dense cell indices, cyclic
    std::vector<int32_t> tri_cells;       // 3 per triangle (dense cell indices)
    std::vector<simd_float3> face_points; // 4 per quad; see correspondence above
};
GlobalMesh build_global_mesh(const DcsddInit& init, const SampleGrid& grid,
                              const std::vector<simd_float3>& cell_vertices);

// Narrow-band sample -> interesting-cell assignment, CSR per dense cell
// (`DcsddInit::cell_id`/`cell_vertex` order): cell_sample_indices[
// cell_sample_offsets[c] .. cell_sample_offsets[c+1]) are flat sample indices
// (SampleGrid convention) assigned to dense cell c.
struct SampleAssignment {
    std::vector<int32_t> cell_sample_offsets; // size == init.cell_id.size() + 1
    std::vector<int32_t> cell_sample_indices; // flat sample indices into grid.values
};

// For every sample with |s| < 2*cell_diagonal (cell_diagonal =
// grid.spacing*sqrt(3); a deviation from the paper — our narrow-band gate),
// finds the closest point on `mesh`'s triangulated surface (accelerated by a
// build-time uniform-grid triangle binning + expanding Chebyshev-ring search
// from the sample's own cell — see assign_samples' implementation comments
// for the termination argument), then:
// - drops the sample if that distance exceeds cell_diagonal + |s| (paper's
//   outlier rejection — the sample is too far from the current surface
//   estimate to be trustworthy);
// - otherwise locates the grid cell containing the closest point
//   (floor((p-origin)/spacing), clamped to the valid cell range) and, if that
//   cell is an interesting cell, assigns the sample to it; otherwise drops it
//   (no vertex there to optimize against).
// No config parameters: every threshold above derives from grid.spacing.
SampleAssignment assign_samples(const SampleGrid& grid, const DcsddInit& init,
                                 const std::vector<simd_float3>& cell_vertices, const GlobalMesh& mesh);

} // namespace sq
