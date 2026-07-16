#pragma once

// Builds a renderable terrain mesh from mapgen output by tessellating the block
// grid. Game-side geometry (no engine/GPU deps) — the output is a flat vertex
// buffer for the kTerrainBlend vertex layout, ready to drop into a
// StaticTexturedMeshComponent and render with MaterialLibrary::TerrainBlend.

#include <cstdint>
#include <vector>

#include "mapgen/field2d.hpp"

namespace badlands {

// A non-indexed kTerrainBlend triangle list. Each vertex is 14 floats:
// pos(3) + normal(3) + blend_weights(4) + layer_indices(4, u32 bitcast).
struct TerrainMesh {
  std::vector<float> vertices;
  uint32_t vertex_count = 0;

  static constexpr int kFloatsPerVertex = 14;
};

struct TerrainMeshParams {
  int subdiv = 4;  // subgrid cells per 10 m block edge (subdiv+1 nodes/edge)
};

// Tessellate the block grid of a heightmap (world meters) + per-sample biome
// map into a terrain mesh. Each 10 m block becomes a subdiv x subdiv subgrid;
// each subgrid cell is X-split into 4 triangles meeting at the cell centre.
// Vertex heights are bilinearly sampled from the heightmap, normals from
// central differences, and blend_weights/layer_indices from the biome map:
// one-hot per vertex, with each triangle's three vertices sharing the same
// (<=4) layer-index set so the flat-interpolated indices stay consistent while
// the weights interpolate.
TerrainMesh BuildTerrainMesh(const mapgen::Field2D<float>& heightmap,
                             const mapgen::Field2D<uint8_t>& biome,
                             const TerrainMeshParams& params = {});

}  // namespace badlands
