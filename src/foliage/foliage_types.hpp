#pragma once

// The foliage generator's OUTPUT: placed instances, bucketed into a regular
// grid of world-space cells.
//
// Cells exist so a renderer can coarse-cull on the CPU (one frustum test per
// cell, ~1000x fewer tests than per instance) before handing the survivors to
// the GPU-driven per-instance cull. They are NOT a streaming unit: generation
// is whole-map and batch, and a cell is just how the result is filed.
//
// Pure CPU: glm + <vector>. No engine, no Aabb, no GPU types -- which is why
// a cell stores only its Y extent (see CellYBounds) rather than a full box.

#include <cstdint>
#include <vector>

#include <glm/glm.hpp>

namespace badlands::foliage {

// World size of one foliage cell, in metres. Compile-time by design: the cell
// grid is an internal filing scheme, and a runtime cell size would mean every
// consumer's cull loop has to handle a size it did not choose.
//
// 32 m is a balance -- small enough that a cell's contents are mostly on-screen
// together (so culling a cell is meaningful), large enough that a 512 m map is
// 16x16 = 256 cells rather than thousands of near-empty ones.
inline constexpr float kFoliageCellSizeM = 32.0f;

// One placed piece of foliage. `model` indexes ForestType::models, NOT any
// mesh/asset registry -- resolving a model index to actual geometry is the
// consumer's business (see src/game/visual/forest_catalog.hpp for badlands').
struct FoliageInstance {
  glm::vec3 position{0.0f};  // world; y is the ground it stands on
  float yaw = 0.0f;          // radians about +Y
  float scale = 1.0f;        // uniform
  uint16_t model = 0;        // index into ForestType::models
  uint16_t layer = 0;        // index into ForestType::layers (debug/filtering)
};

// A cell's vertical extent, in world metres. Only Y: a cell's XZ bounds are
// implicit from its index and the cell size, so storing them would be storing
// arithmetic. Keeping the box out of here is also what keeps engine's Aabb --
// and therefore the engine -- out of this library.
//
// An EMPTY cell has min_y > max_y (the Empty() sentinel); callers must skip it
// rather than culling against a degenerate box.
struct CellYBounds {
  float min_y = 0.0f;
  float max_y = 0.0f;

  static constexpr CellYBounds Empty() { return {1e30f, -1e30f}; }
  constexpr bool empty() const { return min_y > max_y; }
};

// A whole map's worth of placed foliage, filed by cell.
//
// `cells` and `cell_y` are parallel, dense and row-major: index
// `cz * cells_x + cx`. Dense rather than sparse because the grid is small
// (a 512 m map is 256 cells) and a dense index makes the consumer's cull a
// flat loop with no lookup.
struct FoliageField {
  glm::vec2 origin_m{0.0f};  // world XZ of cell (0,0)'s minimum corner
  int cells_x = 0;
  int cells_z = 0;
  std::vector<std::vector<FoliageInstance>> cells;
  std::vector<CellYBounds> cell_y;

  bool empty() const { return cells_x <= 0 || cells_z <= 0; }

  int CellIndex(int cx, int cz) const { return cz * cells_x + cx; }

  // Cell coordinates containing world XZ, CLAMPED to the grid. Clamping (not
  // failing) because callers index with positions that can sit exactly on the
  // far edge, where the floor lands one cell past the end.
  glm::ivec2 CellCoordAt(float x, float z) const;

  // Flat cell index containing world XZ, clamped as above. -1 if the field is
  // empty.
  int CellIndexAt(float x, float z) const;

  // Minimum corner of cell (cx, cz) in world XZ.
  glm::vec2 CellOrigin(int cx, int cz) const {
    return origin_m + glm::vec2(static_cast<float>(cx), static_cast<float>(cz)) *
                          kFoliageCellSizeM;
  }

  // Total placed instances across every cell.
  size_t InstanceCount() const;
};

}  // namespace badlands::foliage
