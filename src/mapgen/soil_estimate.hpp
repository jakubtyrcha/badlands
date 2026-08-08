#pragma once

// TEMPORARY. Erodible cover estimated from slope, for map sources that carry a
// heightfield and nothing else.
//
// THIS IS A STAND-IN FOR THE SIMULATION'S soil RASTER, and the only reason it
// exists is that real LiDAR arrives with no substrate history. Delete it the
// moment a coarse world's soil field is available for the same ground.
//
// It is not an invented aesthetic scalar. The relation it encodes was MEASURED
// on the 16 km coarse world, where soil depth and slope separate by 4.4x:
//
//     bare/thin (< 0.5 m)   53.6% of the map   mean slope 31.6 deg
//     deep      (> 4 m)     29.1% of the map   mean slope  7.1 deg
//
// so "steep ground is close to bedrock" falls out of the erosion physics rather
// than being imposed on it. What is fabricated here is only the interpolation
// BETWEEN those two points, and the choice of an exponential to do it with.
//
// The honest limitation: soil depth in the sim is genuinely orthogonal to
// elevation and only correlated with slope. Two slopes of equal gradient can
// carry very different regolith depending on what drained across them, and this
// cannot know that. When the sim's raster arrives it replaces SLOPE as the
// input to the same consumers rather than changing them -- which is why the
// consumers take soil, not slope, even today.

#include "mapgen/field2d.hpp"

namespace badlands::mapgen {

// The two measured points the fit passes through. Named rather than folded into
// coefficients so the calibration stays legible and re-derivable.
inline constexpr float kMeasuredDeepSoilM = 4.0f;
inline constexpr float kMeasuredDeepSlopeDeg = 7.1f;
inline constexpr float kMeasuredThinSoilM = 0.4f;
inline constexpr float kMeasuredThinSlopeDeg = 31.6f;

// Metres of erodible cover per texel, from the local slope of `height`.
//
// Slope is a central difference in world metres, so `texel_m` is load-bearing:
// the same terrain at two resolutions must give the same soil, and a gradient
// divided by texels instead of metres would not.
//
// A degenerate field (empty, or texel_m <= 0) returns an empty field.
Field2D<float> estimate_soil(const Field2D<float>& height, float texel_m);

}  // namespace badlands::mapgen
