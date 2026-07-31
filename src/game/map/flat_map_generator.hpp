#pragma once

// A featureless flat plain: every node is Plains, at one height, everywhere.
// No seed, no noise, no lake, no biome variety.
//
// It exists so a world that wants NO terrain can SAY so. Before it, make_world
// copied the symbolic map unconditionally, which meant any world staged at the
// origin was standing on that map's central Lake -- invisible while nothing
// consulted the terrain, and a floor the navmesh would refuse to walk on the
// moment anything did. "This world has no terrain" is now a statement rather
// than an accident of which biome happens to sit under the spawn point.
//
// Deliberately the symbolic map's lattice: the placement grid must span the
// whole map (make_world's static_assert), so both generators are 256 m square.
// Pure CPU -- MapData + the mapgen Biome enum, no engine, no GPU.

#include "game/map/map_data.hpp"

namespace badlands {

class FlatMapGenerator : public MapGenerator {
 public:
  MapData Generate() const override;

  static constexpr int kNodesPerSide = 65;
  static constexpr float kSpacingM = 4.0f;
  static constexpr float kMapSizeM = (kNodesPerSide - 1) * kSpacingM;  // 256 m
  // Ground plane. Zero, not the symbolic map's 0.5 -- there is no water here
  // for a ground height to be measured against.
  static constexpr float kHeightM = 0.0f;
};

}  // namespace badlands
