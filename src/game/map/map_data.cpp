#include "game/map/map_data.hpp"

#include <algorithm>
#include <cassert>
#include <cmath>

namespace badlands {

namespace {

// Bilinear setup for a world coordinate on a lattice axis: the lower node index
// and the [0,1) fraction to the next node, clamped so off-map reads return the
// edge value instead of going out of bounds.
struct AxisLerp {
  int i0 = 0;
  int i1 = 0;
  float t = 0.0f;
};

AxisLerp AxisSetup(float world, float spacing, int nodes) {
  AxisLerp a;
  if (nodes <= 1) return a;
  const float g = std::clamp(world / spacing, 0.0f,
                             static_cast<float>(nodes - 1));
  const float f = std::floor(g);
  a.i0 = static_cast<int>(f);
  a.i1 = std::min(a.i0 + 1, nodes - 1);
  a.t = g - f;
  return a;
}

}  // namespace

mapgen::Biome BiomeWeights::Dominant() const {
  int best = 0;
  for (int b = 1; b < kBiomeSliceCount; ++b) {
    if (w[b] > w[best]) best = b;  // strict: ties keep the lower index
  }
  return static_cast<mapgen::Biome>(best);
}

float BiomeWeights::Sum() const {
  float s = 0.0f;
  for (float v : w) s += v;
  return s;
}

MapData::MapData(int nodes_x, int nodes_z, float spacing_m)
    : nodes_x_(std::max(0, nodes_x)),
      nodes_z_(std::max(0, nodes_z)),
      spacing_m_(spacing_m > 0.0f ? spacing_m : 1.0f) {
  const std::size_t n =
      static_cast<std::size_t>(nodes_x_) * static_cast<std::size_t>(nodes_z_);
  height_.assign(n, 0.0f);
  for (auto& s : slices_) s.assign(n, 0);
}

float MapData::height(int i, int j) const {
  assert(i >= 0 && j >= 0 && i < nodes_x_ && j < nodes_z_);
  return height_[index(i, j)];
}

float& MapData::mutable_height(int i, int j) {
  assert(i >= 0 && j >= 0 && i < nodes_x_ && j < nodes_z_);
  return height_[index(i, j)];
}

uint8_t MapData::slice(int biome, int i, int j) const {
  assert(biome >= 0 && biome < kBiomeSliceCount);
  assert(i >= 0 && j >= 0 && i < nodes_x_ && j < nodes_z_);
  return slices_[biome][index(i, j)];
}

uint8_t& MapData::mutable_slice(int biome, int i, int j) {
  assert(biome >= 0 && biome < kBiomeSliceCount);
  assert(i >= 0 && j >= 0 && i < nodes_x_ && j < nodes_z_);
  return slices_[biome][index(i, j)];
}

BiomeWeights MapData::WeightsAtNode(int i, int j) const {
  BiomeWeights out;
  if (empty()) return out;
  const int ci = std::clamp(i, 0, nodes_x_ - 1);
  const int cj = std::clamp(j, 0, nodes_z_ - 1);
  float sum = 0.0f;
  for (int b = 0; b < kBiomeSliceCount; ++b) {
    const float v = static_cast<float>(slices_[b][index(ci, cj)]);
    out.w[b] = v;
    sum += v;
  }
  if (sum > 0.0f) {
    for (float& v : out.w) v /= sum;
  } else {
    out.w.fill(0.0f);  // no coverage: leave all-zero rather than inventing one
  }
  return out;
}

float MapData::HeightAt(float wx, float wz) const {
  if (empty()) return 0.0f;
  const AxisLerp ax = AxisSetup(wx, spacing_m_, nodes_x_);
  const AxisLerp az = AxisSetup(wz, spacing_m_, nodes_z_);
  const float h00 = height_[index(ax.i0, az.i0)];
  const float h10 = height_[index(ax.i1, az.i0)];
  const float h01 = height_[index(ax.i0, az.i1)];
  const float h11 = height_[index(ax.i1, az.i1)];
  const float a = h00 + (h10 - h00) * ax.t;
  const float b = h01 + (h11 - h01) * ax.t;
  return a + (b - a) * az.t;
}

BiomeWeights MapData::BiomesAt(float wx, float wz) const {
  BiomeWeights out;
  if (empty()) return out;
  const AxisLerp ax = AxisSetup(wx, spacing_m_, nodes_x_);
  const AxisLerp az = AxisSetup(wz, spacing_m_, nodes_z_);
  float sum = 0.0f;
  for (int b = 0; b < kBiomeSliceCount; ++b) {
    const auto& s = slices_[b];
    const float s00 = static_cast<float>(s[index(ax.i0, az.i0)]);
    const float s10 = static_cast<float>(s[index(ax.i1, az.i0)]);
    const float s01 = static_cast<float>(s[index(ax.i0, az.i1)]);
    const float s11 = static_cast<float>(s[index(ax.i1, az.i1)]);
    const float a = s00 + (s10 - s00) * ax.t;
    const float c = s01 + (s11 - s01) * ax.t;
    const float v = a + (c - a) * az.t;
    out.w[b] = v;
    sum += v;
  }
  if (sum > 0.0f) {
    for (float& v : out.w) v /= sum;
  } else {
    out.w.fill(0.0f);
  }
  return out;
}

mapgen::Biome MapData::DominantBiomeAt(float wx, float wz) const {
  return BiomesAt(wx, wz).Dominant();
}

}  // namespace badlands
