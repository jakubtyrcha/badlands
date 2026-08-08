#pragma once

// NODATA FILL: a prepass, and deliberately nothing more.
//
// Real bare-earth DTMs have voids -- water bodies the survey could not return
// from, buildings removed in post, flight-line gaps. They arrive as a sentinel
// value, and everything downstream (slope, curvature, the cluster DAG, the
// hillshade) treats a sentinel as terrain hundreds of metres below its
// neighbours, which is a one-texel cliff at every void boundary.
//
// This knows nothing about patches, cover, providers or terrain-net. It takes a
// field and a sentinel, and returns a MASK of what it touched. The caller
// decides what a filled texel means -- marking it Cover::Unknown, refusing the
// patch above some fraction, or ignoring it. Keeping that decision out of here
// is the whole point: the fill can be replaced with something better (an
// inpaint that respects local gradient, say) without a consumer noticing.

#include <cstdint>

#include "mapgen/field2d.hpp"

namespace badlands::mapgen {

// Replaces every invalid texel with the nearest valid sample, in place.
//
// INVALID means exactly `sentinel`, or any non-finite value. NaN is included on
// purpose: a raster that reaches here still carrying NaN would otherwise poison
// every derived quantity silently, and the caller asked for a field it can
// differentiate.
//
// "Nearest" is 4-connected BFS distance, not Euclidean -- the simplest thing
// that terminates in one pass and is O(width * height). Expand when a void
// large enough for the difference to be visible actually shows up.
//
// Returns a mask the same size as `field`, 1 where a texel was filled. The mask
// is always correctly sized, even when nothing was filled.
//
// DETERMINISTIC: the frontier is seeded in linear-index order and neighbours are
// visited in a fixed order, so the same input always yields the same fill.
//
// A field with no invalid texel is left BITWISE unchanged. A field that is
// ENTIRELY invalid has nothing to fill from and is also left unchanged, with a
// fully-set mask -- the caller has to handle that case, and quietly writing
// zeros would hide it.
Field2D<uint8_t> fill_nodata(Field2D<float>& field, float sentinel);

}  // namespace badlands::mapgen
