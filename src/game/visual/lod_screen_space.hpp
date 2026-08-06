#pragma once

// Derives a model's LOD ladder -- how many levels, how coarse each one is, and
// where each takes over -- from its WORLD SIZE and its source triangle count.
//
// Pure math: no GPU, no mesh, no domain vocabulary. Header-only.
//
// WHY DERIVED RATHER THAN TABULATED. A fixed ratio schedule spreads results
// over whatever band the inputs span: the shipped props run 2.6k to 103k
// triangles at sizes from a warhammer to a boulder, so one table would leave
// the big models expensive and the small ones needlessly coarse. Deriving from
// a screen-space budget makes both the LEVEL COUNT and the switch distances
// per-model, which is exactly what GpuInstanceRenderer::ModelLod already
// supports (its LOD count is runtime, not the compile-time kMaxLods cap).
//
// NOT USED BY FOLIAGE, on purpose. kFoliageVoxelWorldSizes,
// kFoliageLodThresholdsPreviewM and the 130 m impostor cutoff are
// screenshot-tuned around real measured failures -- the pine needle-sprig
// aliasing dead zone, and the 16-view parallax A/B that rejected 70 m (see
// foliage_voxel_config.hpp). Re-deriving them through this formula would
// silently change the forest. The two coexist deliberately.

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <vector>

#include "engine/rendering/gpu_instance_renderer.hpp"

namespace badlands {

// Vertical focal length in pixels at the reference framing: 1920x1080, 60
// degree vertical FOV, i.e. (1080/2) / tan(30deg). The same reference the
// foliage cutoffs were originally derived from, kept identical so the two
// families of numbers remain comparable even though neither feeds the other.
inline constexpr float kLodReferenceFocalPx = 935.0f;

struct LodLadderOptions {
  // Screen pixels one triangle should cover at the level's NEAR edge -- the
  // most demanding point of its range. Lower means denser meshes held longer.
  float px_per_triangle = 8.0f;
  // Each level takes over at this multiple of the previous level's distance,
  // so its budget falls by the square. 2.0 quarters the triangle count per
  // step, which matches the ~4x steps the foliage chain settled on.
  float distance_growth = 2.0f;
  // Below this the level is not worth its draw call and its own vertex buffer.
  int min_triangles = 64;
  // Where the impostor takes over, as a multiple of the model's size. 16.25 is
  // the foliage chain's own ratio (130 m for an 8 m tree), and it transfers
  // because it is set by the ATLAS -- 16 views over a hemisphere are ~20
  // degrees apart, so a view up to ~10 degrees off the nearest baked one shows
  // a parallax error of roughly 0.18 * depth. That is a property of
  // kImpostorViewsPerAxis, not of trees. Raising the view count is what would
  // let the impostor come in earlier.
  float impostor_size_ratio = 16.25f;
  // Total mesh levels INCLUDING level 0, capped one below the engine's
  // compile-time limit so the impostor the field builder appends still fits.
  size_t max_mesh_levels = GpuInstanceRenderer::kMaxLods - 1;
};

// Level 0 is always the source mesh and has no budget entry.
// `triangle_budgets[i]` is level i+1's target, taking over at `thresholds[i]`.
// So a ladder with N budgets describes N+1 mesh levels and N cutoffs, which is
// exactly InstancedLodModel's thresholds contract.
struct LodLadder {
  std::vector<int> triangle_budgets;
  std::vector<float> thresholds;  // world metres, strictly ascending
  float impostor_threshold_m = 0.0f;

  size_t mesh_level_count() const { return triangle_budgets.size() + 1; }
};

// The ladder for a model of `size_m` (its bounding-sphere DIAMETER, not its
// height -- a wide flat rock must not be underestimated the way a tree's
// height happens to work) and `source_tris` triangles.
//
// The construction, in one line each:
//
//   d1 = size * focal / sqrt(source_tris * px_per_tri)
//        the distance at which the SOURCE mesh first meets the density target;
//        nearer than this it is genuinely needed.
//   thresholds[i] = d1 * growth^(i+1)
//        the first coarser level waits one growth step past d1 -- at d1 itself
//        the derived budget is exactly source_tris, i.e. an identity level.
//   budgets[i]    = source_tris / growth^(2*(i+1))
//        distance grows, so covered area falls by the square.
//
// Levels stop at the first of: a budget below `min_triangles`, `max_mesh_levels`
// reached, or a threshold that has run past the impostor. That last rule is what
// keeps the chain strictly ascending once the field builder appends
// `impostor_threshold_m` -- GpuInstanceRenderer rejects an equal pair, treating
// it as a malformed chain rather than as a way to retire a level.
//
// Degenerate inputs (no size, no triangles) return a bare level 0 with no
// cutoffs, which is a valid one-level chain.
inline LodLadder BuildLodLadder(float size_m, size_t source_tris,
                                const LodLadderOptions& opts = {}) {
  LodLadder ladder;
  if (!(size_m > 0.0f) || source_tris == 0) return ladder;

  ladder.impostor_threshold_m = size_m * opts.impostor_size_ratio;

  const float px_per_tri = std::max(opts.px_per_triangle, 0.01f);
  const float growth = std::max(opts.distance_growth, 1.01f);
  const float d1 = size_m * kLodReferenceFocalPx /
                   std::sqrt(static_cast<float>(source_tris) * px_per_tri);

  float distance = d1;
  double budget = static_cast<double>(source_tris);
  // Against mesh_level_count(), NOT budget count -- level 0 has no budget
  // entry, so the two differ by one and comparing the wrong one overshoots the
  // cap by a level, leaving no room for the impostor on top.
  while (ladder.mesh_level_count() < opts.max_mesh_levels) {
    distance *= growth;
    budget /= static_cast<double>(growth) * growth;

    if (budget < static_cast<double>(opts.min_triangles)) break;
    if (distance >= ladder.impostor_threshold_m) break;

    ladder.triangle_budgets.push_back(static_cast<int>(budget));
    ladder.thresholds.push_back(distance);
  }
  return ladder;
}

}  // namespace badlands
