#pragma once

namespace badlands {

// Geometry type determines how the material samples textures and computes
// normals
enum class GeometryType {
  kTexturedMesh,    // UV-mapped mesh geometry (2D texture sampling)
  kSphericalMesh,   // Spherical mesh geometry (cubemap sampling, sphere-derived normals)
  kTerrainBlend,    // Per-vertex blend weights + texture_2d_array layer lookup
  kTerrainCluster,  // Per-vertex baked color + metadata; multi-range indexed submesh
  kInstancedMesh    // Standard textured-mesh vertex stream, but the per-object
                    // transform is read per-instance from a group-1 storage
                    // array (indexed by @builtin(instance_index)); material
                    // constants live in a group-0 UBO. See the "instanced"
                    // shader feature + RenderingMaterialInstance::BindInstanceData.
};

}  // namespace badlands
