#pragma once

// Terrain geometry + picking over the frozen MapData contract. Game-side and
// pure CPU (no GPU deps; the only engine header is the header-only Ray struct).
//
// This is the SIMPLE terrain builder: one X-split quad per lattice cell (four
// triangles meeting at the cell centre), emitting the kTerrainBlend vertex
// layout ready to drop into a StaticTexturedMeshComponent and render with
// MaterialLibrary::TerrainBlend. It is deliberately replaceable -- a decimating
// / nanite-style builder will take its place behind the same MapData input.

#include <cstdint>
#include <vector>

#include <glm/glm.hpp>

#include "engine/core/ray.hpp"
#include "game/map/map_data.hpp"

namespace badlands {

// Up to 4 (layer, weight) pairs for one vertex -- exactly what the kTerrainBlend
// attributes carry. `weights` are u8 summing to exactly 255, or all zero where
// the map has no coverage. Unused slots carry weight 0: slot index 0 is a REAL
// layer (Biome::Lake), so weight-0 discipline is what keeps padding harmless.
struct VertexBlend {
  uint8_t layers[4]{};
  uint8_t weights[4]{};
};

// Keep the 4 strongest biomes, renormalize them, and quantize sum-preserving
// (largest remainder) so the packed weights total exactly 255. Ties resolve to
// the lower biome index, so the result is deterministic.
//
// The 4-pair cap is a vertex-FORMAT limit (Uint8x4 + Unorm8x4), which is why it
// lives here and not in MapData -- the map exposes every slice.
VertexBlend ResolveVertexBlend(const BiomeWeights& weights);

// First intersection of `ray` with the terrain surface (the map's bilinear
// heightmap). Marches until the ray passes below the surface, then bisects.
// Returns false when the ray never goes under the terrain within the map extent
// (pointing at the sky, or leaving the map) -- callers treat that as "no hover".
//
// The surface is the bilinear heightmap, not the tessellated mesh, so a hit can
// differ from the rendered triangles by the tessellation error -- fine for
// picking, not for physics.
bool RaycastTerrain(const MapData& map, const Ray& ray, glm::vec3& out_hit);

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
  // Lattice-cell sub-region to tessellate (for chunked terrain). Vertices are in
  // ABSOLUTE world meters regardless of the region. cells_x/z < 0 means "to the
  // map edge" from cell_x0/z0. Default = the whole map.
  int cell_x0 = 0;
  int cell_z0 = 0;
  int cells_x = -1;
  int cells_z = -1;
};

// Tessellate a lattice-cell region of `map` into a terrain mesh.
//
// Each cell becomes four triangles meeting at its centre. CORNER vertices read
// the lattice directly (height + biome slices). The CENTRE vertex is DERIVED:
// its height, normal, and biome weights are the average of the cell's four
// corner vertices -- so the four triangles agree by construction and no
// independent centre sample can disagree with its corners.
//
// Because a vertex's blend is a pure function of its lattice position, every
// triangle sharing a vertex sees the identical normalized blend; there is
// nothing to reconcile across triangles and so no harsh seams.
TerrainMesh BuildTerrainMesh(const MapData& map,
                             const TerrainMeshParams& params = {});

}  // namespace badlands
