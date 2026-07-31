#pragma once

// Shared tuning constants for the volumetric-foliage feature's voxel-crown
// LODs -- previously triplicated (byte-for-byte, "keep in sync" comments and
// all) across src/executables/viewer/model_viewer_view.cpp,
// game/tests/leaf_voxelizer_tests.cpp, and game/tests/tree_field_gpu_tests.cpp.
// A single source of truth here means a future retune only needs one edit;
// the tests that depend on these values inherently track it instead of
// silently drifting from whatever the viewer actually ships.
#include <array>

namespace badlands {

// Preview height the tree generators are display-scaled to (their native
// ez-tree units are tens-of-meters tall, which frames far away and reads
// tiny) -- model_viewer_view.cpp's `s` (native -> preview) rescale factor is
// derived from this for both the single-tree and Multi-mode (instanced
// field) paths.
inline constexpr float kFoliagePreviewHeight = 8.0f;

// Voxel-crown LOD (volumetric-foliage Phase 3): three progressively coarser
// tet-voxelization cell sizes, one per LOD (index 0 = finest), given in
// WORLD (preview, i.e. kFoliagePreviewHeight-rescaled) space -- callers
// convert to a tree's own native units by dividing by its own `s` (the same
// kFoliagePreviewHeight rescale its bark/leaves already go through) before
// passing it to LeafVoxelizeOptions::cell_size.
//
// Phase 6 MUST-FIX retune: L1 was 0.30 (the naive 0.15/0.30/0.60 doubling
// progression), but at that exact preview-rescaled cell size, Pine
// (medium)/(large)'s needle-sprig cards hit an aliasing dead zone in
// SplatLeafCards' per-quad lattice sampling (nu=ceil(w_len/(cell_size/3))
// lands on an even sample count for most of the crown's cards, so no
// lattice sample -- all offset away from u=0 -- ever lands inside the
// PineSprig stem/needle band; area_alpha is then exactly 0 for every cell,
// not merely below occupancy_fraction*cell^2, so no occupancy_fraction
// value can recover it). Swept world_l1 in
// game/tests/leaf_voxelizer_tests.cpp: Pine (medium)/(large) (and, briefly,
// Bush 3) go empty for every value in [0.23, 0.30]; [0.16, 0.22] is clear of
// the dead zone for all 15 TreeCatalog presets. 0.20 sits mid-band for
// margin on both sides and keeps monotonic tet counts (L0 >= L1 >= L2, all
// non-empty, no preset balloons past the sane-band cap) -- see
// leaf_voxelizer_tests.cpp's "every TreeCatalog preset stays in a sane
// tet-count band" test.
inline constexpr std::array<float, 3> kFoliageVoxelWorldSizes = {0.15f, 0.20f,
                                                                  0.60f};

}  // namespace badlands
