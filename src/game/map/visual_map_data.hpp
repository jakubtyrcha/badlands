#pragma once

// VISUAL map data: what the render path reads, and the only map type that knows
// about mapgen::Cover.
//
// Its counterpart is game/map/map_data.hpp, which is the GAMEPLAY map -- biome
// coverage for walkability, movement cost, habitat and animal spawning, read by
// the simulation across the C ABI. The two are kept apart on purpose. They
// describe the same ground and answer different questions, and a single type
// carrying both would put two meanings on one `slice(k)`.
//
// (MapData is not yet named GameplayMapData. Renaming it touches badlands_game
// and the sim's plumbing, which is out of scope here; the split is what
// mattered, and the rename follows when that app is next opened.)
//
// Both feed the terrain mesh builder through TerrainLattice, which is deliberately
// vocabulary-free -- see game/map/terrain_lattice.hpp.
//
// Pure CPU: <vector> plus the header-only Cover enum. No engine, no GPU.

#include <cstddef>
#include <cstdint>
#include <vector>

#include "game/map/terrain_lattice.hpp"
#include "mapgen/cover.hpp"
#include "mapgen/terrain_class.hpp"

namespace badlands {

// A lattice of nodes_x * nodes_z nodes, `spacing_m` apart. Node (i, j) sits at
// world (i * spacing_m, j * spacing_m).
//
// Cover is stored as ONE class per node rather than as coverage slices. The
// gameplay map keeps slices because blend weights are what a movement cost
// wants to interpolate; here the class is a categorical label feeding foliage
// and a debug tint, and the GROUND MATERIAL does not come from it at all --
// that is derived from slope, curvature and soil at full resolution
// (mapview/ground_splat.hpp). Storing weights nothing reads would be storing
// weights nothing reads.
class VisualMapData {
 public:
  VisualMapData() = default;
  VisualMapData(int nodes_x, int nodes_z, float spacing_m);

  int nodes_x() const { return nodes_x_; }
  int nodes_z() const { return nodes_z_; }
  float spacing_m() const { return spacing_m_; }
  float size_x_m() const { return static_cast<float>(nodes_x_ - 1) * spacing_m_; }
  float size_z_m() const { return static_cast<float>(nodes_z_ - 1) * spacing_m_; }
  bool empty() const { return nodes_x_ <= 0 || nodes_z_ <= 0; }

  // How this ground was carved -- which rock and ground materials to paint
  // with, which props to scatter. Per map, as the sources emit it.
  mapgen::TerrainClass terrain_class() const { return terrain_class_; }
  void set_terrain_class(mapgen::TerrainClass c) { terrain_class_ = c; }

  float height(int i, int j) const;
  float& mutable_height(int i, int j);
  mapgen::Cover cover(int i, int j) const;
  void set_cover(int i, int j, mapgen::Cover c);

  // Bilinear, clamped to the map edge.
  float HeightAt(float wx, float wz) const;
  // Nearest -- the class is categorical, so there is nothing between two of them.
  mapgen::Cover CoverAt(float wx, float wz) const;

  // How much of the neighbourhood around (wx, wz) is `c`, in [0, 1]: bilinear
  // over the four surrounding nodes' indicator.
  //
  // A hard 0/1 test would be the obvious thing and would give the foliage
  // generator a blocky forest edge. This reproduces what bilinear coverage
  // slices gave before cover became one class per node, so a boundary still
  // ramps.
  float CoverFractionAt(float wx, float wz, mapgen::Cover c) const;

  // The vocabulary-free view the terrain mesh builder takes. Borrows this
  // object's storage, so it must not outlive it.
  TerrainLattice Lattice() const;

 private:
  std::size_t index(int i, int j) const {
    return static_cast<std::size_t>(j) * static_cast<std::size_t>(nodes_x_) +
           static_cast<std::size_t>(i);
  }

  int nodes_x_ = 0;
  int nodes_z_ = 0;
  float spacing_m_ = 1.0f;
  mapgen::TerrainClass terrain_class_ = mapgen::TerrainClass::Unknown;
  std::vector<float> height_;
  std::vector<uint8_t> cover_;
};

}  // namespace badlands
