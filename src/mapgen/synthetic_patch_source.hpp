#pragma once

// A patch invented analytically: a valley with a river running down it and a
// lake at the bottom, at a realistic absolute elevation.
//
// WHY THIS EXISTS. Styling work -- materials, foliage, water, relief -- needs a
// patch to look at, and getting one out of the real pipeline costs a ~5 minute
// coarse simulation plus a patch-selection search whose gates cannot even be
// satisfied at small sizes. None of that has anything to do with how the terrain
// LOOKS. This source removes both from the loop: it answers any request
// instantly, deterministically, with no files and no upstream stage.
//
// It is not a toy. Everything downstream of PatchData runs against it unchanged
// -- the cluster DAG, the biome splat and terrain materials, the lake surface,
// the river arcs and carve, the foliage plopper -- so it exercises the whole of
// stage 3. If stage 3 can tell this apart from a real patch, the contract is
// leaking.
//
// DELIBERATELY ANALYTIC, not noise-driven. Every feature has a closed form, so a
// test can state the right answer rather than pin whatever came out: the valley
// floor is where the profile says it is, the lake surface is exactly flat, the
// channel runs exactly down the centreline. Adding fBm here would trade that for
// prettiness the styling pass is going to replace anyway.
//
// THE ELEVATION IS THE POINT OF THE DEFAULT. A protogen world sits 400-900 m up,
// and the map view's camera frames a patch by pitching at a focus pinned to
// y = 0 -- so a patch that sits near zero hides the framing bug that a real one
// exposes. The default base elevation keeps this source honest about that.

#include <cstdint>

#include "mapgen/patch_source.hpp"

namespace badlands::mapgen {

// The scene, declared rather than simulated. Every length is world METRES, so
// the same params at two resolutions give the same patch, sampled more finely.
struct SyntheticPatchParams {
  // Ground elevation at the upstream edge of the valley floor.
  float base_elevation_m = 420.0f;
  // Fall along +Z, in metres per metre. This is also the channel slope the
  // hydraulics solve against.
  float downstream_drop = 0.03f;
  // The valley: a compact-support cross-section, zero outside its half width.
  float valley_depth_m = 14.0f;
  float valley_width_m = 96.0f;
  // Relief on the valley SIDES only, tapered to zero at the channel so the bed
  // stays clean. Two incommensurate periods, so it does not read as tiling.
  float hill_relief_m = 22.0f;
  // Discharge at the downstream end; the channel grows into it from 30% at the
  // headwater, which is what gives width and depth somewhere to go.
  float river_discharge_m3_s = 0.6f;
  // Share of the patch, measured from the downstream edge, that ponds. 0
  // disables the lake entirely.
  float lake_fraction = 0.22f;
  // Soil depth on flat ground; thins to zero as slope approaches the reference.
  float soil_max_m = 3.0f;
  float soil_slope_ref_deg = 34.0f;
  // Absolute soil cutoffs for the biome split, thinnest first:
  //
  //     wet                        -> Lake
  //     soil <  cut_mountain       -> Mountain   (bare rock)
  //     soil <  cut_hills          -> Hills
  //     soil <  cut_forest         -> Plains
  //     otherwise                  -> Forest     (deepest cover)
  //
  // Absolute rather than per-patch quantiles for the same reason the real
  // pipeline puts them in the coarse manifest: a quantile taken over the patch
  // makes the same ground classify differently depending on what was cut.
  //
  // Forest is included deliberately. The real classification emits none, so a
  // pipeline patch plants no trees at all -- which would leave the foliage half
  // of stage 3 untested by the very fixture meant to exercise it.
  float soil_cut_mountain_m = 0.35f;
  float soil_cut_hills_m = 1.40f;
  float soil_cut_forest_m = 2.30f;
};

class SyntheticPatchSource final : public PatchSource {
 public:
  SyntheticPatchSource() = default;
  explicit SyntheticPatchSource(const SyntheticPatchParams& params)
      : params_(params) {}

  PatchData Fetch(const PatchRequest& req) const override;

  const SyntheticPatchParams& params() const { return params_; }

 private:
  SyntheticPatchParams params_{};
};

// Exposed so a test can assert the terrain against the same closed form the
// source fills its raster from, rather than against a copy of it. Patch-local
// world metres; `size_m` is the patch extent.
float synthetic_ground_m(const SyntheticPatchParams& p, float size_m, float x,
                         float z);

}  // namespace badlands::mapgen
