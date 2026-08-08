#pragma once

// ============================================================================
// FROZEN MAP-DATA CONTRACT
// ============================================================================
//
// THIS IS THE GAMEPLAY MAP. Its vocabulary is mapgen::Biome -- walkability,
// movement cost, habitat, animal spawning -- and the simulation reads it across
// the C ABI (badlands_sim.hpp's BiomeAt), which is what makes that vocabulary
// expensive to change.
//
// The RENDER path has its own map, game/map/visual_map_data.hpp, speaking
// mapgen::Cover. Keeping them apart is deliberate: they describe the same
// ground and answer different questions, and one type carrying both would put
// two meanings on a single `slice(k)`.
//
// (The name has not caught up yet -- this is GameplayMapData in everything but
// spelling. Renaming it touches badlands_game and the sim's plumbing, which is
// why the split landed first and the rename follows.)
//
// The map is DATA, not behaviour: per-biome coverage slices + a heightmap, both
// sampled on one shared regular lattice. Everything downstream reads only this.
//
// Two consumers, both served by what is below:
//   * GAME LOGIC       -- queries biome + height at an arbitrary 2D world
//                         position (HeightAt / BiomesAt / DominantBiomeAt).
//   * TERRAIN VISUAL   -- through TerrainLattice (game/map/terrain_lattice.hpp),
//     LAYER              which is vocabulary-free, so the mesh builder never
//                        sees a Biome.
//
// STABILITY: this shape is the contract. What sits on either side of it is
// explicitly NOT part of it and is expected to be replaced:
//   * how the data is GENERATED (procedural, authored, streamed, ...),
//   * how a MESH is built from it (a decimating / nanite-style builder will
//     replace the simple triangulator),
//   * how slices become MATERIALS (multi-texture per-biome blending will
//     replace the one-colour-per-biome debug material).
// None of those may require a change here.
//
// Deliberate design choices that keep it stable:
//   * 8-bit slices. A weight is a blend ratio, not a measurement, and 1/255 is
//     already finer than the Unorm8x4 vertex attribute that carries it.
//   * ALL slices are exposed -- never "the top N". The 4-pair-per-vertex cap is
//     a vertex-FORMAT limit, so it belongs to the mesh builder, not to the map.
//     Keeping it out of here is what lets richer material blending land later.
//   * Resolution-agnostic (`spacing_m`), so a coarse authored map and a fine
//     procedural one are the same type.
//   * No triangles, no materials, no render types anywhere in this header.
//
// Pure CPU: <array>/<vector> + the header-only mapgen Biome enum. No engine,
// no GPU, no glm needed -- so it compiles into the Catch2-only test targets.

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "game/map/terrain_lattice.hpp"
#include "mapgen/biomes.hpp"

namespace badlands {

// One coverage slice per biome; slice index == mapgen::Biome value == the
// terrain material's texture-array layer index.
inline constexpr int kBiomeSliceCount = mapgen::kBiomeCount;

// Normalized biome weights at a point. Sums to 1, or is all-zero where the map
// has no coverage at all (which callers must treat as "no data", not as Lake).
struct BiomeWeights {
  std::array<float, kBiomeSliceCount> w{};

  // Heaviest biome; ties resolve to the lower slice index (deterministic).
  mapgen::Biome Dominant() const;
  float Sum() const;
};

class MapData {
 public:
  MapData() = default;
  // A lattice of nodes_x * nodes_z nodes, `spacing_m` apart. Node (i, j) sits
  // at world (i * spacing_m, j * spacing_m), so the map spans
  // [0, (nodes-1) * spacing_m] on each axis. Heights start at 0, slices empty.
  MapData(int nodes_x, int nodes_z, float spacing_m);

  int nodes_x() const { return nodes_x_; }
  int nodes_z() const { return nodes_z_; }
  float spacing_m() const { return spacing_m_; }
  float size_x_m() const { return static_cast<float>(nodes_x_ - 1) * spacing_m_; }
  float size_z_m() const { return static_cast<float>(nodes_z_ - 1) * spacing_m_; }
  bool empty() const { return nodes_x_ <= 0 || nodes_z_ <= 0; }

  // World Y of the water plane. Map data (the map decides where water sits),
  // not a render setting.
  float water_level_m() const { return water_level_m_; }
  void set_water_level_m(float y) { water_level_m_ = y; }

  // --- Raw lattice access (terrain visual layer) ---
  float height(int i, int j) const;
  float& mutable_height(int i, int j);
  uint8_t slice(int biome, int i, int j) const;
  uint8_t& mutable_slice(int biome, int i, int j);
  // Normalized weights at a lattice NODE (no interpolation) -- what a mesh
  // builder wants for a vertex sitting exactly on a node.
  BiomeWeights WeightsAtNode(int i, int j) const;

  // --- Queries at an arbitrary 2D world position (game logic) ---
  // Bilinear, clamped to the map edge, so off-map queries return the border
  // value rather than failing.
  float HeightAt(float wx, float wz) const;
  BiomeWeights BiomesAt(float wx, float wz) const;
  mapgen::Biome DominantBiomeAt(float wx, float wz) const;

  // The vocabulary-free view the terrain mesh builder takes. Slices have to be
  // reduced to one class per node to build it, so the result OWNS that buffer
  // and the lattice inside it borrows from the storage, not from this map.
  TerrainLatticeStorage Lattice() const;

 private:
  std::size_t index(int i, int j) const {
    return static_cast<std::size_t>(j) * static_cast<std::size_t>(nodes_x_) +
           static_cast<std::size_t>(i);
  }

  int nodes_x_ = 0;
  int nodes_z_ = 0;
  float spacing_m_ = 1.0f;
  float water_level_m_ = 0.0f;
  std::vector<float> height_;                                   // nodes_x*nodes_z
  std::array<std::vector<uint8_t>, kBiomeSliceCount> slices_;   // each the same
};

// Abstract producer of map data. Concrete generators (symbolic/greybox now,
// procedural later) differ entirely behind this.
struct MapGenerator {
  virtual ~MapGenerator() = default;
  virtual MapData Generate() const = 0;
};

}  // namespace badlands
