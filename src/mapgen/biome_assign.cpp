#include "mapgen/biome_assign.hpp"

#include <algorithm>
#include <cmath>

#include "mapgen/parallel.hpp"

namespace badlands::mapgen {

// Whittaker-style split: elevation picks the row, moisture the column.
//   high elevation           -> Hills (regardless of moisture)
//   low elevation, wet / dry  -> Lake / Swamp
//   mid elevation, wet / dry  -> Forest / Plains
Biome classify_biome(float elevation, float moisture, const MapgenConfig& cfg) {
  if (elevation >= cfg.elev_high) return Biome::Hills;
  if (elevation < cfg.elev_lake) {
    return moisture >= cfg.moisture_wet ? Biome::Lake : Biome::Swamp;
  }
  return moisture >= cfg.moisture_wet ? Biome::Forest : Biome::Plains;
}

BiomeMap assign_biomes(const MapgenConfig& cfg, const Voronoi& voronoi,
                       const Fields& fields) {
  BiomeMap m;
  m.cell_biome.assign(voronoi.cell_count(), Biome::Plains);

  for (int id = 0; id < voronoi.cell_count(); ++id) {
    const glm::vec2 s = voronoi.seeds[id];
    const int sx =
        std::clamp(static_cast<int>(std::lround(s.x)), 0, cfg.width - 1);
    const int sy =
        std::clamp(static_cast<int>(std::lround(s.y)), 0, cfg.height - 1);
    m.cell_biome[id] =
        classify_biome(fields.elevation.at(sx, sy),
                       fields.moisture.at(sx, sy), cfg);
  }

  m.pixel = Field2D<uint8_t>(cfg.width, cfg.height);
  parallel_tiles(
      cfg.width, cfg.height, 64, [] { return 0; },
      [&](int&, int x0, int y0, int x1, int y1) {
        for (int y = y0; y < y1; ++y) {
          for (int x = x0; x < x1; ++x) {
            m.pixel.at(x, y) = static_cast<uint8_t>(
                m.cell_biome[voronoi.cell.at(x, y)]);
          }
        }
      });

  return m;
}

}  // namespace badlands::mapgen
