#pragma once

// Terrain geometry + queries over mapgen output. Game-side and pure CPU (no GPU
// deps; the only engine header is the header-only Ray struct) — BuildTerrainMesh
// emits a flat vertex buffer for the kTerrainBlend vertex layout, ready to drop
// into a StaticTexturedMeshComponent and render with
// MaterialLibrary::TerrainBlend, while SampleHeight/RaycastTerrain answer
// height + picking queries against the same heightmap.

#include <cstdint>
#include <vector>

#include <glm/glm.hpp>

#include "engine/core/ray.hpp"
#include "mapgen/field2d.hpp"

namespace badlands {

// Bilinear terrain height (world meters) at world position (wx, wz) meters.
// Takes WORLD coordinates and converts to heightmap sample space internally
// (world_m / kMetersPerSample), so that convention lives in exactly one place.
// Clamps to the map bounds, so off-map queries return the edge height.
float SampleHeight(const mapgen::Field2D<float>& heightmap, float wx, float wz);

// First intersection of `ray` with the terrain surface defined by `heightmap`.
//
// Marches the ray sampling SampleHeight until it passes below the surface, then
// bisects to refine. Returns false when the ray never goes under the terrain
// within the map's extent (pointing at the sky, or leaving the map) — callers
// should treat that as "no hover".
//
// The surface is the bilinear heightmap, not the tessellated mesh, so a hit can
// differ from the rendered triangles by the tessellation error (sub-decimeter at
// the subdivisions used here) — fine for picking, not for physics.
bool RaycastTerrain(const mapgen::Field2D<float>& heightmap, const Ray& ray,
                    glm::vec3& out_hit);

// A kTerrainBlend triangle mesh. Each vertex is 8 floats: pos(3) + normal(3) +
// layer_indices(Uint8x4 packed as 1 float) + blend_weights(Unorm8x4 packed as 1
// float). Non-indexed when `indices` is empty; otherwise `indices` addresses
// shared vertices (DrawIndexed).
struct TerrainMesh {
  std::vector<float> vertices;
  std::vector<uint32_t> indices;
  uint32_t vertex_count = 0;

  static constexpr int kFloatsPerVertex = 8;
};

struct TerrainMeshParams {
  int subdiv = 4;  // subgrid cells per 10 m block edge (subdiv+1 nodes/edge)
  // Block sub-region to tessellate (for chunked terrain). Vertices are in
  // ABSOLUTE world meters regardless of the region. blocks_x/z < 0 means "to
  // the map edge" from block_x0/z0. Default = the whole map.
  int block_x0 = 0;
  int block_z0 = 0;
  int blocks_x = -1;
  int blocks_z = -1;
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
