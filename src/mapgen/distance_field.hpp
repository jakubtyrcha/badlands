#pragma once

// Exact Euclidean distance transform over a 2D mask, in WORLD METERS.
//
// Split out of generator.{hpp,cpp} so a consumer can link the EDT ALONE.
// `generator.hpp` pulls in erosion, rivers and smoothing, and `generator.cpp`
// links FastNoiseLite and the whole generation pipeline behind it -- far too
// much for a caller that only wants a distance field. The foliage generator
// (src/foliage/, which is deliberately engine-free and Dawn-free) compiles
// this TU directly rather than linking badlands_mapgen_lib, which links
// badlands_engine.
//
// Pure CPU: field2d.hpp + parallel.hpp (both header-only) + glm. No noise, no
// I/O, no failure path.

#include <glm/glm.hpp>

#include "mapgen/field2d.hpp"

namespace badlands::mapgen {

// Exact Euclidean distance (WORLD METERS) from each texel to the nearest
// nonzero mask texel, with texel (x, y) at world (x*texel_m.x, y*texel_m.y).
// Felzenszwalb–Huttenlocher two-pass EDT — exact, not a chamfer
// approximation. An all-zero mask returns all zeros (the documented
// degenerate: there is no seed to measure a distance to).
//
// Generic over the seed set on purpose: mapgen's distance_to_plains wraps it
// with a Plains mask, the detail filter needs distance-to-water, and the
// foliage depth field needs distance-to-forest-edge.
Field2D<float> distance_to_mask(const Field2D<uint8_t>& mask,
                                glm::vec2 texel_m);

}  // namespace badlands::mapgen
