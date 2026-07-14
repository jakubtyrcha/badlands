#pragma once

// Ported from sampo's src/rendering/geometry/textured_mesh_builders.hpp,
// namespace sampo -> badlands, verbatim otherwise (includes adapted to
// badlands paths).

#include <glm/glm.hpp>

#include "engine/rendering/components/mesh_components.hpp"
#include "engine/rendering/geometry/aabb.hpp"

namespace badlands {

// Vertex layout constants
inline constexpr size_t kTexturedMeshFloatsPerVertex =
    11;  // pos(3)+uv(2)+normal(3)+tangent(3)
inline constexpr size_t kCubeMapMeshFloatsPerVertex =
    9;  // pos(3)+normal(3)+tangent(3)

// Result of mesh generation: mesh data + precomputed local-space AABB.
struct TexturedMeshResult {
  StaticTexturedMeshComponent mesh;
  Aabb local_bounds;
};

// UV-mapped sphere mesh (for 2D texture sampling)
// Uses cube sphere topology with EAC UV mapping
TexturedMeshResult GenerateSphereTexturedMesh(float radius,
                                              int resolution = 16);

// UV-mapped quad mesh in XY plane
// Quad spans [-size/2, size/2] in X and Y, Z=0
TexturedMeshResult GenerateQuadTexturedMesh(float size = 1.0f,
                                            int resolution = 1);

// Cubemap-sampled sphere mesh (for spherical/EAC sampling)
// No UV - direction-based cubemap sampling
// Uses cube sphere topology
StaticCubeMapTexturedMesh GenerateSphereCubeMapTexturedMesh(float radius,
                                                            int resolution = 16);

// Compute local-space AABB from StaticTexturedMeshComponent vertex data.
Aabb ComputeLocalAabb(const StaticTexturedMeshComponent& mesh);

// Compute local-space AABB from raw interleaved vertex data (first 3 floats =
// position).
Aabb ComputeLocalAabbFromVertices(const std::vector<float>& vertices,
                                  size_t floats_per_vertex);

}  // namespace badlands
