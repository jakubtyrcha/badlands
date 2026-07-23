// Ported from sampo's src/core/math/cube_sphere.cpp, namespace sampo ->
// badlands, verbatim otherwise (includes adapted to the relocated
// engine/rendering/geometry/ path — see the deviation note in aabb.hpp).
#include "engine/rendering/geometry/cube_sphere.hpp"

#include <algorithm>

#include "engine/rendering/geometry/eac.hpp"

namespace badlands {

// Get the world-space direction that aligns with increasing U on a cube face
// Derived from EACFaceUVToDirection: which world direction does "increasing U"
// point to?
static glm::vec3 GetFaceUDirection(eac::CubeFace face) {
  switch (face) {
    case eac::CubeFace::kPosX:
      // dir = (1, -v, -u), so increasing u moves toward -Z
      return glm::vec3(0.0f, 0.0f, -1.0f);
    case eac::CubeFace::kNegX:
      // dir = (-1, -v, u), so increasing u moves toward +Z
      return glm::vec3(0.0f, 0.0f, 1.0f);
    case eac::CubeFace::kPosY:
      // dir = (u, 1, v), so increasing u moves toward +X
      return glm::vec3(1.0f, 0.0f, 0.0f);
    case eac::CubeFace::kNegY:
      // dir = (u, -1, -v), so increasing u moves toward +X
      return glm::vec3(1.0f, 0.0f, 0.0f);
    case eac::CubeFace::kPosZ:
      // dir = (u, -v, 1), so increasing u moves toward +X
      return glm::vec3(1.0f, 0.0f, 0.0f);
    case eac::CubeFace::kNegZ:
      // dir = (-u, -v, -1), so increasing u moves toward -X
      return glm::vec3(-1.0f, 0.0f, 0.0f);
    default:
      return glm::vec3(1.0f, 0.0f, 0.0f);
  }
}

// Compute tangent aligned with the face's U direction, projected onto the
// sphere surface This ensures consistent tangent frames within each face
static glm::vec3 ComputeFaceAlignedTangent(eac::CubeFace face,
                                           const glm::vec3& dir) {
  glm::vec3 face_u = GetFaceUDirection(face);
  // Project face_u onto the tangent plane at dir (remove component parallel to
  // dir)
  glm::vec3 tangent = face_u - glm::dot(face_u, dir) * dir;
  return glm::normalize(tangent);
}

CubeSphereMeshResult GenerateCubeSphereMesh(int resolution) {
  CubeSphereMeshResult result;

  const int vertices_per_face = resolution * resolution;
  const int total_vertices = 6 * vertices_per_face;
  const int triangles_per_face = 2 * (resolution - 1) * (resolution - 1);
  const int total_indices = 6 * triangles_per_face * 3;

  // Edge-aligned grid: the outermost samples land on the cube-face edges
  // (u,v in {0,1}), so adjacent faces share the exact cube-edge line and the 6
  // faces tile the cube surface watertight with no gap. No edge-stitching or
  // corner triangles are needed -- the old texel-center grid inset every face
  // by half a cell and bridged the gap with a UV-smearing triangle strip
  // (which showed up as a compressed ribbon of texture at every seam).
  const float inv =
      resolution > 1 ? 1.0f / static_cast<float>(resolution - 1) : 0.0f;

  result.positions.reserve(total_vertices);
  result.uvs.reserve(total_vertices);
  result.tangents.reserve(total_vertices);
  result.face_ids.reserve(total_vertices);
  result.indices.reserve(total_indices);

  // Generate vertices for each face
  for (int face = 0; face < 6; ++face) {
    auto cube_face = static_cast<eac::CubeFace>(face);

    // Generate vertices in grid pattern
    for (int y = 0; y < resolution; ++y) {
      for (int x = 0; x < resolution; ++x) {
        // UV in [0, 1], edge-inclusive: x=0 -> u=0 and x=res-1 -> u=1 land on
        // the cube-face boundary, so adjacent faces meet exactly at the shared
        // edge (positions coincide; per-face UVs give a normal texture seam).
        float u = static_cast<float>(x) * inv;
        float v = static_cast<float>(y) * inv;
        glm::vec2 eac_uv(u, v);

        // Convert EAC UV to direction (sphere position)
        glm::vec3 dir = eac::EACFaceUVToDirection(cube_face, eac_uv);

        // Compute tangent aligned with the face's U direction
        // This ensures consistent tangent frames within each face and correct
        // gradient orientation
        glm::vec3 tangent = ComputeFaceAlignedTangent(cube_face, dir);

        result.positions.push_back(dir);
        result.uvs.push_back(eac_uv);
        result.tangents.push_back(tangent);
        result.face_ids.push_back(static_cast<uint32_t>(face));
      }
    }

    // Generate indices for this face with flip-flop diagonal pattern
    // Alternating diagonals reduce anisotropy and improve normal quality on
    // displaced surfaces
    int face_vertex_offset = face * vertices_per_face;
    for (int y = 0; y < resolution - 1; ++y) {
      for (int x = 0; x < resolution - 1; ++x) {
        int base = face_vertex_offset + y * resolution + x;

        // Alternate diagonal direction based on checkerboard pattern
        if ((x + y) % 2 == 0) {
          // Diagonal from bottom-left to top-right
          result.indices.push_back(base);
          result.indices.push_back(base + resolution + 1);
          result.indices.push_back(base + 1);

          result.indices.push_back(base);
          result.indices.push_back(base + resolution);
          result.indices.push_back(base + resolution + 1);
        } else {
          // Diagonal from bottom-right to top-left
          result.indices.push_back(base);
          result.indices.push_back(base + resolution);
          result.indices.push_back(base + 1);

          result.indices.push_back(base + 1);
          result.indices.push_back(base + resolution);
          result.indices.push_back(base + resolution + 1);
        }
      }
    }
  }

  return result;
}

TessellatedQuadResult GenerateTessellatedQuad(int resolution) {
  TessellatedQuadResult result;

  const int total_vertices = resolution * resolution;
  const int total_triangles = 2 * (resolution - 1) * (resolution - 1);
  const int total_indices = total_triangles * 3;

  result.positions.reserve(total_vertices);
  result.uvs.reserve(total_vertices);
  result.tangents.reserve(total_vertices);
  result.indices.reserve(total_indices);

  // Constant tangent for flat quad (aligned with +X / U direction)
  glm::vec3 tangent(1.0f, 0.0f, 0.0f);

  // Generate vertices in grid pattern
  // Quad spans [-0.5, 0.5] in X and Y
  for (int y = 0; y < resolution; ++y) {
    for (int x = 0; x < resolution; ++x) {
      // UV in [0, 1] range
      float u = (static_cast<float>(x) + 0.5f) / static_cast<float>(resolution);
      float v = (static_cast<float>(y) + 0.5f) / static_cast<float>(resolution);

      // Position in [-0.5, 0.5] range
      float px = u - 0.5f;
      float py = v - 0.5f;

      result.positions.push_back(glm::vec3(px, py, 0.0f));
      result.uvs.push_back(glm::vec2(u, v));
      result.tangents.push_back(tangent);
    }
  }

  // Generate indices with flip-flop diagonal pattern (CCW winding when viewed
  // from +Z) Alternating diagonals reduce anisotropy and improve normal quality
  // on displaced surfaces
  for (int y = 0; y < resolution - 1; ++y) {
    for (int x = 0; x < resolution - 1; ++x) {
      int base = y * resolution + x;

      // Alternate diagonal direction based on checkerboard pattern
      if ((x + y) % 2 == 0) {
        // Diagonal from bottom-left to top-right: base to base+resolution+1
        // First triangle (lower-right)
        result.indices.push_back(base);
        result.indices.push_back(base + 1);
        result.indices.push_back(base + resolution + 1);

        // Second triangle (upper-left)
        result.indices.push_back(base);
        result.indices.push_back(base + resolution + 1);
        result.indices.push_back(base + resolution);
      } else {
        // Diagonal from bottom-right to top-left: base+1 to base+resolution
        // First triangle (lower-left)
        result.indices.push_back(base);
        result.indices.push_back(base + 1);
        result.indices.push_back(base + resolution);

        // Second triangle (upper-right)
        result.indices.push_back(base + 1);
        result.indices.push_back(base + resolution + 1);
        result.indices.push_back(base + resolution);
      }
    }
  }

  return result;
}

}  // namespace badlands
