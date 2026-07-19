#pragma once

// Derive world-static fog emitters from the biome map (Task: map fog generator).
//
// The map is sampled on a jittered 32 m patch grid; each patch's neighbourhood is
// classified by a majority biome vote, and Forest / Swamp patches emit one fog
// emitter whose FOOTPRINT is fitted to the covariance of the matching-biome
// samples around it — an oriented ellipse (EmitterShape::Ellipse: the OBB frame
// read through a radial function). Forest fog is flat (EmitterType::Disc); swamp
// fog is the same radial footprint modulated by a time-animated noise slice
// (EmitterType::Noise) for a granular, drifting look. Other biomes emit nothing.
//
// Pure CPU (glm only) so it is unit-testable without the pipeline or a GPU.

#include <cstdint>
#include <vector>

#include <glm/glm.hpp>

#include "engine/rendering/fog_sim.hpp"
#include "mapgen/field2d.hpp"

namespace badlands::mapgen {

// Tunables for GenerateBiomeFog. Distances are world meters (1 sample = 1 m).
struct BiomeFogParams {
  float patch_m = 32.0f;          // one sample point is placed per patch_m square
  float gather_radius_m = 16.0f;  // neighbourhood radius for the vote + covariance fit
  int min_samples = 12;           // skip patches with too few matching samples to fit
  float extent_scale = 2.0f;      // half_extent = extent_scale * sqrt(eigenvalue)
  float min_extent_m = 4.0f;      // floor so a near-collinear fit still has width
  float base_lift_m = 0.0f;       // emitter base above the terrain at its centre
  float height_m = 14.0f;         // vertical extent of the fog volume
  float magnitude = 0.05f;        // peak sigma_t (m^-1)
  float radial_falloff = 0.55f;   // soft elliptical edge
  float vertical_falloff = 0.5f;
  // Swamp noise fill.
  float noise_freq = 0.12f;
  float noise_contrast = 1.6f;
  glm::vec3 noise_scroll{0.0f, 0.3f, 0.0f};
};

// One emitter per qualifying Forest/Swamp patch, fitted to the biome map. `height`
// is sampled for the emitter base_y; `seed` makes the patch jitter deterministic.
std::vector<fog::Emitter> GenerateBiomeFog(const Field2D<uint8_t>& biome,
                                           const Field2D<float>& height,
                                           uint32_t seed,
                                           const BiomeFogParams& params = {});

}  // namespace badlands::mapgen
