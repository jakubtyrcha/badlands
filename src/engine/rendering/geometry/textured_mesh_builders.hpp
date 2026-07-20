#pragma once

// Ported from sampo's src/rendering/geometry/textured_mesh_builders.hpp,
// namespace sampo -> badlands, verbatim otherwise (includes adapted to
// badlands paths).

#include <functional>

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
// `uv_scale` multiplies the generated [0,1] UVs (e.g. 2.0 makes the texture
// repeat twice across the quad) -- combine with a Repeat-address-mode
// sampler to tile a texture across a large quad instead of stretching one
// copy across it.
TexturedMeshResult GenerateQuadTexturedMesh(float size = 1.0f,
                                            int resolution = 1,
                                            float uv_scale = 1.0f);

// Height-field terrain mesh over the XZ plane (+Y up), spanning
// [-size/2, size/2] in X and Z with `resolution` grid cells per side
// (>=1; `resolution+1` samples per side). Vertex Y = `height_fn(x, z)`;
// per-vertex normals come from central differences of `height_fn`, tangents
// point along +X on the surface (T = normalize(vec3(1, dH/dx, 0)), so T.N == 0).
// UV = XZ normalized to [0,1] * `uv_scale`. Non-indexed triangle list, wound
// CCW as seen from +Y. Used for the test lake bottom (a dipped basin).
TexturedMeshResult GenerateHeightmapMesh(
    float size, int resolution,
    const std::function<float(float x, float z)>& height_fn,
    float uv_scale = 1.0f);

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
