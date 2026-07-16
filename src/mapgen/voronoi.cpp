#include "mapgen/voronoi.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>

#include "mapgen/parallel.hpp"

namespace badlands::mapgen {

namespace {

constexpr int kTile = 64;

// Deterministic hash of (x, y, seed, salt) -> [0, 1).
float hash01(uint32_t x, uint32_t y, uint32_t seed, uint32_t salt) {
  uint32_t h = x * 374761393u + y * 668265263u + seed * 2246822519u +
               salt * 3266489917u;
  h = (h ^ (h >> 13)) * 1274126177u;
  h ^= h >> 16;
  return static_cast<float>(h & 0x00ffffffu) /
         static_cast<float>(0x01000000u);
}

}  // namespace

Voronoi build_voronoi(const MapgenConfig& cfg) {
  const float cell = std::max(1.0f, cfg.cell_size_m);
  const float jitter = std::clamp(cfg.seed_jitter, 0.0f, 1.0f);

  Voronoi v;
  // +1 so seeds extend one grid step past each edge (pixels near the border
  // still have neighbor seeds on all sides).
  v.cols = static_cast<int>(std::ceil(cfg.width / cell)) + 1;
  v.rows = static_cast<int>(std::ceil(cfg.height / cell)) + 1;
  v.seeds.resize(static_cast<size_t>(v.cols) * v.rows);

  for (int gy = 0; gy < v.rows; ++gy) {
    for (int gx = 0; gx < v.cols; ++gx) {
      // Jitter the seed within its grid cell, centered at 0.5.
      const float jx =
          0.5f + (hash01(gx, gy, cfg.seed, 1) - 0.5f) * jitter;
      const float jy =
          0.5f + (hash01(gx, gy, cfg.seed, 2) - 0.5f) * jitter;
      v.seeds[static_cast<size_t>(gy) * v.cols + gx] =
          glm::vec2((gx + jx) * cell, (gy + jy) * cell);
    }
  }

  v.cell = Field2D<int>(cfg.width, cfg.height);
  parallel_tiles(
      cfg.width, cfg.height, kTile, [] { return 0; },
      [&](int&, int x0, int y0, int x1, int y1) {
        for (int y = y0; y < y1; ++y) {
          for (int x = x0; x < x1; ++x) {
            const glm::vec2 p(x + 0.5f, y + 0.5f);
            const int gx = std::clamp(static_cast<int>(p.x / cell), 0,
                                      v.cols - 1);
            const int gy = std::clamp(static_cast<int>(p.y / cell), 0,
                                      v.rows - 1);
            float best = std::numeric_limits<float>::max();
            int best_id = gy * v.cols + gx;
            for (int dy = -1; dy <= 1; ++dy) {
              for (int dx = -1; dx <= 1; ++dx) {
                const int cx = gx + dx;
                const int cy = gy + dy;
                if (cx < 0 || cy < 0 || cx >= v.cols || cy >= v.rows) continue;
                const int id = cy * v.cols + cx;
                const glm::vec2 d = p - v.seeds[id];
                const float dist2 = d.x * d.x + d.y * d.y;
                if (dist2 < best) {
                  best = dist2;
                  best_id = id;
                }
              }
            }
            v.cell.at(x, y) = best_id;
          }
        }
      });

  return v;
}

}  // namespace badlands::mapgen
