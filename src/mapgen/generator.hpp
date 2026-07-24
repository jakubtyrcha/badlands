#pragma once

// Procedural map generation: a continuous "bedrock" latent field (low-frequency
// fBm + belt-masked ridged fractal), classified into biomes by quantile
// cutoffs. See docs/superpowers/specs/2026-07-24-biome-generation-design.md.
//
// Pure function of params — no I/O, no failure path. Noise is sampled in world
// METERS, so the same (seed, size_m) at two resolutions is the same map, just
// sharper.

#include <cstdint>

#include <glm/glm.hpp>

#include "mapgen/field2d.hpp"

namespace badlands::mapgen {

struct MapGenParams {
  uint32_t seed = 1;
  glm::ivec2 resolution{512, 512};   // texels
  glm::vec2 size_m{512.0f, 512.0f};  // world meters
};

// Everything one generation produces. `bedrock` is the latent field the biomes
// were cut from — kept because previews dump it and erosion will consume it.
struct MapArtifacts {
  Field2D<float> bedrock;    // latent field (raw; roughly [0, 3.5])
  Field2D<uint8_t> biome;    // Biome enum values (Plains/Hills/Mountain now)
  Field2D<float> heightmap;  // world meters — all zeros this phase
};

MapArtifacts generate_map(const MapGenParams& params);

// --- exposed for unit tests (threshold logic without the noise) ---

// Target area fractions. Quantile cutoffs make them structural: they hold for
// every seed, not on average.
inline constexpr float kPlainsFrac = 0.55f;
inline constexpr float kMountainFrac = 0.12f;

// Quantile cutoffs over the ACTUAL bedrock raster: t_hills at kPlainsFrac,
// t_mountain at 1 - kMountainFrac (exact order statistics).
struct BiomeCutoffs {
  float t_hills = 0.0f;
  float t_mountain = 0.0f;
};
BiomeCutoffs compute_cutoffs(const Field2D<float>& bedrock);

// bedrock < t_hills -> Plains, < t_mountain -> Hills, else Mountain.
Field2D<uint8_t> classify_biomes(const Field2D<float>& bedrock,
                                 const BiomeCutoffs& cutoffs);

// Exact Euclidean distance (WORLD METERS) from each texel to the nearest
// texel classified Plains, with texel (x, y) at world (x*texel_m.x,
// y*texel_m.y). Felzenszwalb–Huttenlocher two-pass EDT — exact, not a
// chamfer approximation. A map with no plains at all returns all zeros
// (unreachable via generate_map: the quantile cutoffs guarantee a plains
// share). Exposed for unit testing (pattern of compute_cutoffs).
Field2D<float> distance_to_plains(const Field2D<uint8_t>& biome,
                                  glm::vec2 texel_m);

}  // namespace badlands::mapgen
