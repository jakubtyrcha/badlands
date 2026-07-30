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

#include "mapgen/erosion.hpp"
#include "mapgen/river_graph.hpp"
#include "mapgen/smooth.hpp"
#include "mapgen/field2d.hpp"

namespace badlands::mapgen {

// Output-resolution post-processing, distinct from the sim's own parameters.
// The detail_* fields on ErosionParams arguably belong here too — they are
// also an output-res post-process — but migrating them is a separable change
// deliberately left out.
struct PostProcessParams {
  // Gaussian sigma in WORLD METRES; <= 0 disables. Metres rather than texels so
  // the same value gives the same terrain at any resolution.
  //
  // Chosen from the preview across sigma {1, 2} x strength {0.5, 1.0}, not
  // from the numbers: 2.0/1.0 washed the terrain out entirely (the hillshade
  // went near-white as slope magnitudes collapsed), while 1.0/0.5 was too
  // subtle to answer the complaint. 1.0/1.0 takes the gully wrinkle off and
  // leaves the fans and basins reading clearly.
  float smooth_sigma_m = 1.0f;
  float smooth_strength = 1.0f;  // 0 = passthrough, 1 = fully blurred
};

struct MapGenParams {
  uint32_t seed = 1;
  int resolution = 512;         // output grid (texels, square)
  float world_size_m = 512.0f;  // world extent (meters, square)
  ErosionParams erosion;        // hydraulic erosion + lakes sim (own sim grid)
  PostProcessParams post;       // output-res smoothing (see smooth.hpp)
};

// Everything one generation produces. `bedrock` is the latent field the biomes
// were cut from — kept because previews dump it and erosion will consume it.
struct MapArtifacts {
  Field2D<float> bedrock;      // latent field (raw; roughly [0, 3.5])
  Field2D<uint8_t> biome;      // Biome enum values, incl. Lake post-erosion
  Field2D<float> heightmap;    // world meters — eroded + detailed ground surface
  Field2D<float> water_depth;  // world meters — standing water; surface = heightmap + water_depth
  // Per-cell lake index into `lakes`, -1 where dry, and the per-lake record.
  // A Seeded lake was placed deliberately by carve_cavities and is never
  // pruned; an Emergent one is whatever the sim ponded. Sim-grid indices
  // resolved to the OUTPUT grid by nearest sample.
  Field2D<int32_t> lake_id;
  std::vector<LakeInfo> lakes;
  Field2D<float> flow;         // drainage area (m^2)
  Field2D<float> sediment;     // sediment thickness (m)
  // v2 river network. Rasterized conservatively from `river_graph` at output
  // resolution rather than resampled from a sim-grid raster, so a thin channel
  // stays connected instead of smearing or fragmenting.
  //
  // Hierarchy is carried by `river_class`, not by width: at honest runoff a
  // 512 m map's largest outlet is ~0.0025 m^3/s, giving a 0.25 m channel —
  // sub-texel. Width is not lost, it lives on the graph and is recoverable
  // anywhere as channel_width_coeff * sqrt(discharge).
  Field2D<float> river_discharge_m3_s;  // reach discharge splatted onto its texels
  Field2D<uint8_t> river_class;         // RiverClass; 0 doubles as the channel mask
  Field2D<float> river_depth_m;
  Field2D<float> river_speed_m_s;
  Field2D<glm::vec2> river_flow_dir;    // unit; (0,0) off-channel
  RiverGraph river_graph;               // width_m, strahler_order, shreve_magnitude
};

// sink, if non-null, receives named debug rasters as generation proceeds (see
// generator.cpp for the exact stage list and emission order).
MapArtifacts generate_map(const MapGenParams& params, MapDebugSink* sink = nullptr);

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
// nonzero mask texel, with texel (x, y) at world (x*texel_m.x, y*texel_m.y).
// Felzenszwalb–Huttenlocher two-pass EDT — exact, not a chamfer
// approximation. An all-zero mask returns all zeros. Generic over the seed
// set (distance_to_plains wraps this with a Plains mask; the detail filter
// needs distance-to-water).
Field2D<float> distance_to_mask(const Field2D<uint8_t>& mask,
                                glm::vec2 texel_m);

// Exact Euclidean distance (WORLD METERS) from each texel to the nearest
// texel classified Plains, with texel (x, y) at world (x*texel_m.x,
// y*texel_m.y). A map with no plains at all returns all zeros (unreachable
// via generate_map: the quantile cutoffs guarantee a plains share). Thin
// wrapper over distance_to_mask. Exposed for unit testing (pattern of
// compute_cutoffs).
Field2D<float> distance_to_plains(const Field2D<uint8_t>& biome,
                                  glm::vec2 texel_m);

}  // namespace badlands::mapgen
