#pragma once

// CPU mirror of the octahedron normal encode/decode used by the G-buffer, so
// tests can decode a read-back RG16Float normals texture and compare against a
// CPU model. Encode mirrors shaders/common/gbuffer_encode.wesl::encodeOctahedron;
// decode mirrors shaders/common/frame.wesl::decodeOctahedron. Keep in sync.
//
// Header-only, glm-only (no Dawn) — usable from pure-CPU test targets.

#include <cmath>

#include <glm/glm.hpp>

namespace badlands {

// Unit normal -> octahedron RG in [-1, 1].
inline glm::vec2 EncodeOctahedron(glm::vec3 n) {
  glm::vec3 absN = glm::abs(n);
  glm::vec2 oct = glm::vec2(n.x, n.y) / (absN.x + absN.y + absN.z);
  if (n.z < 0.0f) {
    glm::vec2 sign(oct.x >= 0.0f ? 1.0f : -1.0f, oct.y >= 0.0f ? 1.0f : -1.0f);
    oct = (glm::vec2(1.0f) - glm::abs(glm::vec2(oct.y, oct.x))) * sign;
  }
  return oct;
}

// Octahedron RG in [-1, 1] -> unit normal.
inline glm::vec3 DecodeOctahedron(glm::vec2 oct) {
  glm::vec3 n(oct.x, oct.y, 1.0f - std::abs(oct.x) - std::abs(oct.y));
  if (n.z < 0.0f) {
    float nx = (1.0f - std::abs(n.y)) * (n.x >= 0.0f ? 1.0f : -1.0f);
    float ny = (1.0f - std::abs(n.x)) * (n.y >= 0.0f ? 1.0f : -1.0f);
    n = glm::vec3(nx, ny, n.z);
  }
  return glm::normalize(n);
}

}  // namespace badlands
