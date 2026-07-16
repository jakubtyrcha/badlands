#include "mapgen/heightmap.hpp"

#include <cmath>

#include <glm/glm.hpp>

#include "mapgen/parallel.hpp"

namespace badlands::mapgen {

float compose_height(const MapgenConfig& cfg, Biome biome, float elevation,
                     float ridged, float fine) {
  // Continuous relief: smooth base, plus ridged/rocky detail in hills, minus a
  // basin under low (lake/swamp) areas. The elevation field is smooth so basin
  // walls are gentle slopes; the ridged term is folded in BEFORE quantization
  // so hills cross more terrace levels (rugged) than flat plains.
  float relief = elevation * cfg.height_scale_m;
  if (biome == Biome::Hills) relief += (ridged - 0.5f) * cfg.hills_ridge_m;
  // Guard elev_lake <= 0 (no lake band -> no basin, consistent with
  // classify_biome): smoothstep would otherwise divide by -elev_lake -> NaN.
  const float lowness = cfg.elev_lake > 0.0f
                            ? glm::smoothstep(cfg.elev_lake, 0.0f, elevation)
                            : 0.0f;
  relief -= cfg.cavity_depth_m * lowness;

  // Snap to terrace levels: this is what gives the map its mesa/plateau
  // structure and makes the flat sections sit at distinct heights (the ledges
  // between terraces become section boundaries).
  float terraced = relief;
  if (cfg.terrace_step_m > 0.0f) {
    terraced =
        std::round(relief / cfg.terrace_step_m) * cfg.terrace_step_m;
  }

  // Small per-biome variation on top so terraces don't look dead-flat. Kept
  // below section_step_m so it never splits a terrace into extra sections.
  const float variation =
      (fine - 0.5f) * cfg.variation_amp_m[static_cast<int>(biome)];
  return terraced + variation;
}

Field2D<float> compose_heightmap(const MapgenConfig& cfg, const Fields& fields,
                                 const BiomeMap& biomes) {
  Field2D<float> h(cfg.width, cfg.height);
  parallel_tiles(
      cfg.width, cfg.height, 64, [] { return 0; },
      [&](int&, int x0, int y0, int x1, int y1) {
        for (int y = y0; y < y1; ++y) {
          for (int x = x0; x < x1; ++x) {
            const auto biome = static_cast<Biome>(biomes.pixel.at(x, y));
            h.at(x, y) = compose_height(cfg, biome, fields.elevation.at(x, y),
                                        fields.ridged.at(x, y),
                                        fields.fine.at(x, y));
          }
        }
      });
  return h;
}

}  // namespace badlands::mapgen
