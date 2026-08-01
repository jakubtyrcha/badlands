#include "mapview/lake_surface.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <deque>

namespace badlands {

namespace {

constexpr std::array<glm::ivec2, 4> kNeighbours = {
    glm::ivec2{1, 0}, glm::ivec2{-1, 0}, glm::ivec2{0, 1}, glm::ivec2{0, -1}};

}  // namespace

std::vector<glm::vec3> BuildLakeSurfaceTriangles(const mapgen::MapArtifacts& art,
                                                 float world_size_m) {
  std::vector<glm::vec3> tris;
  const int w = art.lake_id.width, h = art.lake_id.height;
  if (w <= 0 || h <= 0 || art.lakes.empty()) return tris;
  if (art.heightmap.width != w || art.heightmap.height != h) return tris;
  const float s = world_size_m / static_cast<float>(w);
  if (s <= 0.0f) return tris;
  // The skirt grows one TEXEL at a time but is capped in METRES, so on a coarse
  // map (say --resolution 50 --size 500x500 -> 10 m texels) the very first step
  // would already exceed kMaxSkirtM and every lake would be clipped flush to its
  // own boundary, with no skirt at all and no diagnostic. Allow at least one
  // ring: one texel of overlap is the least that can hide an edge.
  const float skirt_cap_m = std::max(kMaxSkirtM, s);

  // owner[i] = which lake covers texel i (-1 = none). Seeded with the lake
  // texels themselves, then grown outward by a multi-source BFS so each skirt
  // texel is claimed by its NEAREST lake -- two lakes at different levels
  // cannot both claim the same ground, which would put two water planes on one
  // patch of terrain.
  std::vector<int32_t> owner(static_cast<size_t>(w) * h, -1);
  std::vector<float> dist(static_cast<size_t>(w) * h, 0.0f);
  std::deque<int> queue;
  for (int i = 0; i < w * h; ++i) {
    const int32_t id = art.lake_id.data[i];
    if (id >= 0 && id < static_cast<int32_t>(art.lakes.size())) {
      owner[i] = id;
      queue.push_back(i);
    }
  }

  while (!queue.empty()) {
    const int i = queue.front();
    queue.pop_front();
    const int32_t id = owner[i];
    const float level = art.lakes[id].level_m;
    const int x = i % w, z = i / w;

    // This texel is the buried boundary: keep it, but expand no further. Lake
    // texels themselves always expand (their bed is below the level by
    // definition).
    if (art.lake_id.data[i] < 0 && art.heightmap.data[i] >= level + kBurialM) {
      continue;
    }

    for (const glm::ivec2& d : kNeighbours) {
      const int nx = x + d.x, nz = z + d.y;
      if (nx < 0 || nz < 0 || nx >= w || nz >= h) continue;
      const int ni = nz * w + nx;
      if (owner[ni] >= 0) continue;                  // already water or claimed
      const float nd = dist[i] + s;
      if (nd > skirt_cap_m) continue;                // past the cap
      if (art.heightmap.data[ni] < level) continue;  // would float over voids
      owner[ni] = id;
      dist[ni] = nd;
      queue.push_back(ni);
    }
  }

  // Two triangles per maximal RUN of same-owner texels along a row, not per
  // texel. A lake surface is flat and single-coloured, so every texel in a run
  // is coplanar and the merged quad covers exactly the same ground with the
  // same winding and the same Y -- the geometry is identical, only the
  // tessellation is coarser.
  //
  // Per-texel emission does not survive a fine grid: a 2048^2 map produced
  // 1.38M triangles and a 379 MB vertex buffer, over Dawn's 268 MB default
  // maxBufferSize, which invalidated the command buffer and blanked the frame.
  // Runs are hundreds of texels long on a real lake, so this is a ~1000x cut.
  // Merging along one axis keeps it a single pass with no bookkeeping; the
  // second axis would need a rectangle decomposition for far less.
  for (int z = 0; z < h; ++z) {
    int x = 0;
    while (x < w) {
      const int32_t id = owner[static_cast<size_t>(z) * w + x];
      if (id < 0) {
        ++x;
        continue;
      }
      int run_end = x + 1;
      while (run_end < w && owner[static_cast<size_t>(z) * w + run_end] == id)
        ++run_end;
      const float y = art.lakes[id].level_m;
      const float x0 = static_cast<float>(x) * s;
      const float x1 = static_cast<float>(run_end) * s;
      const float z0 = static_cast<float>(z) * s, z1 = z0 + s;
      const glm::vec3 a(x0, y, z0), b(x1, y, z0), c(x1, y, z1), d(x0, y, z1);
      // CCW seen from +Y.
      tris.push_back(a);
      tris.push_back(d);
      tris.push_back(c);
      tris.push_back(a);
      tris.push_back(c);
      tris.push_back(b);
      x = run_end;
    }
  }
  return tris;
}

}  // namespace badlands
