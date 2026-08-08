#pragma once

// THE SEAM BETWEEN A MAP AND THE TERRAIN MESH BUILDER.
//
// The cluster-LOD builder used to take a `const MapData&`, which meant it saw
// that map's whole classification vocabulary. There are now two vocabularies
// and they must not mix:
//
//   * mapgen::Biome  -- GAMEPLAY. Walkability, movement cost, habitat, animal
//                       spawning. Frozen across the C ABI (BiomeAt).
//   * mapgen::Cover  -- RENDER. What grows here, for shading and foliage.
//
// A builder taking a whole map would have to pick one, and `slice(k)` would
// silently mean different things in badlands_game and badlands_mapview.
//
// So it takes THIS instead: a lattice, heights, one already-resolved class byte
// per node, and a palette to colour it with. It has no idea what the byte means
// and cannot ask -- which is exactly the property that lets one builder serve
// both maps. See
// docs/superpowers/specs/2026-08-07-detailed-patch-rendering-design.md.
//
// PLAIN POINTERS, NOT AN INTERFACE. The build samples these millions of times
// per patch; a virtual call or a std::function per sample would be paid on
// every one. The lattice borrows -- whatever produced it must outlive the
// build.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

#include "mapgen/cover.hpp"

namespace badlands {

struct TerrainLattice {
  int nodes_x = 0;
  int nodes_z = 0;
  float spacing_m = 1.0f;

  const float* height = nullptr;      // nodes_x * nodes_z, row-major
  const uint8_t* class_id = nullptr;  // one resolved class per node
  const mapgen::Rgb* palette = nullptr;
  int palette_count = 0;

  bool empty() const { return nodes_x <= 0 || nodes_z <= 0; }
  float size_x_m() const { return static_cast<float>(nodes_x - 1) * spacing_m; }
  float size_z_m() const { return static_cast<float>(nodes_z - 1) * spacing_m; }

  int clamp_i(int i) const { return std::clamp(i, 0, nodes_x - 1); }
  int clamp_j(int j) const { return std::clamp(j, 0, nodes_z - 1); }

  float HeightAtNode(int i, int j) const {
    return height[static_cast<size_t>(clamp_j(j)) * nodes_x + clamp_i(i)];
  }

  uint8_t ClassAtNode(int i, int j) const {
    return class_id[static_cast<size_t>(clamp_j(j)) * nodes_x + clamp_i(i)];
  }

  // Bilinear, clamped at the edges, so an off-map sample returns the border
  // value rather than failing. A pure function of world position: two callers
  // sampling one point get bitwise-identical results, which is what keeps
  // shared cluster-boundary vertices crack-free.
  // THE ARITHMETIC FORM IS LOAD-BEARING, not just the algebra. It reproduces
  // MapData::HeightAt operation for operation -- floor, then a + (b - a) * t
  // rather than a*(1-t) + b*t -- because the two forms differ in the last bits
  // and the cluster DAG is pinned against a bitwise golden hash. An
  // algebraically identical rewrite moves every vertex normal by an ulp and
  // fails that pin for no reason anyone could act on.
  float HeightAt(float wx, float wz) const {
    if (empty()) return 0.0f;
    const float gx = std::clamp(wx / spacing_m, 0.0f,
                                static_cast<float>(nodes_x - 1));
    const float gz = std::clamp(wz / spacing_m, 0.0f,
                                static_cast<float>(nodes_z - 1));
    const float ffx = std::floor(gx), ffz = std::floor(gz);
    const int i0 = static_cast<int>(ffx), j0 = static_cast<int>(ffz);
    const int i1 = std::min(i0 + 1, nodes_x - 1);
    const int j1 = std::min(j0 + 1, nodes_z - 1);
    const float tx = gx - ffx, tz = gz - ffz;
    const float h00 = HeightAtNode(i0, j0), h10 = HeightAtNode(i1, j0);
    const float h01 = HeightAtNode(i0, j1), h11 = HeightAtNode(i1, j1);
    const float a = h00 + (h10 - h00) * tx;
    const float b = h01 + (h11 - h01) * tx;
    return a + (b - a) * tz;
  }

  // AREA-WEIGHTED MAJORITY over the four surrounding nodes, ties to the lower
  // class id. Never an interpolated value: there is nothing between "grass" and
  // "rock", so the result is always a class that exists.
  //
  // Not nearest-node, which is the obvious choice and the wrong one. This
  // reproduces, bit for bit, the rule it replaced -- bilinear over one-hot
  // coverage slices, then argmax -- and it has to, because the class byte
  // reaches meshoptimizer as a vertex attribute weight, so changing the filter
  // moves real geometry along every class boundary. The weights are derived
  // from the four corners rather than stored, which is why dropping the slices
  // cost nothing here.
  uint8_t ClassAt(float wx, float wz) const {
    if (empty()) return 0;
    const float gx = std::clamp(wx / spacing_m, 0.0f,
                                static_cast<float>(nodes_x - 1));
    const float gz = std::clamp(wz / spacing_m, 0.0f,
                                static_cast<float>(nodes_z - 1));
    const float ffx = std::floor(gx), ffz = std::floor(gz);
    const int i0 = static_cast<int>(ffx), j0 = static_cast<int>(ffz);
    const int i1 = std::min(i0 + 1, nodes_x - 1);
    const int j1 = std::min(j0 + 1, nodes_z - 1);
    const float tx = gx - ffx, tz = gz - ffz;

    const uint8_t c[4] = {ClassAtNode(i0, j0), ClassAtNode(i1, j0),
                          ClassAtNode(i0, j1), ClassAtNode(i1, j1)};
    // Only a corner's own class can score, so at most four candidates. Visiting
    // them in ASCENDING id with a strict comparison is what puts ties on the
    // lower id, matching the rule this replaced.
    uint8_t sorted[4] = {c[0], c[1], c[2], c[3]};
    std::sort(sorted, sorted + 4);

    uint8_t best = 0;
    float best_w = 0.0f;
    for (int k = 0; k < 4; ++k) {
      if (k > 0 && sorted[k] == sorted[k - 1]) continue;
      const uint8_t b = sorted[k];
      // The same arithmetic the slice bilinear used, on the same 0/255 values,
      // so the comparison lands on the same side of every tie.
      const float s00 = c[0] == b ? 255.0f : 0.0f;
      const float s10 = c[1] == b ? 255.0f : 0.0f;
      const float s01 = c[2] == b ? 255.0f : 0.0f;
      const float s11 = c[3] == b ? 255.0f : 0.0f;
      const float a = s00 + (s10 - s00) * tx;
      const float d = s01 + (s11 - s01) * tx;
      const float v = a + (d - a) * tz;
      if (v > best_w) {
        best_w = v;
        best = b;
      }
    }
    return best;
  }

  mapgen::Rgb ColorFor(uint8_t id) const {
    if (!palette || palette_count <= 0) return {0, 0, 0};
    return palette[id < palette_count ? id : 0];
  }
};

// Owns whatever a lattice had to derive. A map that stores coverage SLICES has
// no per-node class byte lying around, so one is materialised here and the
// lattice points at it -- which is why this must outlive any build using it.
struct TerrainLatticeStorage {
  std::vector<uint8_t> class_id;
  TerrainLattice lattice;

  // ALWAYS go through this rather than touching `lattice` directly. The view
  // holds a raw pointer into `class_id`, and any assignment or relocation of
  // this object leaves that pointer describing the wrong buffer. Re-seating it
  // here is one instruction and removes a whole class of dangling view.
  const TerrainLattice& View() {
    lattice.class_id = class_id.data();
    return lattice;
  }
};

}  // namespace badlands
