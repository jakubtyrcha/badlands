#pragma once

// Builds the biome-weight SPLAT raster the cluster terrain samples by world XZ
// to pick its per-biome materials.
//
// Why a texture and not vertex attributes: the cluster-LOD DAG decimates
// vertices, so vertex-carried blend weights would have to be averaged by the
// simplifier and biome-boundary resolution would follow the LOD cut. A splat is
// LOD-independent -- the coarsest cluster still gets full-resolution biome
// detail -- and it leaves the cluster vertex format and DAG build untouched.
//
// Layout matches terrain_layers.wesl's 8 dense weight slots: slot index IS the
// mapgen::Biome enum value, which is also the texture-array layer index (the
// same convention assets/materials/terrain_biomes.json and LoadTerrainArrays
// use). Slots 6-7 are always zero today (kBiomeCount == 6).
//
// Pure CPU (mapgen + <vector>), so it is unit-testable without a GPU.

#include <cstdint>
#include <vector>

#include "mapgen/field2d.hpp"

namespace badlands {

// Blur radius applied to the one-hot biome weights before the top-2 cull, in
// world metres. Softens the raster's one-texel staircase into a transition band
// wide enough to read as a blend rather than a jagged edge.
inline constexpr float kBiomeBlendM = 3.0f;

// Two RGBA8 rasters holding 8 biome weight slots, sized to the input field.
// slots0 = slots 0..3, slots1 = slots 4..7; both are tightly packed RGBA8 and
// `width * height * 4` bytes long. The weights at a texel sum to exactly 255.
struct BiomeSplat {
  int width = 0;
  int height = 0;
  std::vector<uint8_t> slots0;
  std::vector<uint8_t> slots1;

  bool empty() const { return width <= 0 || height <= 0; }
};

// Builds the splat from a per-texel biome raster (values are mapgen::Biome enum
// values). `texel_m` is the world size of one texel, which is what turns
// kBiomeBlendM into a pixel radius. An empty field returns an empty splat.
//
// Only the two strongest weights per texel survive: the fragment shader's cost
// is proportional to the number of non-zero layers, and bilinear filtering of a
// 2-weight texel can already produce 4 at a biome triple point.
BiomeSplat BuildBiomeSplat(const mapgen::Field2D<uint8_t>& biome, float texel_m);

}  // namespace badlands
