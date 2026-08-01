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
// a 3x3 neighbourhood instead of every instance placed so far. The grid's cell
// size is the LARGEST model radius in the forest, which is what makes 3x3
// sufficient: the pairwise rule is max(r_i, r_j), and that can never exceed the
// largest radius, so no conflicting instance can sit more than one cell away.
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

  // True when `p` is too close to something already placed, under the
  // multi-class rule max(r_p, r_other).
  bool Conflicts(glm::vec2 p, float radius) const {
    const glm::ivec2 c = Coord(p);
    for (int dz = -1; dz <= 1; ++dz) {
      for (int dx = -1; dx <= 1; ++dx) {
        const int x = c.x + dx, z = c.y + dz;
        if (x < 0 || z < 0 || x >= nx_ || z >= nz_) continue;
        for (const Placed& q : buckets_[static_cast<size_t>(z) * nx_ + x]) {
          const float r = std::max(radius, q.radius);
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

  float max_radius = 0.0f;
  for (const FoliageModel& m : forest.models)
    max_radius = std::max(max_radius, m.radius_m);
  if (max_radius <= 0.0f) {
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

  SpacingGrid spacing(params.origin_m, params.size_m, max_radius);

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

        // 4. Terrain.
        const float ground = query.HeightAt(px, pz);
        if (ground <= water_floor) continue;
        if (SlopeDegreesAt(query, px, pz) > layer.max_slope_deg) continue;

        // 5. Spacing, against every layer placed so far as well as this one.
        const glm::vec2 p(px, pz);
        if (spacing.Conflicts(p, model.radius_m)) continue;

        // Accepted. The edge ramp is what makes trees near the boundary read as
        // younger rather than merely sparser.
        float scale = rng.NextRange(model.scale_range.x, model.scale_range.y);
        if (layer.edge_scale < 1.0f && layer.edge_scale_depth_m > 0.0f) {
          const float t = std::clamp(depth / layer.edge_scale_depth_m, 0.0f, 1.0f);
          scale *= layer.edge_scale + (1.0f - layer.edge_scale) * t;
        }

        FoliageInstance inst;
        inst.position = glm::vec3(px, ground - kGroundSinkM, pz);
        inst.yaw = rng.Next01() * kTwoPi;
        inst.scale = scale;
        inst.model = model_index;
        inst.layer = static_cast<uint16_t>(li);

        spacing.Insert(p, model.radius_m);

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
