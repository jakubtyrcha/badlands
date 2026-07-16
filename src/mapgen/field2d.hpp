#pragma once

#include <cassert>
#include <cstddef>
#include <vector>

namespace badlands::mapgen {

// A dense row-major 2D grid of T. Used for the heightmap (float), the biome
// map (Biome), the voronoi cell-id map (int), and the block grid (Block).
// Deliberately minimal — the mapgen passes read/write disjoint regions in
// parallel, so there is no synchronization here.
template <typename T>
struct Field2D {
  int width = 0;
  int height = 0;
  std::vector<T> data;

  Field2D() = default;
  Field2D(int w, int h, T init = T{})
      : width(w), height(h), data(static_cast<size_t>(w) * h, init) {}

  T& at(int x, int y) { return data[index(x, y)]; }
  const T& at(int x, int y) const { return data[index(x, y)]; }

  bool in_bounds(int x, int y) const {
    return x >= 0 && y >= 0 && x < width && y < height;
  }
  size_t size() const { return data.size(); }

 private:
  size_t index(int x, int y) const {
    assert(x >= 0 && y >= 0 && x < width && y < height);
    return static_cast<size_t>(y) * width + x;
  }
};

}  // namespace badlands::mapgen
