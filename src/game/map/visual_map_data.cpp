#include "game/map/visual_map_data.hpp"

#include <algorithm>
#include <cmath>

namespace badlands {

VisualMapData::VisualMapData(int nodes_x, int nodes_z, float spacing_m)
    : nodes_x_(std::max(0, nodes_x)),
      nodes_z_(std::max(0, nodes_z)),
      spacing_m_(spacing_m > 0.0f ? spacing_m : 1.0f) {
  const std::size_t n =
      static_cast<std::size_t>(nodes_x_) * static_cast<std::size_t>(nodes_z_);
  height_.assign(n, 0.0f);
  cover_.assign(n, static_cast<uint8_t>(mapgen::Cover::Unknown));
}

float VisualMapData::height(int i, int j) const {
  if (empty()) return 0.0f;
  return height_[index(std::clamp(i, 0, nodes_x_ - 1),
                       std::clamp(j, 0, nodes_z_ - 1))];
}

float& VisualMapData::mutable_height(int i, int j) {
  return height_[index(std::clamp(i, 0, nodes_x_ - 1),
                       std::clamp(j, 0, nodes_z_ - 1))];
}

mapgen::Cover VisualMapData::cover(int i, int j) const {
  if (empty()) return mapgen::Cover::Unknown;
  return static_cast<mapgen::Cover>(cover_[index(
      std::clamp(i, 0, nodes_x_ - 1), std::clamp(j, 0, nodes_z_ - 1))]);
}

void VisualMapData::set_cover(int i, int j, mapgen::Cover c) {
  if (empty()) return;
  cover_[index(std::clamp(i, 0, nodes_x_ - 1),
               std::clamp(j, 0, nodes_z_ - 1))] = static_cast<uint8_t>(c);
}

float VisualMapData::HeightAt(float wx, float wz) const {
  return Lattice().HeightAt(wx, wz);
}

mapgen::Cover VisualMapData::CoverAt(float wx, float wz) const {
  if (empty()) return mapgen::Cover::Unknown;
  return static_cast<mapgen::Cover>(Lattice().ClassAt(wx, wz));
}

float VisualMapData::CoverFractionAt(float wx, float wz, mapgen::Cover c) const {
  if (empty()) return 0.0f;
  const float gx =
      std::clamp(wx / spacing_m_, 0.0f, static_cast<float>(nodes_x_ - 1));
  const float gz =
      std::clamp(wz / spacing_m_, 0.0f, static_cast<float>(nodes_z_ - 1));
  const float ffx = std::floor(gx), ffz = std::floor(gz);
  const int i0 = static_cast<int>(ffx), j0 = static_cast<int>(ffz);
  const int i1 = std::min(i0 + 1, nodes_x_ - 1);
  const int j1 = std::min(j0 + 1, nodes_z_ - 1);
  const float tx = gx - ffx, tz = gz - ffz;

  const auto ind = [&](int i, int j) {
    return cover(i, j) == c ? 1.0f : 0.0f;
  };
  const float a = ind(i0, j0) + (ind(i1, j0) - ind(i0, j0)) * tx;
  const float b = ind(i0, j1) + (ind(i1, j1) - ind(i0, j1)) * tx;
  return a + (b - a) * tz;
}

TerrainLattice VisualMapData::Lattice() const {
  TerrainLattice l;
  l.nodes_x = nodes_x_;
  l.nodes_z = nodes_z_;
  l.spacing_m = spacing_m_;
  l.height = height_.data();
  // Already one class per node -- nothing to derive, which is why this needs no
  // TerrainLatticeStorage while the slice-based gameplay map does.
  l.class_id = cover_.data();
  l.palette = mapgen::kCoverPalette.data();
  l.palette_count = mapgen::kCoverCount;
  return l;
}

}  // namespace badlands
