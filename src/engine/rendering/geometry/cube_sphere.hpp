#pragma once

// Ported from sampo's src/core/math/cube_sphere.hpp, namespace sampo ->
// badlands, verbatim otherwise. Relocated from `core/math/` to
// `engine/rendering/geometry/` — see the deviation note in aabb.hpp; this is
// a direct dependency of textured_mesh_builders.{hpp,cpp} (sphere/quad mesh
// generation) and has no other consumer here.

#include <cstdint>
#include <glm/glm.hpp>
#include <vector>

namespace badlands {

struct CubeSphereMeshResult {
  std::vector<glm::vec3> positions;  // Positions on unit sphere
  std::vector<glm::vec2> uvs;        // EAC UV coordinates for texture lookup
  std::vector<glm::vec3>
      tangents;  // Tangent vectors (perpendicular to radial direction)
  std::vector<uint32_t> face_ids;  // Cubemap face index (0-5)
  std::vector<uint32_t> indices;   // Triangle indices
};

// Generates a sphere mesh from 6 cube faces, each tessellated to resolution x
// resolution. Uses EAC (Equi-Angular Cubemap) mapping for uniform vertex
// distribution. Total vertices: 6 * resolution * resolution Total triangles: 6
// * 2 * (resolution-1) * (resolution-1)
CubeSphereMeshResult GenerateCubeSphereMesh(int resolution);

struct TessellatedQuadResult {
  std::vector<glm::vec3> positions;  // Positions in XY plane (Z=0)
  std::vector<glm::vec2> uvs;        // UV coordinates [0,1] x [0,1]
  std::vector<glm::vec3>
      tangents;                   // Tangent vectors (all (1,0,0) for flat quad)
  std::vector<uint32_t> indices;  // Triangle indices
};

// Generates a tessellated quad in the XY plane, centered at origin.
// Quad spans [-0.5, 0.5] x [-0.5, 0.5] in X and Y, Z=0.
// Total vertices: resolution * resolution
// Total triangles: 2 * (resolution-1) * (resolution-1)
TessellatedQuadResult GenerateTessellatedQuad(int resolution);

}  // namespace badlands
