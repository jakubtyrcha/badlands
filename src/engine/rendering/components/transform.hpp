#pragma once

// Ported from sampo's src/rendering/components/transform.hpp, namespace
// sampo -> badlands, verbatim otherwise.

#include <glm/glm.hpp>

namespace badlands {

// Transform component - stores object's world position
struct Transform {
  glm::mat4 matrix = glm::mat4(1.0f);
};

// Per-object uniform data for GPU (matches shader ObjectUniforms struct)
struct ObjectUniforms {
  glm::mat4 model_matrix{1.0f};  // LocalSpace -> WorldCameraOffsetedSpace
};
static_assert(sizeof(ObjectUniforms) == 64,
              "ObjectUniforms must match WGSL layout");

}  // namespace badlands
