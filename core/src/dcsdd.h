#pragma once
#include <simd/simd.h>
#include <array>
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

// Closest point on triangle (a,b,c) to `p`, plus the barycentric weights
// (u,v,w) of (a,b,c) at that point (u+v+w == 1 always; a vertex hit gives a
// one-hot weight, an edge hit gives the third weight exactly 0). Same
// Voronoi-region derivation as closest_point_on_triangle above, extended to
// also report the weights. D4 uses this to decompose a sample's closest
// point on a cell's fan triangle (p, h, x) into t_j = gamma*p + beta*h +
// alpha*x (a=p -> gamma=u, b=h -> beta=v, c=x -> alpha=w).
struct TriangleBarycentric {
    simd_float3 point;
    float u, v, w;
};
TriangleBarycentric closest_point_on_triangle_barycentric(simd_float3 p, simd_float3 a, simd_float3 b,
                                                            simd_float3 c);

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

// ---------------------------------------------------------------------------
// D4: per-cell local optimization (inner loop, paper §3.3 + supplementary
// §C). One cell at a time; the outer loop/parallelization across cells is
// D5, not built here.

// Inner-loop tuning knobs, extended by D5 with outer-loop/final-mesh fields.
struct DcsddConfig {
    float w_hermite = 0.02f;  // paper w_H (Eq. 5 Hermite-row weight)
    float mu = 0.05f;         // regularizer mu (Eq. 11); paper recommends [1e-2, 1e-1]
    int32_t inner_iters = 30; // per-cell inner-loop iteration cap; paper default 100
    float inner_tol = 1e-5f;  // convergence threshold on ||x_{r+1} - x_r||

    int32_t outer_iters = 30; // outer-loop iteration cap (fixed count, no early exit); paper default 100
    float w_update = 0.5f;    // paper w_u (Eq. 7 Hermite-update weight); paper recommends [0.2, 0.8]
    int32_t resolution = 64;  // grid samples per axis; consumed by editor-side sampling (later task) --
                               // carried here so all DCSDD tunables live in one struct
};

// A cell's local-mesh topology (paper Fig. 8): CSR over dense cells, each
// entry a (face-point index, interesting-edge index) pair. Together with the
// cell's own (moving) vertex x, entry i forms the fan triangle
// (mesh.face_points[fan_face_point[i]], init.hermite_p[fan_edge[i]], x).
// Flat CSR, GPU-ready; the hash map used to group entries by cell is
// build-time only, never stored here.
struct CellFans {
    std::vector<int32_t> cell_fan_offsets; // size == DcsddInit::cell_id.size() + 1
    std::vector<int32_t> fan_face_point;   // index into GlobalMesh::face_points
    std::vector<int32_t> fan_edge;         // index into DcsddInit edge_*/hermite_* arrays
};

// Builds CellFans from the global mesh: for quad q (edge e, cells
// quad_cells[4q..4q+4) cyclic c0..c3), the cell at cycle position i is
// flanked by face points 4q+i (mesh edge c_i -> c_{i+1}) and 4q+(i+3)%4
// (mesh edge c_{i-1} -> c_i); both pair with edge e. So each quad contributes
// 8 (cell, face-point, edge) entries, grouped here by dense cell index.
CellFans build_cell_fans(const DcsddInit& init, const GlobalMesh& mesh);

// One linearized row of the per-cell least-squares system: coeff . x == rhs.
struct SolveRow {
    simd_float3 coeff;
    float rhs;
};

// Builds one sample row (supplementary §C, Eq. 9 linearization):
// alpha . d^T x = q.d - beta*(h.d) - gamma*(p.d), where (alpha,beta,gamma)
// are the fan triangle's barycentric weights of (x,h,p) at the sample's
// closest point (t_j = gamma*p + beta*h + alpha*x). Applies the alpha==0
// rule verbatim from supplementary §C: when alpha is (numerically) zero, the
// closest point lies entirely on the triangle's fixed p-h edge, independent
// of x, so the naive row degenerates to 0 = 0 (no information -- x is free
// to "lock" in place along d). The paper's fix instead sets
// alpha=1,beta=0,gamma=0, turning the row into d^T x = q.d: a direct pull of
// x towards/away from the sample's sphere, avoiding that numerical locking.
// eps is the threshold on |alpha| (paper: "when alpha == 0"; we use < 1e-6;
// checking the absolute value also catches the near-zero-but-negative case a
// barycentric weight can land on due to floating-point error).
SolveRow build_sample_row(float alpha, float beta, float gamma, simd_float3 d, simd_float3 q, simd_float3 h,
                           simd_float3 p, float eps = 1e-6f);

// Builds one Hermite row (Eq. 5): w_hermite * n^T x = w_hermite * (n.h).
SolveRow build_hermite_row(simd_float3 n, simd_float3 h, float w_hermite);

// Accumulates QtQ (3x3 symmetric) and Qtc (3) directly over `rows` (no
// matrix library), adds the regularizer's contribution toward `x_prev`
// (mu*I to QtQ, mu*x_prev to Qtc -- Eq. 11), and solves the resulting 3x3
// normal equations via simd_inverse. mu > 0 makes QtQ + mu*I strictly
// positive definite (mu*I alone is already SPD, and QtQ is PSD), so it is
// always invertible regardless of how degenerate `rows` is (e.g. zero rows,
// or all rows parallel). Exposed standalone as the inner loop's "one solve
// given rows" step, so it's testable without the geometric pipeline.
simd_float3 solve_weighted_normal_equations(const std::vector<SolveRow>& rows, float mu, simd_float3 x_prev);

// Optimizes dense cell `dense_cell`'s vertex, starting from `x_start`, by
// the per-cell inner loop (paper §3.3 + supplementary §C): each iteration,
// for every sample assigned to this cell, finds its closest point on the
// cell's (moving) fan triangles, derives a sample row (with the alpha==0
// rule); for every interesting edge of this cell, a Hermite row; plus a
// regularizer pulling towards the current iterate; solves, and repeats until
// ||x_{r+1} - x_r|| < config.inner_tol or config.inner_iters is reached.
// Fixed throughout: h, n, p, the fan topology, the sample assignment, and
// all other cells' vertices -- only x moves. Vertices may leave their cell;
// not clamped (paper Fig. 15). A cell with no assigned samples still
// optimizes (Hermite + regularizer rows only).
simd_float3 optimize_cell_vertex(int32_t dense_cell, simd_float3 x_start, const DcsddInit& init,
                                  const GlobalMesh& mesh, const CellFans& fans, const SampleAssignment& assignment,
                                  const SampleGrid& grid, const DcsddConfig& config);

// ---------------------------------------------------------------------------
// D5: Hermite-data update (paper §3.2, Eq. 7), the outer loop, and the public
// reconstruct() entry point. After each outer iteration's per-cell vertex
// optimization (D4), every interesting edge's Hermite data (h, n) is updated
// from the 4 new vertices of its containing cells, via PCA best-fit plane +
// Eq. 7 blend. See task-D5-report.md for the exact printed form of Eq. 7 and
// how it maps onto the functions below.

// Analytic smallest eigenvector of a symmetric 3x3 matrix `m` (paper §3.2 PCA
// step, applied to the 4-point covariance in pca_best_fit_plane below):
// eigenvalues via the standard closed-form trigonometric method for
// symmetric 3x3 matrices, eigenvector via the cross product of two rows of
// (m - lambda_min*I) (each row pair spans the same null-space direction, up
// to sign/scale, for a matrix of rank <= 2). Of the 3 candidate row pairs,
// picks whichever cross product has the largest norm -- the best-
// conditioned choice -- rather than a fixed pair with a fallback: a fixed
// absolute-magnitude threshold on just row0 x row1 turned out not to be
// robust (see task-D5-report.md for the concrete failing case a TDD cycle
// surfaced: a tiny float32 residual in one row, amplified by another row's
// much larger magnitude, can clear any reasonable fixed epsilon while still
// pointing the wrong way). Pinned against np.linalg.eigh in tests.
simd_float3 smallest_eigenvector_symmetric3x3(simd_float3x3 m);

// PCA best-fit plane through exactly 4 points (paper §3.2 Hermite update):
// centroid c = mean(points), normal = smallest_eigenvector_symmetric3x3 of
// the covariance C = sum (v-c)(v-c)^T. `normal` is unit length but NOT
// sign-disambiguated (eigenvectors have arbitrary sign) -- see
// disambiguate_normal_sign below.
struct PcaPlane {
    simd_float3 centroid;
    simd_float3 normal;
};
PcaPlane pca_best_fit_plane(const std::array<simd_float3, 4>& points);

// Flips `n` to the hemisphere of `n_old` when they disagree (dot < 0);
// eigenvectors have arbitrary sign, so without this the Hermite normal could
// flip direction between outer iterations for no geometric reason.
simd_float3 disambiguate_normal_sign(simd_float3 n, simd_float3 n_old);

// Intersection of a plane (through `plane_point`, normal `plane_normal`)
// with the grid edge's line e(t) = edge_base + t*axis_dir (world space, unit
// axis_dir, t in [0, spacing]): solves plane_normal.(e(t)-plane_point) = 0
// for t. `valid=false` if |plane_normal.axis_dir| ~ 0 (near-parallel: no
// reliable intersection -- caller should keep the old Hermite data). `t` is
// clamped to [0, spacing] so `y` stays on the physical edge segment: a
// robustness deviation from the paper's letter (cell vertices may wander far
// from their cells -- paper Fig. 15 -- so the best-fit plane's true
// intersection can fall outside the segment) -- flagged here per the brief.
struct EdgeIntersection {
    bool valid;
    simd_float3 y;
};
EdgeIntersection intersect_plane_edge(simd_float3 plane_point, simd_float3 plane_normal, simd_float3 edge_base,
                                       simd_float3 axis_dir, float spacing);

// Eq. 7 (paper page 6), transcribed EXACTLY as printed (see task-D5-report.md
// for the printed form alongside this):
//   h^{k+1} = h^k + w_u*(y - h^k)
//   n^{k+1} = normalize(n_new + w_u*n_old)
// NOT symmetric with the position update: the freshly-computed PCA normal
// `n_new` (already sign-disambiguated against `n_old` -- see
// disambiguate_normal_sign) carries weight 1, while `w_u` weights the OLD
// normal `n_old`.
struct HermiteUpdate {
    simd_float3 h;
    simd_float3 n;
};
HermiteUpdate hermite_eq7_blend(simd_float3 h_old, simd_float3 n_old, simd_float3 y, simd_float3 n_new, float w_u);

// Full per-edge Hermite update (paper §3.2): PCA best-fit plane of the
// edge's 4 containing cells' NEW vertices, sign disambiguation against
// n_old, edge-line intersection, Eq. 7 blend. Guard (near-parallel
// plane/edge, per intersect_plane_edge) leaves h_old/n_old unchanged.
// Callers only invoke this for edges with all 4 containing cells present --
// GlobalMesh::quad_cells/quad_edge already restrict to exactly these (an
// edge without a quad keeps its old Hermite data untouched, per the brief:
// "skip edges with <4 containing cells -- no quad, no update").
HermiteUpdate update_edge_hermite(const std::array<simd_float3, 4>& cell_vertices, simd_float3 h_old,
                                   simd_float3 n_old, simd_float3 edge_base, simd_float3 axis_dir, float spacing,
                                   float w_u);

// ---------------------------------------------------------------------------
// Public entry point: the full DCSDD pipeline (paper Algorithm 1).

// Flat, GPU-ready, non-indexed triangle mesh: 3 positions/normals per
// triangle (no shared-vertex indexing).
struct TriangleMesh {
    std::vector<simd_float3> positions; // 3 per triangle
    std::vector<simd_float3> normals;   // facet normal, repeated 3x per triangle
};

// Runs the full DCSDD reconstruction: dcsdd_init, then config.outer_iters
// outer iterations of (build_global_mesh -> assign_samples -> parallel
// optimize_cell_vertex over every interesting cell -> Hermite update), then
// triangulates the final quads into a flat mesh with per-facet normals.
// Empty grid (no interesting cells) -> empty mesh (both vectors empty). Fixed
// iteration count, no early exit (matches the paper; comment at the call
// site). Deterministic: the parallel per-cell optimization phase writes each
// cell's new vertex to its own slot in a fresh output array (never in-place),
// so repeated calls on the same input produce bit-identical output regardless
// of thread scheduling.
TriangleMesh reconstruct(const SampleGrid& grid, const DcsddConfig& config);

} // namespace sq
