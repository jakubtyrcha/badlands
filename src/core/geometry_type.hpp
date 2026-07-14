#pragma once

namespace badlands {

// Geometry type determines how the material samples textures and computes
// normals
enum class GeometryType {
  kTexturedMesh,   // UV-mapped mesh geometry (2D texture sampling)
  kSphericalMesh   // Spherical mesh geometry (cubemap sampling, sphere-derived normals)
};

}  // namespace badlands
