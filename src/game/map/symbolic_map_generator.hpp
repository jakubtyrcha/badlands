#pragma once

// A fixed, hand-authored greybox map (no seed, fully deterministic): a 5x5
// symbolic biome grid over a 256x256 m square, produced as MapData.
//
// Generation works at BLOCK-EDGE density: the lattice nodes are the corners of
// the 4 m gameplay blocks (65x65 nodes at 4 m spacing). Biomes are grown as
// per-biome coverage channels rather than hard ids, so borders come out blended
// and rounded:
//
//   1. seed one channel per biome from the symbolic tile grid
//   2. diffusion smoothing, iterated: blur -> argmax -> re-seed one-hot. This is
//      a discrete curvature flow -- it shrinks convex bulges and fills concave
//      notches, so corners round off into arcs with no 45/90-degree bias while
//      straight runs stay straight.
//   3. one final per-channel low-pass, quantized into MapData's 8-bit slices.
//      Its radius is what sets how wide the rendered biome transition is.
//
// The terrain is flat at 0.5 m except the lake, carved into a basin reaching
// 8 m below the water surface (y = 0) at the point farthest from its shore,
// easing down via easeInOutSine of the normalized distance-to-shore -- so the
// bank crosses the water level *inside* the lake (no water-free gap at the shore).
//
// Symbolic grid (row 0 = north / z = 0, col 0 = west / x = 0):
//     W W S S S      W woodland -> Biome::Forest
//     W L L L S      S swamp    -> Biome::Swamp
//     W P L L S      L lake     -> Biome::Lake  (lake bottom)
//     W P P P S      P plain    -> Biome::Plains
//     W W W W W

#include "game/map/map_data.hpp"

namespace badlands {

class SymbolicMapGenerator : public MapGenerator {
 public:
  MapData Generate() const override;

  static constexpr int kTilesPerSide = 5;
  // The gameplay block grid, and the block-EDGE lattice generation runs on.
  static constexpr int kBlocksPerSide = 64;
  static constexpr int kNodesPerSide = kBlocksPerSide + 1;  // block corners
  static constexpr float kSpacingM = 4.0f;  // block-edge lattice spacing
  static constexpr float kMapSizeM = kBlocksPerSide * kSpacingM;   // 256 m
  static constexpr float kTileSizeM = kMapSizeM / kTilesPerSide;   // 51.2 m

  static constexpr float kFlatHeight = 0.5f;   // terrain plane
  static constexpr float kWaterLevel = 0.0f;   // lake surface y
  static constexpr float kLakeBottom = -8.0f;  // deepest, 8 m below water

  // Region shaping: blur -> argmax -> re-seed, this many times, at this blur
  // radius (in lattice cells). Each pass rounds corners by roughly its radius.
  static constexpr int kShapingPasses = 3;
  static constexpr int kShapingBlurCells = 1;
  // Final low-pass on the coverage channels: sets the rendered transition width.
  // A radius-1 box repeated 3x has sigma ~1.4 cells (~6 m at 4 m spacing), so
  // the blend spans roughly 20 m -- wide enough to kill the staircase, narrow
  // enough that a 51 m tile still reads as its own biome. Radius 2 washes the
  // whole map into one gradient.
  static constexpr int kShadingBlurCells = 1;
};

// easeInOutSine mapping [0,1] -> [0,1]:  -(cos(pi*t) - 1) / 2.
// Exposed so the carve profile can be unit-tested directly.
float EaseInOutSine(float t);

}  // namespace badlands
