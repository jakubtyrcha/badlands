#pragma once

// Ported from sampo's src/core/math/eac.hpp, namespace sampo -> badlands,
// verbatim otherwise. Relocated from `core/math/` to
// `engine/rendering/geometry/` — see the deviation note in aabb.hpp; this is
// a transitive dependency of cube_sphere.cpp (EAC face/UV <-> direction
// mapping for the cube-sphere mesh builder) and has no other consumer here.

#include <cmath>
#include <glm/glm.hpp>
#include <glm/gtc/constants.hpp>

namespace badlands {
namespace eac {

// EAC (Equi-Angular Cubemap) mapping utilities
// EAC provides more uniform pixel density on a sphere compared to standard
// cubemaps by using angular (tan-based) interpolation instead of linear

// Cubemap face indices (matches WebGPU/OpenGL convention)
enum class CubeFace : int {
  kPosX = 0,  // +X (right)
  kNegX = 1,  // -X (left)
  kPosY = 2,  // +Y (up)
  kNegY = 3,  // -Y (down)
  kPosZ = 4,  // +Z (front in our convention)
  kNegZ = 5,  // -Z (back)
};

// Convert a 3D direction to cubemap face + UV coordinates (standard cubemap)
inline void DirectionToCubemapFaceUV(const glm::vec3& dir, CubeFace& face,
                                     glm::vec2& uv) {
  glm::vec3 abs_dir = glm::abs(dir);

  float max_axis = glm::max(abs_dir.x, glm::max(abs_dir.y, abs_dir.z));
  float u, v;

  if (max_axis == abs_dir.x) {
    if (dir.x > 0) {
      face = CubeFace::kPosX;
      u = -dir.z / abs_dir.x;
      v = -dir.y / abs_dir.x;
    } else {
      face = CubeFace::kNegX;
      u = dir.z / abs_dir.x;
      v = -dir.y / abs_dir.x;
    }
  } else if (max_axis == abs_dir.y) {
    if (dir.y > 0) {
      face = CubeFace::kPosY;
      u = dir.x / abs_dir.y;
      v = dir.z / abs_dir.y;
    } else {
      face = CubeFace::kNegY;
      u = dir.x / abs_dir.y;
      v = -dir.z / abs_dir.y;
    }
  } else {
    if (dir.z > 0) {
      face = CubeFace::kPosZ;
      u = dir.x / abs_dir.z;
      v = -dir.y / abs_dir.z;
    } else {
      face = CubeFace::kNegZ;
      u = -dir.x / abs_dir.z;
      v = -dir.y / abs_dir.z;
    }
  }

  // Convert from [-1, 1] to [0, 1]
  uv.x = u * 0.5f + 0.5f;
  uv.y = v * 0.5f + 0.5f;
}

// Apply EAC forward mapping: linear UV -> angular UV
// Input: standard cubemap UV in [0, 1]
// Output: EAC-mapped UV in [0, 1]
inline glm::vec2 LinearToEAC(const glm::vec2& uv) {
  // Convert to [-1, 1] range
  glm::vec2 ndc = uv * 2.0f - 1.0f;

  // Apply EAC transformation: u' = (4/pi) * atan(u)
  // For linear in [-1, 1], atan gives [-π/4, π/4], so (4/π) maps to [-1, 1]
  // This makes angular distance proportional to UV distance
  constexpr float kFourOverPi = 4.0f / glm::pi<float>();
  glm::vec2 eac_ndc;
  eac_ndc.x = kFourOverPi * std::atan(ndc.x);
  eac_ndc.y = kFourOverPi * std::atan(ndc.y);

  // Convert back to [0, 1]
  return eac_ndc * 0.5f + 0.5f;
}

// Apply EAC inverse mapping: angular UV -> linear UV
// Input: EAC-mapped UV in [0, 1]
// Output: standard cubemap UV in [0, 1]
inline glm::vec2 EACToLinear(const glm::vec2& eac_uv) {
  // Convert to [-1, 1] range
  glm::vec2 eac_ndc = eac_uv * 2.0f - 1.0f;

  // Apply inverse EAC transformation: u = tan(u' * pi/4)
  // For eac_ndc in [-1, 1], this gives tan([-π/4, π/4]) = [-1, 1]
  constexpr float kPiOverFour = glm::pi<float>() / 4.0f;
  glm::vec2 ndc;
  ndc.x = std::tan(eac_ndc.x * kPiOverFour);
  ndc.y = std::tan(eac_ndc.y * kPiOverFour);

  // Convert back to [0, 1]
  return ndc * 0.5f + 0.5f;
}

// Convert a 3D direction to EAC cubemap face + UV coordinates
inline void DirectionToEACFaceUV(const glm::vec3& dir, CubeFace& face,
                                 glm::vec2& uv) {
  DirectionToCubemapFaceUV(dir, face, uv);
  uv = LinearToEAC(uv);
}

// Convert EAC face + UV to 3D direction
inline glm::vec3 EACFaceUVToDirection(CubeFace face, const glm::vec2& eac_uv) {
  // First convert EAC UV to standard cubemap UV
  glm::vec2 uv = EACToLinear(eac_uv);

  // Convert UV from [0, 1] to [-1, 1]
  float u = uv.x * 2.0f - 1.0f;
  float v = uv.y * 2.0f - 1.0f;

  glm::vec3 dir;
  switch (face) {
    case CubeFace::kPosX:
      dir = glm::vec3(1.0f, -v, -u);
      break;
    case CubeFace::kNegX:
      dir = glm::vec3(-1.0f, -v, u);
      break;
    case CubeFace::kPosY:
      dir = glm::vec3(u, 1.0f, v);
      break;
    case CubeFace::kNegY:
      dir = glm::vec3(u, -1.0f, -v);
      break;
    case CubeFace::kPosZ:
      dir = glm::vec3(u, -v, 1.0f);
      break;
    case CubeFace::kNegZ:
      dir = glm::vec3(-u, -v, -1.0f);
      break;
  }

  return glm::normalize(dir);
}

// Convert standard cubemap face + UV to 3D direction (for comparison/testing)
inline glm::vec3 CubemapFaceUVToDirection(CubeFace face, const glm::vec2& uv) {
  // Convert UV from [0, 1] to [-1, 1]
  float u = uv.x * 2.0f - 1.0f;
  float v = uv.y * 2.0f - 1.0f;

  glm::vec3 dir;
  switch (face) {
    case CubeFace::kPosX:
      dir = glm::vec3(1.0f, -v, -u);
      break;
    case CubeFace::kNegX:
      dir = glm::vec3(-1.0f, -v, u);
      break;
    case CubeFace::kPosY:
      dir = glm::vec3(u, 1.0f, v);
      break;
    case CubeFace::kNegY:
      dir = glm::vec3(u, -1.0f, -v);
      break;
    case CubeFace::kPosZ:
      dir = glm::vec3(u, -v, 1.0f);
      break;
    case CubeFace::kNegZ:
      dir = glm::vec3(-u, -v, -1.0f);
      break;
  }

  return glm::normalize(dir);
}

// =============================================================================
// Cross-Face Sampling Utilities
// =============================================================================

// Edge encoding for cube faces
// 0 = u_min (u=0, left edge)
// 1 = u_max (u=1, right edge)
// 2 = v_min (v=0, bottom edge)
// 3 = v_max (v=1, top edge)
enum class EdgeType : int {
  kUMin = 0,
  kUMax = 1,
  kVMin = 2,
  kVMax = 3,
};

// Edge connection between cube faces
struct EdgeConnection {
  int face_a;
  int edge_a;
  int face_b;
  int edge_b;
  bool reversed;  // if true, vertex i on edge_a connects to (resolution-1-i) on
                  // edge_b
};

// 12 edges of the cube - which face edges are adjacent
// Derived from EACFaceUVToDirection mappings
inline constexpr EdgeConnection kEdgeConnections[12] = {
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

// Result of cross-face UV remapping
struct CrossFaceResult {
  CubeFace face;
  glm::vec2 uv;
  bool crossed;  // true if UV crossed a boundary
};

// Find edge connection for a given face and edge
// Returns nullptr if not found (shouldn't happen with valid inputs)
inline const EdgeConnection* FindEdgeConnection(int face, int edge) {
  for (const auto& conn : kEdgeConnections) {
    if (conn.face_a == face && conn.edge_a == edge) {
      return &conn;
    }
    if (conn.face_b == face && conn.edge_b == edge) {
      // Return a modified view (swap a and b)
      static EdgeConnection reversed;
      reversed.face_a = conn.face_b;
      reversed.edge_a = conn.edge_b;
      reversed.face_b = conn.face_a;
      reversed.edge_b = conn.edge_a;
      reversed.reversed = conn.reversed;
      return &reversed;
    }
  }
  return nullptr;
}

// Get the position along an edge given the edge type and the "other" coordinate
// For u edges (0,1): position is determined by v
// For v edges (2,3): position is determined by u
inline float GetEdgePosition(int edge, const glm::vec2& uv) {
  switch (edge) {
    case 0:  // u=0, position along edge is v
    case 1:  // u=1, position along edge is v
      return uv.y;
    case 2:  // v=0, position along edge is u
    case 3:  // v=1, position along edge is u
      return uv.x;
    default:
      return 0.5f;
  }
}

// Construct UV from edge type and position along edge
// edge: 0=u_min, 1=u_max, 2=v_min, 3=v_max
// position: [0,1] position along the edge
// offset: how far inside the face from the edge (positive = inside)
inline glm::vec2 ConstructUVFromEdge(int edge, float position, float offset) {
  switch (edge) {
    case 0:  // u=0 (left edge)
      return {offset, position};
    case 1:  // u=1 (right edge)
      return {1.0f - offset, position};
    case 2:  // v=0 (bottom edge)
      return {position, offset};
    case 3:  // v=1 (top edge)
      return {position, 1.0f - offset};
    default:
      return {0.5f, 0.5f};
  }
}

// Remap UV that has crossed a face boundary to the correct adjacent face
// Input: face and UV where some component is outside [0, 1]
// Output: remapped face and UV on the adjacent face
// If UV is inside [0, 1], returns the same face/UV with crossed=false
inline CrossFaceResult RemapCrossFaceUV(CubeFace face, const glm::vec2& uv) {
  CrossFaceResult result;
  result.face = face;
  result.uv = uv;
  result.crossed = false;

  int face_idx = static_cast<int>(face);

  // Check which edge was crossed
  int crossed_edge = -1;
  float overflow = 0.0f;

  if (uv.x < 0.0f) {
    crossed_edge = 0;  // u_min
    overflow = -uv.x;
  } else if (uv.x > 1.0f) {
    crossed_edge = 1;  // u_max
    overflow = uv.x - 1.0f;
  } else if (uv.y < 0.0f) {
    crossed_edge = 2;  // v_min
    overflow = -uv.y;
  } else if (uv.y > 1.0f) {
    crossed_edge = 3;  // v_max
    overflow = uv.y - 1.0f;
  }

  if (crossed_edge < 0) {
    // UV is inside [0, 1], no crossing
    return result;
  }

  // Find the edge connection
  const EdgeConnection* conn = FindEdgeConnection(face_idx, crossed_edge);
  if (!conn) {
    // Shouldn't happen, but clamp as fallback
    result.uv = glm::clamp(uv, glm::vec2(0.0f), glm::vec2(1.0f));
    return result;
  }

  // Get position along the edge
  float edge_pos = GetEdgePosition(crossed_edge, uv);

  // Apply reversal if needed
  if (conn->reversed) {
    edge_pos = 1.0f - edge_pos;
  }

  // Construct UV on the target face
  // The overflow becomes the distance from the edge on the target face
  result.face = static_cast<CubeFace>(conn->face_b);
  result.uv = ConstructUVFromEdge(conn->edge_b, edge_pos, overflow);
  result.crossed = true;

  return result;
}

// Get neighbor direction for gradient computation, handling cross-face sampling
// This is the key function to replace clamped UV sampling in the shader
// face: current face (as int for compatibility with shader)
// center_uv: UV coordinate of the center sample point
// offset: offset to add for neighbor sampling (e.g., {texel_size, 0} for right
// neighbor) Returns: 3D direction for the neighbor sample
inline glm::vec3 GetNeighborDirectionForGradient(CubeFace face,
                                                 const glm::vec2& center_uv,
                                                 const glm::vec2& offset) {
  glm::vec2 neighbor_uv = center_uv + offset;

  // Check if we crossed a face boundary
  CrossFaceResult remapped = RemapCrossFaceUV(face, neighbor_uv);

  // Convert the (possibly remapped) face+UV to direction
  return EACFaceUVToDirection(remapped.face, remapped.uv);
}

// Overload taking face as int (for compatibility with shader patterns)
inline glm::vec3 GetNeighborDirectionForGradient(int face,
                                                 const glm::vec2& center_uv,
                                                 const glm::vec2& offset) {
  return GetNeighborDirectionForGradient(static_cast<CubeFace>(face), center_uv,
                                         offset);
}

}  // namespace eac
}  // namespace badlands
