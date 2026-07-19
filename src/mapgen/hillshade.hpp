#pragma once

// Hillshaded relief render of a heightmap, for judging terrain shape by eye.
//
// A raw grayscale heightmap is nearly useless for this: "sharp connected ridges with
// valleys" and "gentle swells" can look almost identical as grey ramps, because the eye
// reads *slope*, not absolute height. Shading by the surface normal is what makes ridge
// lines, valley floors and terracing visible at a glance.

#include <string>

#include "mapgen/field2d.hpp"

namespace badlands::mapgen {

// Shade of `height` (world meters) under a fixed sun, in [0,1], at the height field's
// own resolution.
//
// `meters_per_sample` sets the horizontal scale, so the slope is real: the SAME height
// field shaded at a different sample density is genuinely a different surface, and
// hard-coding 1.0 would silently lie about how steep the terrain is.
//
// The light is a fixed NW / 45-degree sun (the cartographic convention -- lighting from
// the top-left is what makes relief read as raised rather than sunken). A little ambient
// keeps shadowed faces from crushing to black and hiding their shape.
Field2D<float> compute_hillshade(const Field2D<float>& height,
                                 float meters_per_sample = 1.0f);

// compute_hillshade + write it as 8-bit grayscale.
void write_hillshade_png(const Field2D<float>& height, const std::string& path,
                         float meters_per_sample = 1.0f);

// Nearest-neighbour upscale by an integer factor. Display only: terrain is authored and
// evaluated at the real sample density, and this just makes a 128-sample patch legible
// on screen without pretending to add detail (nearest, not bilinear, so no invented
// smoothness).
Field2D<float> upscale_nearest(const Field2D<float>& field, int factor);

}  // namespace badlands::mapgen
