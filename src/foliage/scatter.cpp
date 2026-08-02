#include "foliage/scatter.hpp"

#include <algorithm>
#include <cmath>
#include <vector>

#include <FastNoiseLite.h>
#include <spdlog/spdlog.h>

#include "foliage/depth_field.hpp"
#include "foliage/hash.hpp"

namespace badlands::foliage {

namespace {

constexpr float kTwoPi = 6.28318530718f;

// Already-placed instances, filed into a uniform grid so a spacing test touches
// a 3x3 neighbourhood instead of every instance placed so far.
//
// `cell_m` must be at least TWICE the largest radius any instance can take.
// The pairwise rule is r_p + r_q, so a conflicting neighbour can sit up to
// 2 * R_max away; only a cell that wide guarantees it is at most one cell out
// and therefore inside the 3x3 sweep. (It used to be R_max, which was correct
// for the old max(r_p, r_q) rule and silently wrong for this one.)
class SpacingGrid {
 public:
  SpacingGrid(glm::vec2 origin_m, glm::vec2 size_m, float cell_m)
      : origin_(origin_m), cell_(cell_m) {
    nx_ = std::max(1, static_cast<int>(std::ceil(size_m.x / cell_m)) + 1);
    nz_ = std::max(1, static_cast<int>(std::ceil(size_m.y / cell_m)) + 1);
    buckets_.resize(static_cast<size_t>(nx_) * nz_);
  }

  void Insert(glm::vec2 p, float radius) {
    const glm::ivec2 c = Coord(p);
    buckets_[static_cast<size_t>(c.y) * nx_ + c.x].push_back({p, radius});
  }

  // True when `p`'s crown circle would overlap one already placed -- the
  // multi-class rule r_p + r_other. Both radii are the instances' own, already
  // multiplied by their instance scale.
  bool Conflicts(glm::vec2 p, float radius) const {
    const glm::ivec2 c = Coord(p);
    for (int dz = -1; dz <= 1; ++dz) {
      for (int dx = -1; dx <= 1; ++dx) {
        const int x = c.x + dx, z = c.y + dz;
        if (x < 0 || z < 0 || x >= nx_ || z >= nz_) continue;
        for (const Placed& q : buckets_[static_cast<size_t>(z) * nx_ + x]) {
          const float r = radius + q.radius;
          const glm::vec2 d = p - q.pos;
          if (glm::dot(d, d) < r * r) return true;
        }
      }
    }
    return false;
  }

 private:
  struct Placed {
    glm::vec2 pos;
    float radius;
  };

  glm::ivec2 Coord(glm::vec2 p) const {
    const glm::vec2 local = (p - origin_) / cell_;
    return glm::ivec2(
        std::clamp(static_cast<int>(std::floor(local.x)), 0, nx_ - 1),
        std::clamp(static_cast<int>(std::floor(local.y)), 0, nz_ - 1));
  }

  glm::vec2 origin_;
  float cell_;
  int nx_ = 0, nz_ = 0;
  std::vector<std::vector<Placed>> buckets_;
};

}  // namespace

bool AnyCoverage(const TerrainQuery& query, const FoliageGenParams& params) {
  if (params.size_m.x <= 0.0f || params.size_m.y <= 0.0f ||
      params.mask_texel_m <= 0.0f) {
    return false;
  }
  const int nx =
      std::max(1, static_cast<int>(std::ceil(params.size_m.x / params.mask_texel_m)));
  const int nz =
      std::max(1, static_cast<int>(std::ceil(params.size_m.y / params.mask_texel_m)));

  for (int j = 0; j <= nz; ++j) {
    const float z = params.origin_m.y +
                    static_cast<float>(j) * params.mask_texel_m;
    for (int i = 0; i <= nx; ++i) {
      const float x = params.origin_m.x +
                      static_cast<float>(i) * params.mask_texel_m;
      if (query.CoverageAt(x, z) > 0.0f) return true;
    }
  }
  return false;
}

float SlopeDegreesAt(const TerrainQuery& query, float x, float z) {
  const float s = kSlopeProbeM;
  const float dx =
      (query.HeightAt(x + s, z) - query.HeightAt(x - s, z)) / (2.0f * s);
  const float dz =
      (query.HeightAt(x, z + s) - query.HeightAt(x, z - s)) / (2.0f * s);
  return glm::degrees(std::atan(std::sqrt(dx * dx + dz * dz)));
}

float RemapClump(float raw_noise, float lo, float hi) {
  // FastNoiseLite is ~[-1, 1]; rescale before applying the window so lo/hi read
  // as "fraction of the noise range", which is how they are tuned.
  const float n01 = (raw_noise + 1.0f) * 0.5f;
  if (hi <= lo) return n01 >= hi ? 1.0f : 0.0f;
  return std::clamp((n01 - lo) / (hi - lo), 0.0f, 1.0f);
}

FoliageField GenerateFoliage(const ForestType& forest,
                             const TerrainQuery& query,
                             const FoliageGenParams& params) {
  if (!forest.Valid()) {
    spdlog::error(
        "GenerateFoliage: malformed ForestType (empty models/layers, or a "
        "layer's model slice is out of range); placing nothing");
    return {};
  }
  if (params.size_m.x <= 0.0f || params.size_m.y <= 0.0f ||
      params.mask_texel_m <= 0.0f) {
    spdlog::error("GenerateFoliage: degenerate region {}x{} m at texel {} m",
                  params.size_m.x, params.size_m.y, params.mask_texel_m);
    return {};
  }

  // The largest crown circle any single instance can present: a model's radius
  // at the top of its scale range (the edge ramp only ever shrinks it).
  float max_scaled_radius = 0.0f;
  for (const FoliageModel& m : forest.models)
    max_scaled_radius = std::max(max_scaled_radius, m.radius_m * m.scale_range.y);
  if (max_scaled_radius <= 0.0f) {
    // Also the guard against a consumer that forgot to measure: radii are
    // filled in from the models' real bounds, and a table of zeroes would
    // otherwise place a forest with no spacing at all.
    spdlog::error("GenerateFoliage: every model has radius <= 0; placing nothing");
    return {};
  }

  const DepthField depth_field =
      BuildDepthField(query, params.origin_m, params.size_m,
                      params.mask_texel_m, forest.noise, params.seed);

  FastNoiseLite clump;
  clump.SetSeed(static_cast<int>(params.seed));
  clump.SetNoiseType(FastNoiseLite::NoiseType_OpenSimplex2);
  clump.SetFractalType(FastNoiseLite::FractalType_FBm);
  clump.SetFractalOctaves(std::max(1, forest.noise.clump_octaves));
  clump.SetFrequency(1.0f / std::max(0.01f, forest.noise.clump_wavelength_m));

  FoliageField field;
  field.origin_m = params.origin_m;
  field.cells_x =
      std::max(1, static_cast<int>(std::ceil(params.size_m.x / kFoliageCellSizeM)));
  field.cells_z =
      std::max(1, static_cast<int>(std::ceil(params.size_m.y / kFoliageCellSizeM)));
  field.cells.resize(static_cast<size_t>(field.cells_x) * field.cells_z);
  field.cell_y.assign(static_cast<size_t>(field.cells_x) * field.cells_z,
                      CellYBounds::Empty());

  SpacingGrid spacing(params.origin_m, params.size_m, 2.0f * max_scaled_radius);

  const glm::vec2 lo = params.origin_m;
  const glm::vec2 hi = params.origin_m + params.size_m;
  const float water_floor = query.WaterLevelM() + kWaterClearanceM;

  for (size_t li = 0; li < forest.layers.size(); ++li) {
    const FoliageLayer& layer = forest.layers[li];

    // Grid indices are ABSOLUTE (derived from world position, not from the
    // region's corner), so regenerating a sub-region reproduces the same
    // instances instead of a shifted lattice.
    const int gx0 = static_cast<int>(std::floor(lo.x / layer.grid_m));
    const int gz0 = static_cast<int>(std::floor(lo.y / layer.grid_m));
    const int gx1 = static_cast<int>(std::floor(hi.x / layer.grid_m));
    const int gz1 = static_cast<int>(std::floor(hi.y / layer.grid_m));

    for (int gz = gz0; gz <= gz1; ++gz) {
      for (int gx = gx0; gx <= gx1; ++gx) {
        HashStream rng(FoliageHash(params.seed, static_cast<uint32_t>(li), gx, gz));

        const float px =
            (static_cast<float>(gx) + rng.Next01()) * layer.grid_m;
        const float pz =
            (static_cast<float>(gz) + rng.Next01()) * layer.grid_m;
        if (px < lo.x || px > hi.x || pz < lo.y || pz > hi.y) continue;

        // 1. Coverage -- the cheapest test, and the one that rejects most.
        if (query.CoverageAt(px, pz) <= 0.0f) continue;

        // 2. Density roll: how many of this layer belong at this depth, times
        //    the clump field that turns a uniform carpet into thickets.
        const float depth = depth_field.DepthAt(px, pz);
        const float density =
            layer.density.Evaluate(depth) *
            RemapClump(clump.GetNoise(px, pz), forest.noise.clump_lo,
                       forest.noise.clump_hi);
        if (rng.Next01() >= density) continue;

        // 3. Model pick, weighted by each model's own depth response -- this is
        //    what shifts the species mix from edge to interior. Done before the
        //    terrain tests because the spacing radius is the model's.
        float total_w = 0.0f;
        for (uint16_t k = 0; k < layer.model_count; ++k) {
          const FoliageModel& m = forest.models[layer.first_model + k];
          total_w += m.weight * m.depth.Evaluate(depth);
        }
        if (total_w <= 0.0f) continue;

        float pick = rng.Next01() * total_w;
        // Defaults to the last model: floating-point accumulation can leave a
        // hair of weight unconsumed, and falling off the end must land on a
        // real model rather than on the layer's first by accident.
        uint16_t model_index =
            static_cast<uint16_t>(layer.first_model + layer.model_count - 1);
        for (uint16_t k = 0; k < layer.model_count; ++k) {
          const FoliageModel& m = forest.models[layer.first_model + k];
          pick -= m.weight * m.depth.Evaluate(depth);
          if (pick <= 0.0f) {
            model_index = static_cast<uint16_t>(layer.first_model + k);
            break;
          }
        }
        const FoliageModel& model = forest.models[model_index];

        // 4. Scale, rolled BEFORE the spacing test because the exclusion circle
        //    is this instance's own, not its model's: a 0.85-scaled oak really
        //    does take less room than a 1.15-scaled one, and spacing every oak
        //    by the largest it could have been thins the stand for nothing. The
        //    edge ramp is what makes trees near the boundary read as younger
        //    rather than merely sparser -- and, now that it feeds the radius, a
        //    young edge tree correctly lets its neighbours stand closer.
        float scale = rng.NextRange(model.scale_range.x, model.scale_range.y);
        if (layer.edge_scale < 1.0f && layer.edge_scale_depth_m > 0.0f) {
          const float t = std::clamp(depth / layer.edge_scale_depth_m, 0.0f, 1.0f);
          scale *= layer.edge_scale + (1.0f - layer.edge_scale) * t;
        }

        // 5. Terrain.
        const float ground = query.HeightAt(px, pz);
        if (ground <= water_floor) continue;
        if (SlopeDegreesAt(query, px, pz) > layer.max_slope_deg) continue;

        // 6. Spacing, against every layer placed so far as well as this one:
        //    this instance's crown circle may not overlap any already placed.
        //    The SUM of the two radii, not the max -- max only kept the trunks
        //    apart, so a bush cleared a 3 m radius from an oak whose crown
        //    reached 7 m and ended up standing under it. Because the canopy
        //    layer is declared first it claims its circles before anything
        //    else, which is what keeps the undergrowth out of the shade.
        const glm::vec2 p(px, pz);
        const float radius = model.radius_m * scale;
        if (spacing.Conflicts(p, radius)) continue;

        FoliageInstance inst;
        inst.position = glm::vec3(px, ground - kGroundSinkM, pz);
        inst.yaw = rng.Next01() * kTwoPi;
        inst.scale = scale;
        inst.model = model_index;
        inst.layer = static_cast<uint16_t>(li);

        spacing.Insert(p, radius);

        const int ci = field.CellIndexAt(px, pz);
        field.cells[static_cast<size_t>(ci)].push_back(inst);
        CellYBounds& yb = field.cell_y[static_cast<size_t>(ci)];
        yb.min_y = std::min(yb.min_y, inst.position.y);
        yb.max_y =
            std::max(yb.max_y, inst.position.y + model.height_m * inst.scale);
      }
    }
  }

  return field;
}

}  // namespace badlands::foliage
