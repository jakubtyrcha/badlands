#pragma once

// STANDING WATER from a DTM plus an OBSERVED water mask.
//
// THE SURVEY'S VALUE UNDER WATER IS THE SURFACE, NOT THE BED. Every national
// LiDAR programme synthesises water differently -- France hydro-enforces with
// virtual points, England fills from older surveys, the Netherlands leaves
// nodata -- but none of them measures the bottom. Measured on the fetched
// bundles, the observed-water region is a flat plate:
//
//     lake-district/00   13.1% water   253.37 .. 261.87 m   std 0.28 m
//     broads/01           0.8% water     0.25 ..   0.95 m   std 0.11 m
//
// and its immediate rim sits +1.56 m and +0.17 m ABOVE that plate.
//
// That measurement rules out priority-flood, which was the obvious approach:
// route_flow would find the plate as a depression and fill it to the rim,
// raising the surface a metre and a half and drowning the shore. It also rules
// out reading the plate as `height`, which the patch contract defines as THE
// BED -- doing so asserts a lake of zero depth, so `depth = max(0, level -
// height)` is zero everywhere and the map has no lake in it at all.
//
// So: observation defines WHICH texels are one body of water, the plate defines
// its LEVEL, and the level cutting the real bed defines its SHORELINE at the
// heightfield's own resolution rather than at land cover's 10 m staircase.
//
// RIVERS ARE NOT DERIVED. A ~1 km window has no catchment worth extracting from
// (measured previously at zero reaches below a 512 m cut), so a river network
// from a window this size would be an artifact of the window. Flowing water
// arrives with the simulation.

#include <cstdint>

#include "mapgen/field2d.hpp"

namespace badlands::mapgen {

// TEMPORARY, and the one quantity here that is assumed rather than measured.
//
// Nothing in the bundle knows how deep a lake is, because no survey looked. But
// the contract cannot express "there is a lake here" without a bed below the
// surface, so refusing to choose means silently dropping every lake. These say
// what was assumed, in metres, where a reader will trip over them. Replaced by
// the simulation's own bathymetry.
inline constexpr float kAssumedLakeDepthM = 3.0f;
// Distance from the shoreline over which the bed falls to that full depth. A
// step instead of a taper would put a vertical wall at the waterline, which is
// visible wherever the water is clear enough to see through.
inline constexpr float kShoreTaperM = 20.0f;

// Smallest observed-water component worth treating as a lake, in texels. Land
// cover at 10 m misclassifies isolated pixels; one of them should not carve a
// pond into the terrain.
inline constexpr int kMinLakeTexels = 64;

struct StandingWater {
  // The water surface. Follows the contract's convention exactly: `level ==
  // bed` on dry ground, `level > bed` inside a lake, so no sentinel and no mask
  // are needed and the surface is flat by construction.
  Field2D<float> level;
  // The input DTM with lake beds carved beneath their surfaces. Identical to
  // the input BITWISE outside every lake.
  Field2D<float> bed;
};

// `dtm` is the survey's raster: ground on land, water SURFACE over water.
// `cover` holds mapgen::Cover values and must match `dtm`'s dimensions;
// Cover::Water is the observed signal.
//
// A mismatched or empty `cover` yields an entirely dry result rather than an
// error -- there is no honest lake to report without observation, and guessing
// one from terrain alone is what this whole approach rejects.
//
// Feed the result to patch_io.hpp's derive_water for depth, lake ids and the
// LakeInfo records.
StandingWater derive_standing_water(const Field2D<float>& dtm,
                                    const Field2D<uint8_t>& cover,
                                    float texel_m);

}  // namespace badlands::mapgen
