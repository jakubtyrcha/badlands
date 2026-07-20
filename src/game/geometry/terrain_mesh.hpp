#pragma once

// Terrain queries over mapgen output. Game-side and pure CPU (no GPU deps; the
// only engine header is the header-only Ray struct). SampleHeight/NormalAt
// answer height + surface-normal queries against the heightmap (also used by the
// terrain cluster-LOD build for crack-free shared vertices), RaycastTerrain does
// mouse picking, and PackU8x4 packs the u8x4 vertex attributes.

#include <cstdint>

#include <glm/glm.hpp>

#include "engine/core/ray.hpp"
#include "mapgen/field2d.hpp"

namespace badlands {

// Bilinear terrain height (world meters) at world position (wx, wz) meters.
// Takes WORLD coordinates and converts to heightmap sample space internally
// (world_m / kMetersPerSample), so that convention lives in exactly one place.
// Clamps to the map bounds, so off-map queries return the edge height.
float SampleHeight(const mapgen::Field2D<float>& heightmap, float wx, float wz);

// Surface normal at world position (wx, wz) from central height differences,
// one sample (kMetersPerSample world meters) apart. A pure function of position
// (SampleHeight clamps at the edges), so two callers sampling the same world
// position get bitwise-identical normals — the terrain cluster DAG relies on
// that for crack-free shared vertices.
glm::vec3 NormalAt(const mapgen::Field2D<float>& heightmap, float wx, float wz);

// Pack four u8 into one float slot (matches the Uint8x4 / Unorm8x4 attributes
// of the terrain vertex layouts).
float PackU8x4(uint8_t a, uint8_t b, uint8_t c, uint8_t d);

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

}  // namespace badlands
