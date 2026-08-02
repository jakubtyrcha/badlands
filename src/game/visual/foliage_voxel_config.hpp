#pragma once

// Shared tuning constants for the volumetric-foliage feature's voxel-crown
// LODs -- previously triplicated (byte-for-byte, "keep in sync" comments and
// all) across src/executables/viewer/model_viewer_view.cpp,
// game/tests/leaf_voxelizer_tests.cpp, and game/tests/tree_field_gpu_tests.cpp.
// A single source of truth here means a future retune only needs one edit;
// the tests that depend on these values inherently track it instead of
// silently drifting from whatever the viewer actually ships.
#include <algorithm>
#include <array>
#include <cstddef>

#include "engine/rendering/components/mesh_components.hpp"
#include "engine/rendering/geometry/textured_mesh_builders.hpp"  // kTexturedMeshFloatsPerVertex
#include "game/geometry/mesh_lod.hpp"

namespace badlands {

// Preview height the tree generators are display-scaled to (their native
// ez-tree units are tens-of-meters tall, which frames far away and reads
// tiny) -- model_viewer_view.cpp's `s` (native -> preview) rescale factor is
// derived from this for both the single-tree and Multi-mode (instanced
// field) paths.
inline constexpr float kFoliagePreviewHeight = 8.0f;

// Voxel-crown LOD (volumetric-foliage Phase 3): four progressively coarser
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
// L3 (the coarsest) continues the ~3x-per-step progression L1->L2 started,
// and is far enough out that the crown is a loose cluster of oversized tets --
// a blob standing in for the silhouette, not a shape. Measured tet output
// across all 15 TreeCatalog presets: 56-428 triangles (see
// leaf_voxelizer_tests.cpp's coarsest-level band test, which pins it).
//
// The chain deliberately stops here. A 5.0m L4 was tried and dropped: at that
// cell size the tets' overscale (circumradius = 0.5 * overscale * cell) makes
// the crown VOLUME visibly larger than the tree it stands for, so an L4 tree
// reads as bigger than the L3 one it replaces -- worse than the level it was
// meant to cheapen, whatever it saved in triangles. Re-adding a level means
// adding its cell size here AND its bark rule (see the static_assert below).
//
// This whole chain is the foliage default: both the single-tree preview levels
// and the instanced-field (Multi) path build every entry, since a model's LOD
// count is runtime (GpuInstanceRenderer::ModelLod) rather than the engine's
// kMaxLods cap.
inline constexpr std::array<float, 4> kFoliageVoxelWorldSizes = {
    0.15f, 0.20f, 0.60f, 1.5f};

// Bark triangle budgets for the coarse tail of the voxel-crown chain --
// indexed by `voxel_level - kDefaultLodRatios.size()`, so entry 0 is L3.
// ABSOLUTE counts, not the kDefaultLodRatios-style ratios the finer levels
// use, for two reasons: source bark meshes span 2.5k-13.7k triangles across
// the catalog, so one ratio spreads the result over a 5x band (a budget
// instead lands every preset in the same place); and the target here is a
// fixed cost, which is what a budget states directly. 256 at L3 measures out
// at 158-255 triangles of actual bark across the catalog.
inline constexpr std::array<int, 1> kFoliageCoarseBarkTriBudgets = {256};

// Distance cutoffs (WORLD METRES) between adjacent voxel-crown LODs, for a tree
// displayed at kFoliagePreviewHeight. One entry per adjacent pair, so
// kFoliageVoxelWorldSizes.size() - 1 of them.
//
// Derived from the same screen-space budget kFoliageVoxelWorldSizes was:
// distance = world_cell_size * focal_px / kFoliageVoxelTargetPx, with focal_px
// = (1080/2) / tan(30deg) ~= 935 at 1920x1080 / 60deg vertical fov. L0->L1
// (0.15 m) ~= 17.5, rounded to 18; L1->L2 (0.20 m) ~= 23.4, rounded to 23;
// L2->L3 (0.60 m) ~= 70.1. The first two were screenshot-tuned alongside the
// Phase 6 empty-crown fix; the last is the formula's value as-is.
//
// Moved here from model_viewer_view.cpp so the instanced-field path can scale
// them per model -- see FoliageLodThresholdsForHeight.
inline constexpr std::array<float, kFoliageVoxelWorldSizes.size() - 1>
    kFoliageLodThresholdsPreviewM = {18.0f, 23.0f, 70.0f};

// The LOD chain retargeted from the 8 m preview tree to a tree that actually
// stands `target_height_m` tall.
//
// Both halves scale by the SAME height ratio, and they have to: LOD selection
// is a screen-space budget, not a world-space one. Scaling the voxel cell size
// with the tree keeps the crown's RELATIVE resolution fixed (so a 24 m oak does
// not silently cost ~27x the tets of an 8 m preview tree at L0), and scaling the
// switch distance with it keeps the on-screen tet size at the same few pixels --
// a bigger tree is legitimately visible from further away.
//
// FoliageVoxelCellNativeM is stated in the tree's OWN native ez-tree units
// (what LeafVoxelizeOptions::cell_size wants) and is deliberately algebraically
// identical to what the model viewer already passes for its preview tree
// (kFoliageVoxelWorldSizes[lod] / (kFoliagePreviewHeight / bark_height_native)),
// so the Phase 6 pine dead-zone retune and leaf_voxelizer_tests' sane-band
// coverage keep holding unchanged.
inline float FoliageVoxelCellNativeM(size_t lod, float bark_height_native) {
  return kFoliageVoxelWorldSizes[lod] * bark_height_native /
         kFoliagePreviewHeight;
}

// The per-LOD tet POSITION jitter that holds the tets' ABSOLUTE displacement
// constant across the chain. `base_jitter` is LeafVoxelizeOptions' own default
// (passed in rather than included, to keep this header off the voxelizer).
//
// LeafVoxelizeOptions::position_jitter is a FRACTION OF A CELL, and the chain's
// cell grows 10x from L0 (0.15) to L3 (1.5) -- so a fixed fraction throws a
// coarse tet ten times further than a fine one. On a 27 m pine that is +-0.15 m
// at L0 against +-1.52 m at L3, and L3 has only a few dozen tets to begin with:
// the displacement stops reading as organic noise on a dense crown and starts
// reading as a scatter of separated lumps with the trunk visible through it.
// The chain's own comment wants L3 to be "a blob standing in for the
// silhouette" -- a blob is exactly what the jitter was pulling apart.
//
// Only POSITION is scaled. axis_jitter is an ANGLE: a tet rotated in place
// varies its facing without moving away from the shape, so it stays useful at
// every level. overscale is likewise left alone -- it is what makes coarse tets
// overlap into a mass, and shrinking it would reopen the gaps this closes.
inline float FoliagePositionJitterForLod(size_t lod, float base_jitter) {
  const size_t level = std::min(lod, kFoliageVoxelWorldSizes.size() - 1);
  return base_jitter * kFoliageVoxelWorldSizes[0] /
         kFoliageVoxelWorldSizes[level];
}

// L3 -> L4, the switch to the baked impostor.
//
// MUST be strictly greater than the last voxel cutoff. GpuInstanceRenderer
// validates the chain as strictly ascending and says so loudly; an equal pair
// makes the level between them unreachable, which it treats as a malformed
// chain rather than as a way to retire a level.
//
// 130 rather than 70. Setting it to 70 -- L2->L3's own value -- was an attempt
// to retire L3 on the strength of a 400 m A/B where L3's tets read as fat blobs
// against the impostor's clean silhouette. That comparison was sound at 400 m
// and the conclusion drawn from it was not: it ignored the impostor's OTHER
// error term. 16 views over a hemisphere are ~20 deg apart, so a view up to
// ~10 deg off the nearest baked one shows a parallax error of roughly
// 0.18 * crown depth -- about 19 px for an 8 m tree at 70 preview metres, which
// is glaring. The tile resolution stops binding at ~58; the VIEW COUNT does not
// stop binding until several times that. 130 is where the earlier screenshots
// looked right, and L3 keeps the 70-130 band.
//
// Raising kImpostorViewsPerAxis is what would let this come in earlier.
inline constexpr float kFoliageImpostorThresholdPreviewM = 130.0f;

inline std::array<float, kFoliageLodThresholdsPreviewM.size()>
FoliageLodThresholdsForHeight(float target_height_m) {
  const float r = target_height_m / kFoliagePreviewHeight;
  std::array<float, kFoliageLodThresholdsPreviewM.size()> out{};
  for (size_t i = 0; i < out.size(); ++i)
    out[i] = kFoliageLodThresholdsPreviewM[i] * r;
  return out;
}

// Every voxel level needs exactly one bark rule: the finer ones take a
// kDefaultLodRatios entry, the coarse tail takes a budget. Adding a cell size
// above without adding its bark budget would otherwise silently reuse L3's
// (SimplifyBarkForVoxelLod clamps), giving a much coarser crown the same
// 256-triangle trunk.
static_assert(kFoliageVoxelWorldSizes.size() ==
              kDefaultLodRatios.size() + kFoliageCoarseBarkTriBudgets.size());

// Applies the LOD chain's bark decimation for `voxel_level` (0-based: 0 = the
// viewer's "Voxel L0") in place. The finer levels take kDefaultLodRatios'
// error-bounded edge collapse; the coarse levels take an absolute budget via
// vertex clustering, since edge collapse cannot merge a tree's disconnected
// per-branch tubes and so floors out thousands of triangles above these
// budgets (see SimplifyMeshSloppy's comment for the measured floors).
inline void SimplifyBarkForVoxelLod(StaticTexturedMeshComponent& bark_mesh,
                                     size_t voxel_level) {
  const size_t tri_count = bark_mesh.indices.size() / 3;
  if (tri_count == 0) return;

  SimplifiedMesh simplified;
  if (voxel_level < kDefaultLodRatios.size()) {
    if (kDefaultLodRatios[voxel_level] >= 1.0f) return;
    simplified =
        SimplifyMesh(bark_mesh.vertices, kTexturedMeshFloatsPerVertex,
                     bark_mesh.indices, kDefaultLodRatios[voxel_level]);
  } else {
    const size_t budget_index =
        std::min(voxel_level - kDefaultLodRatios.size(),
                 kFoliageCoarseBarkTriBudgets.size() - 1);
    const float ratio =
        static_cast<float>(kFoliageCoarseBarkTriBudgets[budget_index]) /
        static_cast<float>(tri_count);
    simplified = SimplifyMeshSloppy(bark_mesh.vertices,
                                    kTexturedMeshFloatsPerVertex,
                                    bark_mesh.indices, ratio);
  }

  bark_mesh.vertices = std::move(simplified.vertices);
  bark_mesh.indices = std::move(simplified.indices);
  bark_mesh.vertex_count = simplified.vertex_count;
  bark_mesh.dirty = true;
}

}  // namespace badlands
