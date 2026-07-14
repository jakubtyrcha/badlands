// Ported from sampo's src/core/math/cube_sphere.cpp, namespace sampo ->
// badlands, verbatim otherwise (includes adapted to the relocated
// engine/rendering/geometry/ path — see the deviation note in aabb.hpp).
#include "engine/rendering/geometry/cube_sphere.hpp"

#include <algorithm>

#include "engine/rendering/geometry/eac.hpp"

namespace badlands {

// Ensure triangle has outward-facing normal (CCW winding for sphere)
static void EnsureOutwardWinding(std::vector<uint32_t>& indices,
                                 const std::vector<glm::vec3>& positions,
                                 size_t triangle_start) {
  uint32_t i0 = indices[triangle_start];
  uint32_t i1 = indices[triangle_start + 1];
  uint32_t i2 = indices[triangle_start + 2];

  glm::vec3 v0 = positions[i0];
  glm::vec3 v1 = positions[i1];
  glm::vec3 v2 = positions[i2];

  // Compute face normal
  glm::vec3 edge1 = v1 - v0;
  glm::vec3 edge2 = v2 - v0;
  glm::vec3 face_normal = glm::cross(edge1, edge2);

  // Centroid direction is outward normal for unit sphere
  glm::vec3 centroid = (v0 + v1 + v2) / 3.0f;

  // If face normal points inward, swap two vertices
  if (glm::dot(face_normal, centroid) < 0) {
    std::swap(indices[triangle_start + 1], indices[triangle_start + 2]);
  }
}

// Edge connection between cube faces
// edge encoding: 0=u_min (u=0), 1=u_max (u=1), 2=v_min (v=0), 3=v_max (v=1)
struct EdgeConnection {
  int face_a;
  int edge_a;
  int face_b;
  int edge_b;
  bool reversed;  // if true, vertex i on edge_a connects to (resolution-1-i) on
                  // edge_b
};

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

// 12 edges of the cube - which face edges are adjacent
// Derived from EACFaceUVToDirection mappings
static const EdgeConnection kEdgeConnections[12] = {
    {0, 0, 4, 1, false},  // PosX u=0 ↔ PosZ u=1
    {0, 1, 5, 0, false},  // PosX u=1 ↔ NegZ u=0
    {0, 2, 2, 1, true},   // PosX v=0 ↔ PosY u=1, reversed
    {0, 3, 3, 1, false},  // PosX v=1 ↔ NegY u=1
    {1, 0, 5, 1, false},  // NegX u=0 ↔ NegZ u=1
    {1, 1, 4, 0, false},  // NegX u=1 ↔ PosZ u=0
    {1, 2, 2, 0, false},  // NegX v=0 ↔ PosY u=0
    {1, 3, 3, 0, true},   // NegX v=1 ↔ NegY u=0, reversed
    {2, 2, 5, 2, true},   // PosY v=0 ↔ NegZ v=0, reversed
    {2, 3, 4, 2, false},  // PosY v=1 ↔ PosZ v=0
    {3, 2, 4, 3, false},  // NegY v=0 ↔ PosZ v=1
    {3, 3, 5, 3, true},   // NegY v=1 ↔ NegZ v=1, reversed
};

// Get vertex index along an edge of a face
// edge: 0=u_min, 1=u_max, 2=v_min, 3=v_max
// i: position along edge [0, resolution-1]
static int GetEdgeVertexIndex(int face, int edge, int i, int resolution) {
  int face_offset = face * resolution * resolution;
  switch (edge) {
    case 0:  // u=0 (left edge): x=0, y varies
      return face_offset + i * resolution + 0;
    case 1:  // u=1 (right edge): x=resolution-1, y varies
      return face_offset + i * resolution + (resolution - 1);
    case 2:  // v=0 (bottom edge): y=0, x varies
      return face_offset + 0 * resolution + i;
    case 3:  // v=1 (top edge): y=resolution-1, x varies
      return face_offset + (resolution - 1) * resolution + i;
    default:
      return 0;
  }
}

CubeSphereMeshResult GenerateCubeSphereMesh(int resolution) {
  CubeSphereMeshResult result;

  const int vertices_per_face = resolution * resolution;
  const int total_vertices = 6 * vertices_per_face;
  const int triangles_per_face = 2 * (resolution - 1) * (resolution - 1);
  // Add stitching triangles: 12 edges × (resolution-1) quads × 2 triangles
  const int stitching_triangles = 12 * (resolution - 1) * 2;
  const int total_indices = (6 * triangles_per_face + stitching_triangles) * 3;

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
        // UV in [0, 1] range at texel centers
        float u =
            (static_cast<float>(x) + 0.5f) / static_cast<float>(resolution);
        float v =
            (static_cast<float>(y) + 0.5f) / static_cast<float>(resolution);
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

  // Generate stitching triangles to connect adjacent faces
  for (const auto& edge : kEdgeConnections) {
    for (int i = 0; i < resolution - 1; ++i) {
      // Get vertex indices along both edges
      int a0 = GetEdgeVertexIndex(edge.face_a, edge.edge_a, i, resolution);
      int a1 = GetEdgeVertexIndex(edge.face_a, edge.edge_a, i + 1, resolution);

      int b_i = edge.reversed ? (resolution - 1 - i) : i;
      int b_i1 = edge.reversed ? (resolution - 2 - i) : (i + 1);

      int b0 = GetEdgeVertexIndex(edge.face_b, edge.edge_b, b_i, resolution);
      int b1 = GetEdgeVertexIndex(edge.face_b, edge.edge_b, b_i1, resolution);

      // Create two triangles for the quad between the edges
      // Triangle 1: a0 -> b0 -> a1
      result.indices.push_back(a0);
      result.indices.push_back(b0);
      result.indices.push_back(a1);

      // Triangle 2: a1 -> b0 -> b1
      result.indices.push_back(a1);
      result.indices.push_back(b0);
      result.indices.push_back(b1);

      // Fix winding order if needed (ensure outward-facing normals)
      size_t tri_start = result.indices.size() - 6;
      EnsureOutwardWinding(result.indices, result.positions, tri_start);
      EnsureOutwardWinding(result.indices, result.positions, tri_start + 3);
    }
  }

  // Generate corner triangles to close gaps where 3 faces meet
  // There are 8 cube corners, each connecting 3 face vertices
  // Corner vertex position: (face, u_edge, v_edge) where edge means 0 or
  // resolution-1
  struct CornerVertex {
    int face;
    int x;  // 0 or resolution-1
    int y;  // 0 or resolution-1
  };
  struct CornerTriangle {
    CornerVertex v0, v1, v2;
  };

  // Each corner has 3 vertices from 3 faces
  // Derived from EAC mapping: which (face, grid_corner) maps to which cube
  // corner face 0 (PosX): dir=(1,-v,-u), so (u_min,v_min)→(+X,+Y,+Z),
  // (u_max,v_min)→(+X,+Y,-Z), etc. face 1 (NegX): dir=(-1,-v,u), so
  // (u_min,v_min)→(-X,+Y,-Z), (u_max,v_min)→(-X,+Y,+Z), etc. face 2 (PosY):
  // dir=(u,1,v), so (u_min,v_min)→(-X,+Y,-Z), (u_max,v_max)→(+X,+Y,+Z), etc.
  // face 3 (NegY): dir=(u,-1,-v), so (u_min,v_min)→(-X,-Y,+Z),
  // (u_max,v_min)→(+X,-Y,+Z), etc. face 4 (PosZ): dir=(u,-v,1), so
  // (u_min,v_min)→(-X,+Y,+Z), (u_max,v_min)→(+X,+Y,+Z), etc. face 5 (NegZ):
  // dir=(-u,-v,-1), so (u_min,v_min)→(+X,+Y,-Z), (u_max,v_min)→(-X,+Y,-Z), etc.
  const int u_min = 0;
  const int u_max = resolution - 1;
  const int v_min = 0;
  const int v_max = resolution - 1;

  const CornerTriangle kCornerTriangles[8] = {
      // Corner (+X, +Y, +Z): PosX(0,0), PosY(max,max), PosZ(max,0)
      {{0, u_min, v_min}, {2, u_max, v_max}, {4, u_max, v_min}},
      // Corner (+X, +Y, -Z): PosX(max,0), PosY(max,0), NegZ(0,0)
      {{0, u_max, v_min}, {2, u_max, v_min}, {5, u_min, v_min}},
      // Corner (+X, -Y, +Z): PosX(0,max), NegY(max,0), PosZ(max,max)
      {{0, u_min, v_max}, {3, u_max, v_min}, {4, u_max, v_max}},
      // Corner (+X, -Y, -Z): PosX(max,max), NegY(max,max), NegZ(0,max)
      {{0, u_max, v_max}, {3, u_max, v_max}, {5, u_min, v_max}},
      // Corner (-X, +Y, +Z): NegX(max,0), PosY(0,max), PosZ(0,0)
      {{1, u_max, v_min}, {2, u_min, v_max}, {4, u_min, v_min}},
      // Corner (-X, +Y, -Z): NegX(0,0), PosY(0,0), NegZ(max,0)
      {{1, u_min, v_min}, {2, u_min, v_min}, {5, u_max, v_min}},
      // Corner (-X, -Y, +Z): NegX(max,max), NegY(0,0), PosZ(0,max)
      {{1, u_max, v_max}, {3, u_min, v_min}, {4, u_min, v_max}},
      // Corner (-X, -Y, -Z): NegX(0,max), NegY(0,max), NegZ(max,max)
      {{1, u_min, v_max}, {3, u_min, v_max}, {5, u_max, v_max}},
  };

  auto get_vertex_index = [&](const CornerVertex& cv) {
    return cv.face * vertices_per_face + cv.y * resolution + cv.x;
  };

  for (const auto& corner : kCornerTriangles) {
    int i0 = get_vertex_index(corner.v0);
    int i1 = get_vertex_index(corner.v1);
    int i2 = get_vertex_index(corner.v2);
    result.indices.push_back(i0);
    result.indices.push_back(i1);
    result.indices.push_back(i2);

    // Fix winding order if needed (ensure outward-facing normals)
    size_t tri_start = result.indices.size() - 3;
    EnsureOutwardWinding(result.indices, result.positions, tri_start);
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
