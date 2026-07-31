#pragma once
#include <cstdint>
#include <unordered_map>
#include <glm/glm.hpp>
#include "engine/rendering/components/mesh_components.hpp"
#include "engine/rendering/geometry/textured_mesh_builders.hpp"
#include "game/geometry/tree_options.hpp"  // LeafSilhouette

namespace badlands {

// Tuning for the CPU leaf-card voxelizer (volumetric-foliage Phase 1): turns
// a GenerateLeafMesh alpha-cut quad soup into a sparse grid of occupied
// cells (SplatLeafCards) and then a triangle mesh of one jittered tet per
// occupied cell (EmitTetMesh). Re-running Splat+Emit at 2x/4x cell_size is
// the LOD path -- there is no separate downsample step.
struct LeafVoxelizeOptions {
  float cell_size = 0.15f;           // native tree units
  float occupancy_fraction = 0.15f;  // occupied iff splatted card area >= fraction * cell^2
  float overscale = 1.4f;            // tet circumradius = 0.5 * overscale * cell
  float axis_jitter = 0.18f;         // ~10 deg axis perturbation
  float brightness_jitter = 0.25f;   // uv.x *= (1 - jitter * hash01)
  int coverage_texture_size = 128;   // BuildLeafRgba8 resolution for CPU sampling
};

// Splat output -- the seam a future GPU port (compute-shader occupancy pass
// writing the same cell grid) would target instead of SplatLeafCards.
struct LeafVoxelGrid {
  glm::vec3 origin{0.0f};  // floor(leaf-mesh AABB min / cell_size) * cell_size
  float cell_size = 0.0f;
  glm::ivec3 dims{0};      // grid extent in cells; SplatLeafCards fails loudly past 512/axis

  // Per-cell splatted-card accumulation (a Riemann sum over the card's own
  // sub-cell lattice, weighted by physical sample area -- see
  // SplatLeafCards). NOT pre-pruned by occupancy_fraction: EmitTetMesh
  // applies that test per cell, so a cell can appear here with area_alpha
  // below the occupied threshold. Keyed by a packed (x,y,z) cell coordinate
  // (see PackCellKey in leaf_voxelizer.cpp); iteration order is NOT
  // deterministic (unordered_map) -- EmitTetMesh sorts keys before emitting.
  struct Accum {
    float area_alpha = 0.0f;  // sum(alpha * dA) -- occupancy signal
    float area_gray = 0.0f;   // sum(alpha * gray * dA) -- brightness numerator
    glm::vec3 area_axis{0.0f};  // sum(alpha * dA * growth_axis) -- tet orientation numerator
  };
  std::unordered_map<uint64_t, Accum> cells;
};

// LeafVoxelGrid::cells' key: a grid cell coordinate packed 21 bits/axis (x
// in bits [0,20], y in [21,41], z in [42,62]) -- collision-free for any
// dims component <= 512, i.e. SplatLeafCards' own fail-loudly limit.
uint64_t PackCellKey(glm::ivec3 cell);
glm::ivec3 UnpackCellKey(uint64_t key);

// Splats every leaf-card quad of `leaf_mesh` (GenerateLeafMesh's output --
// every 6 indices is one disjoint 4-vert quad, see tree_generator.cpp) into
// a uniform grid of `opts.cell_size`-sided cells, sampling `silhouette`'s
// coverage texture (opts.coverage_texture_size, built once) per lattice
// sample. Deterministic: no RNG, no order-dependent floating-point
// reduction beyond per-cell sums (see EmitTetMesh for how those get a
// stable emit order). Fails loudly (spdlog::error + an empty-cells grid) if
// the mesh's AABB would need a grid dimension > 512 cells on any axis;
// an empty (no-index) mesh likewise returns an empty grid, no error logged.
LeafVoxelGrid SplatLeafCards(const StaticTexturedMeshComponent& leaf_mesh,
                             LeafSilhouette silhouette, const LeafVoxelizeOptions& opts);

// Emits one jittered regular tetrahedron (4 verts, 4 tris, shared per-tet
// unit normal) per cell whose area_alpha clears opts.occupancy_fraction *
// cell_size^2, iterating grid.cells in ascending packed-key order so output
// is byte-identical run-to-run despite the unordered_map storage. Vertex
// uv.x carries the cell's jittered brightness (uv.y = 0); local_bounds is
// computed from the emitted tet vertices (never the source card AABB).
TexturedMeshResult EmitTetMesh(const LeafVoxelGrid& grid, const LeafVoxelizeOptions& opts);

// SplatLeafCards + EmitTetMesh.
TexturedMeshResult VoxelizeLeafCards(const StaticTexturedMeshComponent& leaf_mesh,
                                     LeafSilhouette silhouette, const LeafVoxelizeOptions& opts);

}  // namespace badlands
