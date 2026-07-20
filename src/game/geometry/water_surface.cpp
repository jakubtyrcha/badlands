#include "game/geometry/water_surface.hpp"

#include <array>

namespace badlands {

namespace {

// Neighbour offset per X-split triangle, in the order triangles are emitted
// around a cell: 0 = N (-z), 1 = E (+x), 2 = S (+z), 3 = W (-x). Triangle e
// spans the cell edge from corner e to corner e+1, so it faces kEdgeDirs[e].
constexpr std::array<glm::ivec2, 4> kEdgeDirs = {
    glm::ivec2{0, -1}, glm::ivec2{1, 0}, glm::ivec2{0, 1}, glm::ivec2{-1, 0}};

}  // namespace

std::vector<glm::vec3> BuildWaterSurfaceTriangles(const MapData& map) {
  std::vector<glm::vec3> tris;
  if (map.empty()) return tris;

  const int cells_x = map.nodes_x() - 1;
  const int cells_z = map.nodes_z() - 1;
  if (cells_x <= 0 || cells_z <= 0) return tris;

  const float s = map.spacing_m();
  const float y = map.water_level_m();

  // A cell is "lake" when the biome at its centre is Lake. The centre is the
  // bilinear average of the cell's 4 corners, matching how the terrain mesh
  // derives its centre vertex, so water and terrain agree on where the lake is.
  auto is_lake = [&](int cx, int cz) {
    if (cx < 0 || cz < 0 || cx >= cells_x || cz >= cells_z) return false;
    const float wx = (static_cast<float>(cx) + 0.5f) * s;
    const float wz = (static_cast<float>(cz) + 0.5f) * s;
    return map.DominantBiomeAt(wx, wz) == mapgen::Biome::Lake;
  };

  for (int cz = 0; cz < cells_z; ++cz) {
    for (int cx = 0; cx < cells_x; ++cx) {
      std::array<bool, 4> emit{};
      if (is_lake(cx, cz)) {
        emit = {true, true, true, true};
      } else {
        int lake_edges = 0;
        for (int e = 0; e < 4; ++e) {
          if (is_lake(cx + kEdgeDirs[e].x, cz + kEdgeDirs[e].y)) {
            emit[e] = true;
            ++lake_edges;
          }
        }
        // Perpendicular pair == exactly one of {N,S} is set (with 2 total).
        const bool perpendicular_pair = (lake_edges == 2) && (emit[0] != emit[2]);
        if (!perpendicular_pair) emit = {false, false, false, false};
      }

      const float x0 = static_cast<float>(cx) * s, x1 = x0 + s;
      const float z0 = static_cast<float>(cz) * s, z1 = z0 + s;
      // Corners in CCW order seen from +Y, matching kEdgeDirs.
      const glm::vec3 corner[4] = {{x0, y, z0}, {x1, y, z0},
                                   {x1, y, z1}, {x0, y, z1}};
      const glm::vec3 centre(x0 + 0.5f * s, y, z0 + 0.5f * s);

      for (int e = 0; e < 4; ++e) {
        if (!emit[e]) continue;
        tris.push_back(corner[e]);
        tris.push_back(centre);
        tris.push_back(corner[(e + 1) % 4]);
      }
    }
  }
  return tris;
}

}  // namespace badlands
