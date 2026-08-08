#pragma once

// STANDING WATER, TAKEN FROM THE DATA AND NOWHERE ELSE.
//
// Where the water is, and what elevation its surface sits at, both come from the
// bundle: the observed cover mask says which texels are water, and the survey's
// own raster says how high. Nothing here infers water from the shape of the
// terrain.
//
// THAT RULES OUT PRIORITY-FLOOD, which is the obvious approach. Two independent
// reasons, and either alone is fatal:
//
//   - Real 1 m LiDAR is full of drystone walls, field banks, road embankments
//     and hedge bunds. All are genuine relief that genuinely dams water, and a
//     terrain-only flood ponds behind every one of them.
//   - The survey's value under water is the SURFACE, not the bed. Measured on
//     the fetched bundles, observed water is a flat plate whose rim sits ABOVE
//     it -- lake-district/00 is 253.37..261.87 m (std 0.28 m) with a rim +1.56 m
//     higher; broads/01 is 0.25..0.95 m (std 0.11 m), rim +0.17 m. Flooding to
//     that rim would raise the surface a metre and a half and drown the shore.
//
// SO THE BED IS THE ONLY THING INVENTED, and it has to be: no survey looked at a
// lake bottom, but the patch contract expresses water as `depth = max(0, level -
// bed)`, so a bed equal to the surface is a lake of zero depth -- which is to say
// no lake at all. The model is deliberately the simplest thing that reads as a
// basin: lower the ground in proportion to how far it is from the nearest
// non-water texel, up to a cap.
//
// RIVERS ARE NOT DERIVED. A ~1 km window has no catchment worth extracting from
// (measured previously at zero reaches below a 512 m cut), so a river network
// from a window this size would be an artifact of the window. Flowing water
// arrives with the simulation.

#include <cstdint>

#include "mapgen/field2d.hpp"

namespace badlands::mapgen {

// TEMPORARY, and the only quantities here that are assumed rather than read.
// Replaced by the simulation's own bathymetry.
//
// Metres of depth per metre from the shore. A shelving bank; it also makes the
// model self-limiting, since a misclassified speck of cover is never more than a
// texel or two from dry ground and so becomes a puddle rather than a pond.
inline constexpr float kLakeBedSlope = 0.15f;
// Ceiling, so a wide lake becomes a basin rather than a canyon. Reached 80 m
// from the shore at the slope above.
inline constexpr float kMaxLakeDepthM = 12.0f;

struct StandingWater {
  // The water surface. Follows the contract's convention exactly: `level == bed`
  // on dry ground, `level > bed` inside a lake, so no sentinel and no mask are
  // needed. Constant across each body, so the surface is flat by construction.
  Field2D<float> level;
  // The input raster with lake beds carved beneath their surfaces. Identical to
  // the input BITWISE outside every lake.
  Field2D<float> bed;
};

// `dtm` is the survey's raster: ground on land, water SURFACE over water.
// `cover` holds mapgen::Cover values and must match `dtm`'s dimensions;
// Cover::Water is the whole of the extent signal.
//
// A mismatched or empty `cover` yields an entirely dry result rather than an
// error -- there is no water to report without observation, and guessing it from
// terrain is what this approach exists to reject.
//
// THE PATCH EDGE IS NOT A SHORE. Water running off the side of the patch
// continues in the real world, so the distance field is seeded only from actual
// non-water texels; a lake cut by the frame stays deep at the cut instead of
// tapering to nothing against a boundary that is not there.
//
// Feed the result to patch_io.hpp's derive_water for depth, lake ids and the
// LakeInfo records.
StandingWater derive_standing_water(const Field2D<float>& dtm,
                                    const Field2D<uint8_t>& cover,
                                    float texel_m);

}  // namespace badlands::mapgen
