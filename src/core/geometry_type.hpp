#pragma once

namespace badlands {

// Geometry type determines how the material samples textures and computes
// normals
enum class GeometryType {
  kTexturedMesh,    // UV-mapped mesh geometry (2D texture sampling)
  kSphericalMesh,   // Spherical mesh geometry (cubemap sampling, sphere-derived normals)
  kTerrainBlend,    // Per-vertex blend weights + texture_2d_array layer lookup
  kTerrainCluster   // Per-vertex baked color + metadata; multi-range indexed submesh
};

}  // namespace badlands
