#include "game/map/symbolic_map_generator.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <vector>

namespace badlands {

namespace {

using mapgen::Biome;

constexpr float kPi = 3.14159265358979323846f;
constexpr int kN = SymbolicMapGenerator::kNodesPerSide;

// 5x5 symbolic grid: row 0 = north (z = 0), col 0 = west (x = 0).
constexpr const char* kGrid[SymbolicMapGenerator::kTilesPerSide] = {
    "WWSSS",
    "WLLLS",
    "WPLLS",
    "WPPPS",
    "WWWWW",
};

Biome SymbolToBiome(char c) {
  switch (c) {
    case 'W': return Biome::Forest;   // woodland
    case 'S': return Biome::Swamp;
    case 'L': return Biome::Lake;
    case 'P': return Biome::Plains;
    default:  return Biome::Plains;
  }
}

// One coverage channel per biome over the node lattice.
using Channels = std::array<std::vector<float>, kBiomeSliceCount>;

int Idx(int i, int j) { return j * kN + i; }

// The symbolic tile a lattice node falls in -> its biome.
Biome NodeTileBiome(int i, int j) {
  const float wx = static_cast<float>(i) * SymbolicMapGenerator::kSpacingM;
  const float wz = static_cast<float>(j) * SymbolicMapGenerator::kSpacingM;
  const int col = std::min(SymbolicMapGenerator::kTilesPerSide - 1,
                           static_cast<int>(wx / SymbolicMapGenerator::kTileSizeM));
  const int row = std::min(SymbolicMapGenerator::kTilesPerSide - 1,
                           static_cast<int>(wz / SymbolicMapGenerator::kTileSizeM));
  return SymbolToBiome(kGrid[row][col]);
}

int ArgMaxAt(const Channels& ch, int i, int j) {
  int best = 0;
  for (int b = 1; b < kBiomeSliceCount; ++b) {
    if (ch[b][Idx(i, j)] > ch[best][Idx(i, j)]) best = b;
  }
  return best;
}

// Separable box blur, `passes` times (3 ~= Gaussian), clamped at the border so
// the map rim is not darkened toward zero. Grids are tiny (65^2 x 6 channels),
// so the naive clamped-window sum is plenty and is obviously correct.
void BlurChannel(std::vector<float>& ch, int radius, int passes) {
  if (radius <= 0) return;
  std::vector<float> tmp(ch.size());
  for (int p = 0; p < passes; ++p) {
    // Horizontal.
    for (int j = 0; j < kN; ++j) {
      for (int i = 0; i < kN; ++i) {
        float sum = 0.0f;
        for (int d = -radius; d <= radius; ++d) {
          sum += ch[Idx(std::clamp(i + d, 0, kN - 1), j)];
        }
        tmp[Idx(i, j)] = sum / static_cast<float>(2 * radius + 1);
      }
    }
    // Vertical.
    for (int j = 0; j < kN; ++j) {
      for (int i = 0; i < kN; ++i) {
        float sum = 0.0f;
        for (int d = -radius; d <= radius; ++d) {
          sum += tmp[Idx(i, std::clamp(j + d, 0, kN - 1))];
        }
        ch[Idx(i, j)] = sum / static_cast<float>(2 * radius + 1);
      }
    }
  }
}

void BlurAll(Channels& ch, int radius, int passes) {
  for (auto& c : ch) BlurChannel(c, radius, passes);
}

void SeedOneHotFromArgMax(Channels& ch) {
  std::vector<int> labels(static_cast<std::size_t>(kN) * kN);
  for (int j = 0; j < kN; ++j)
    for (int i = 0; i < kN; ++i) labels[Idx(i, j)] = ArgMaxAt(ch, i, j);
  for (int b = 0; b < kBiomeSliceCount; ++b)
    for (int n = 0; n < kN * kN; ++n)
      ch[b][n] = (labels[n] == b) ? 1.0f : 0.0f;
}

// Two-pass chamfer distance from every node satisfying `inside` to the nearest
// node that does not. Approximate Euclidean (orthogonal 1, diagonal sqrt(2));
// units are lattice cells, which cancel because the caller normalizes.
std::vector<float> DistanceToBoundary(const std::vector<uint8_t>& inside) {
  const float kInf = std::numeric_limits<float>::max();
  const float a = 1.0f, b = std::sqrt(2.0f);
  std::vector<float> d(inside.size());
  for (std::size_t n = 0; n < inside.size(); ++n) d[n] = inside[n] ? kInf : 0.0f;

  auto relax = [&](int i, int j, int ni, int nj, float cost) {
    if (ni < 0 || nj < 0 || ni >= kN || nj >= kN) return;
    d[Idx(i, j)] = std::min(d[Idx(i, j)], d[Idx(ni, nj)] + cost);
  };
  for (int j = 0; j < kN; ++j)
    for (int i = 0; i < kN; ++i) {
      relax(i, j, i - 1, j, a);
      relax(i, j, i, j - 1, a);
      relax(i, j, i - 1, j - 1, b);
      relax(i, j, i + 1, j - 1, b);
    }
  for (int j = kN - 1; j >= 0; --j)
    for (int i = kN - 1; i >= 0; --i) {
      relax(i, j, i + 1, j, a);
      relax(i, j, i, j + 1, a);
      relax(i, j, i + 1, j + 1, b);
      relax(i, j, i - 1, j + 1, b);
    }
  return d;
}

}  // namespace

float EaseInOutSine(float t) {
  t = std::clamp(t, 0.0f, 1.0f);
  return -(std::cos(kPi * t) - 1.0f) * 0.5f;
}

MapData SymbolicMapGenerator::Generate() const {
  MapData map(kNodesPerSide, kNodesPerSide, kSpacingM);
  map.set_water_level_m(kWaterLevel);

  // 1. Seed one coverage channel per biome from the symbolic tile grid.
  Channels ch;
  for (auto& c : ch) c.assign(static_cast<std::size_t>(kN) * kN, 0.0f);
  for (int j = 0; j < kN; ++j)
    for (int i = 0; i < kN; ++i)
      ch[static_cast<int>(NodeTileBiome(i, j))][Idx(i, j)] = 1.0f;

  // 2. Diffusion smoothing: blur -> argmax -> re-seed, rounding the borders.
  for (int p = 0; p < kShapingPasses; ++p) {
    BlurAll(ch, kShapingBlurCells, /*passes=*/3);
    SeedOneHotFromArgMax(ch);
  }

  // Hard labels after shaping -- these drive the lake basin (and, downstream,
  // the water surface). Taken before the shading blur so the region is crisp.
  std::vector<uint8_t> is_lake(static_cast<std::size_t>(kN) * kN, 0);
  for (int j = 0; j < kN; ++j)
    for (int i = 0; i < kN; ++i)
      is_lake[Idx(i, j)] =
          ArgMaxAt(ch, i, j) == static_cast<int>(Biome::Lake) ? 1 : 0;

  // 3. Final low-pass -> quantize into the map's 8-bit slices. Normalized per
  //    node so the slices always encode a blend ratio.
  BlurAll(ch, kShadingBlurCells, /*passes=*/3);
  for (int j = 0; j < kN; ++j) {
    for (int i = 0; i < kN; ++i) {
      float sum = 0.0f;
      for (int b = 0; b < kBiomeSliceCount; ++b) sum += ch[b][Idx(i, j)];
      if (sum <= 0.0f) {
        // No coverage anywhere (cannot happen with a seeded grid, but keep the
        // map well-formed rather than all-zero).
        map.mutable_slice(static_cast<int>(NodeTileBiome(i, j)), i, j) = 255;
        continue;
      }
      for (int b = 0; b < kBiomeSliceCount; ++b) {
        const float f = ch[b][Idx(i, j)] / sum;
        map.mutable_slice(b, i, j) = static_cast<uint8_t>(
            std::lround(std::clamp(f, 0.0f, 1.0f) * 255.0f));
      }
    }
  }

  // 4. Flat plane, then carve the lake basin: the height eases from the plane at
  //    the shore down to the water-relative floor at the point farthest in.
  const std::vector<float> dist = DistanceToBoundary(is_lake);
  float d_max = 0.0f;
  for (std::size_t n = 0; n < dist.size(); ++n)
    if (is_lake[n]) d_max = std::max(d_max, dist[n]);

  for (int j = 0; j < kN; ++j) {
    for (int i = 0; i < kN; ++i) {
      float h = kFlatHeight;
      if (is_lake[Idx(i, j)] && d_max > 0.0f) {
        const float e = EaseInOutSine(dist[Idx(i, j)] / d_max);
        h = kFlatHeight + (kLakeBottom - kFlatHeight) * e;
      }
      map.mutable_height(i, j) = h;
    }
  }

  return map;
}

}  // namespace badlands
