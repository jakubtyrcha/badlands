#pragma once

// What a forest IS, as data. The generator (scatter.hpp) carries no per-species
// and no per-layer logic: everything that distinguishes a pine forest from a
// scrubland lives in one of these structs.
//
// Nothing here names a species, a mesh, or an asset. A "model" is an index and
// three numbers the sampler needs (how much room it takes, how tall it stands,
// how big it varies); what that index MEANS is the consumer's table. See
// src/game/visual/forest_catalog.hpp for badlands' binding of index -> tree.

#include <cstdint>
#include <vector>

#include <glm/glm.hpp>

namespace badlands::foliage {

// A trapezoidal response to depth-into-forest, in world METRES.
//
// Metres, not a normalized 0..1 blend, because that is the only unit in which
// "young trees at the edge" is a stable statement -- a forest edge is a light
// and microclimate gradient with a real length scale (~10-30 m in temperate
// forest), while a biome blend's width is an artifact of raster resolution.
//
//   value = 0                      for d <= rise_start
//           ramps 0 -> 1           over [rise_start, rise_end]
//           1                      over [rise_end, fall_start]
//           ramps 1 -> 0           over [fall_start, fall_end]
//           0                      for d >= fall_end
//
// Set fall_start/fall_end to FLT_MAX for a plateau that never falls (a canopy
// species that keeps going all the way into the interior). Degenerate ramps
// (rise_end <= rise_start) are treated as instantaneous steps, not as
// divide-by-zero.
struct DepthCurve {
  float rise_start = 0.0f;
  float rise_end = 0.0f;
  float fall_start = 3.4e38f;
  float fall_end = 3.4e38f;

  float Evaluate(float depth_m) const;
};

// One plantable thing.
//
// `radius_m` is the model's CROWN radius at scale 1 -- the radius of the
// smallest vertical cylinder about its trunk that contains everything it draws.
// It is not a hand-picked "personal space" figure: the consumer measures it off
// the model's own bounds (see game/visual/crown_bounds.hpp) and writes it here,
// because a number typed by hand drifts from the mesh the moment a preset
// changes, and every drift shows up as either interpenetrating crowns or a
// forest mysteriously too sparse.
//
// The sampler treats it as a circle that may not overlap another instance's
// circle -- the rule is r_i * s_i + r_j * s_j, a SUM, so a bush is pushed clear
// of an oak's drip line rather than merely clear of its trunk. That the radius
// is per-model is what makes the sampler multi-class: a bush and an oak in one
// forest keep entirely different amounts of room.
//
// `height_m` is its standing height at scale 1, used ONLY to give a cell its Y
// bounds (the generator never draws anything).
struct FoliageModel {
  float radius_m = 1.0f;
  float height_m = 1.0f;
  glm::vec2 scale_range{0.9f, 1.1f};  // per-instance uniform scale, lerped
  float weight = 1.0f;                // selection weight within its layer
  DepthCurve depth;                   // shifts the species mix with depth
};

// One placement pass. Layers are placed in the order they are declared, and an
// earlier layer's instances block a later one's -- so declaration order IS
// priority, and the canopy must come first if it is to claim its space before
// the undergrowth fills in.
struct FoliageLayer {
  // Jittered-grid spacing: exactly one candidate is tried per grid_m x grid_m
  // square, so this is the layer's density ceiling before any test rejects
  // anything. Spacing itself is per-MODEL (FoliageModel::radius_m).
  float grid_m = 4.0f;
  float max_slope_deg = 35.0f;

  // How MANY, as a function of depth (the models' own curves decide WHICH).
  DepthCurve density;

  // The young-at-the-edge ramp: instance scale is multiplied by a factor
  // climbing linearly from `edge_scale` at depth 0 to 1.0 at
  // `edge_scale_depth_m`. Set edge_scale = 1 to disable.
  float edge_scale = 1.0f;
  float edge_scale_depth_m = 1.0f;

  // This layer's slice of ForestType::models.
  uint16_t first_model = 0;
  uint16_t model_count = 0;
};

// Clumping and edge-raggedness tuning. Both are fBm fields; both are what stop
// the output reading as a lawn with a compass-drawn boundary.
struct ForestNoise {
  // Density clumping. Wavelength is the size of a thicket / glade.
  float clump_wavelength_m = 35.0f;
  int clump_octaves = 3;
  // The remap window applied to the clump fBm (which is ~[-1,1], rescaled to
  // [0,1] first): below `lo` -> 0, above `hi` -> 1, linear between. A window
  // narrower than [0,1] is what opens real glades instead of merely thinning
  // the forest everywhere.
  //
  // Tuned against the ACTUAL noise distribution, not by eye. FastNoiseLite's
  // fBm is nothing like uniform on [-1,1]: at 3 octaves it measures mean 0.000,
  // sd 0.354, p05/p95 of -0.577/+0.575 -- i.e. concentrated near the middle. A
  // window has to be read against that, and the obvious-looking [0.35, 0.80]
  // is a trap: it leaves only 4.1% of the map at full density and 22% at zero,
  // so it does not carve glades, it just thins everything (measured mean
  // multiplier 0.374). [0.30, 0.55] sits below the distribution's centre and
  // gives 40.6% fully closed canopy against 14.8% true glade (mean 0.638) --
  // clumping with real open space, which is the thing being asked for.
  float clump_lo = 0.30f;
  float clump_hi = 0.55f;

  // Depth-field warp. Without it the tree line is a smooth offset curve around
  // the biome mask -- geometrically correct and obviously artificial.
  float warp_amp_m = 4.0f;
  float warp_wavelength_m = 12.0f;
};

// A complete forest definition.
struct ForestType {
  std::vector<FoliageModel> models;
  std::vector<FoliageLayer> layers;  // in placement priority order
  ForestNoise noise;

  // True when every layer's model slice lies inside `models` and every layer
  // has at least one model. The generator checks this and refuses to run on a
  // malformed table rather than reading out of bounds.
  bool Valid() const;
};

}  // namespace badlands::foliage
