#pragma once

#include <cmath>
#include <cstdint>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

// Ported from sampo's src/core/camera.hpp, namespace sampo->badlands.

namespace badlands {

// Simple camera struct - provides view/projection matrices from basic vectors.
// For higher-level camera controls (orbit, pan, zoom), use
// SurfaceCameraController.
struct Camera {
  // Basic camera vectors
  glm::vec3 position{0.0f, 0.0f, 0.0f};
  glm::vec3 direction{0.0f, 0.0f, -1.0f};  // Normalized look direction
  glm::vec3 up{0.0f, 1.0f, 0.0f};

  // Projection parameters
  float fov = 45.0f;
  float aspect = 1.0f;
  float near_plane = 0.1f;
  float far_plane = 10000.0f;

  glm::vec3 GetPosition() const { return position; }

  glm::mat4 GetView() const {
    return glm::lookAt(position, position + direction, up);
  }

  glm::mat4 GetProj() const {
    auto proj =
        glm::perspective(glm::radians(fov), aspect, near_plane, far_plane);
    // Remap Z for reversed-Z: z' = 1 - z (near->1, far->0)
    glm::mat4 remap(1.0f);
    remap[2][2] = -1.0f;
    remap[3][2] = 1.0f;
    return remap * proj;
  }

  // Convenience: set direction to look at a target point
  void LookAt(const glm::vec3& target) {
    glm::vec3 to_target = target - position;
    if (glm::length(to_target) > 1e-6f) {
      direction = glm::normalize(to_target);
    }
  }
};

// Mirrors `FrameUniforms` in shaders/common/frame.wesl (@group(0)@binding(0))
// field-for-field — layout, order, and size must stay in sync with that
// struct. sampo's last field was `dpi_scale`; badlands' frame.wesl retired it
// to an unused `_padding0` float (kept only for struct alignment), so
// `padding0` below mirrors that rather than sampo's original semantics.
struct UniformData {
  glm::mat4 view;  // World-offset view (camera at origin)
  glm::mat4 proj;
  glm::mat4 view_prev;        // Previous frame view (for motion vectors/TAA)
  glm::mat4 proj_prev;        // Previous frame projection
  glm::mat4 light_view_proj;  // Shadow map: light view-projection matrix
  glm::vec4
      camera_world_pos;  // xyz = position, w unused (vec4 for WGSL alignment)
  glm::vec4 sunDir;      // xyz = direction, w unused
  glm::vec4 sunColor;    // xyz = color, w unused
  glm::vec4 ambient_sh[9];  // SH L2 (9 coefficients) for directional ambient,
                            // RGB in xyz
  glm::vec4 sphere_offset;  // xyz = terrain sphere center offset, w unused
  glm::vec2 jitter;         // TAA jitter offset (0,0 until TAA enabled)
  glm::vec2 jitter_prev;    // Previous frame jitter
  float near_plane;         // Camera near plane distance
  float far_plane;          // Camera far plane distance
  glm::vec2 screen_size;    // Screen width, height in pixels
  // Rendering option flags
  uint32_t enable_gtao;       // 1 = AO enabled, 0 = disabled (use 1.0 for AO)
  uint32_t tonemap_mode;      // TonemapMode enum value
  uint32_t output_is_linear;  // 1 = RGBA16Float (linear output), 0 = sRGB
  float padding0 = 0.0f;      // Unused; matches frame.wesl's `_padding0`
};

static_assert(sizeof(UniformData) == 576,
              "UniformData must match FrameUniforms in "
              "shaders/common/frame.wesl (@group(0)@binding(0)) byte-for-byte");

}  // namespace badlands
