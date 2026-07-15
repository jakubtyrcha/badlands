#pragma once

// Directional shadow map. Adapted from sampo's src/rendering/shadow_map.{hpp,
// cpp} (namespace sampo -> badlands), NOT a verbatim port -- see the Z
// convention + fit-math notes in shadow_map.cpp (Task T2).
#include <algorithm>
#include <cstdint>
#include <glm/glm.hpp>
#include <dawn/webgpu_cpp.h>

namespace badlands {

// CPU source of truth for the sun-shadow normal-offset bias length (world
// units): B_norm / max(NdotL, N_clamp), where B_norm = 1.5 * t_size. T3's
// WESL shadow-sampling code mirrors this exact expression -- keep both in
// sync if it ever changes.
namespace ShadowMath {
inline float NormalOffsetLength(float n_dot_l, float t_size,
                                float n_clamp = 0.05f) {
  return 1.5f * t_size / std::max(n_dot_l, n_clamp);
}
}  // namespace ShadowMath

// Directional-light shadow depth map: an orthographic camera at the sun,
// fit with a FIXED world-space coverage (coverage_dmax) around a
// caller-supplied center point, texel-snapped so the resulting matrix
// depends on that center point ONLY through its snapped (T_size-grid) cell
// -- stable under sub-texel camera motion (no shimmer), and deterministic
// enough for an exact byte-for-byte matrix comparison in tests.
//
// Conventional (non-reversed) Z: UpdateLightMatrices builds the projection
// with plain glm::ortho, which -- under the project-wide
// GLM_FORCE_DEPTH_ZERO_TO_ONE -- already yields depth in [0,1] with
// near->0, far->1. This matches the registered `kShadow` pipeline's
// CompareFunction::Less and the shadow pass's depthClearValue = 1.0 (far).
// Do NOT add a depth-remap matrix here (sampo's reversed-Z shadow pass had
// one; badlands' shadow pass is conventional-Z, see standard_material_
// factory.cpp's kShadow TargetConfig).
class ShadowMap {
 public:
  ShadowMap() = default;
  ShadowMap(wgpu::Device device, uint32_t resolution);

  // Non-copyable (owns a GPU texture).
  ShadowMap(const ShadowMap&) = delete;
  ShadowMap& operator=(const ShadowMap&) = delete;

  // Movable.
  ShadowMap(ShadowMap&& other) noexcept;
  ShadowMap& operator=(ShadowMap&& other) noexcept;

  ~ShadowMap();

  uint32_t GetResolution() const { return resolution_; }
  bool IsValid() const { return depth_texture_ != nullptr; }

  wgpu::TextureView GetDepthView() const { return depth_view_; }
  wgpu::Texture GetDepthTexture() const { return depth_texture_; }

  const glm::mat4& GetLightView() const { return light_view_; }
  const glm::mat4& GetLightProj() const { return light_proj_; }
  const glm::mat4& GetLightViewProj() const { return light_view_proj_; }

  // World-space size of one shadow-map texel (coverage_dmax / resolution),
  // as of the last UpdateLightMatrices call.
  float TexelWorldSize() const { return texel_world_size_; }

  // (Re)creates the depth texture at `resolution` x `resolution`
  // (Depth32Float, RenderAttachment|TextureBinding|CopySrc, 1 mip/sample).
  // Safe to call repeatedly (e.g. when ShadowConfig::resolution changes) --
  // Dawn's wgpu::Texture is ref-counted, so overwriting the old texture
  // releases it. Deliberately separate from UpdateLightMatrices, which is
  // pure CPU math with no GPU dependency (so it's directly unit-testable).
  void CreateTexture(wgpu::Device device, uint32_t resolution);

  // Fixed-coverage fit: an orthographic frustum of world-space width/height
  // `coverage_dmax`, centered on `center_world`, looking along
  // -normalize(sun_dir) (the shadow camera looks FROM the sun TOWARD the
  // scene). Texel-snapped -- see shadow_map.cpp -- so the result depends on
  // `center_world` only through its snapped (T_size-grid) cell in all three
  // light-space axes.
  //
  // sun_dir: direction TOWARD the sun (SceneContext::sun_direction's
  // convention).
  // center_world: world-space point the frustum is centered on (e.g. a
  // point ahead of the camera).
  // coverage_dmax: ortho frustum width/height (world units).
  // resolution: shadow map resolution (for T_size = coverage_dmax /
  // resolution); independent of the GPU texture's actual size (see
  // CreateTexture) so this method has no GPU dependency.
  // backward_extension: extra light-space depth toward the light, beyond
  // coverage_dmax, so casters outside the visible near side stay in the
  // frustum. z_range = coverage_dmax + backward_extension is a FIXED
  // constant (independent of center_world) -- required for the
  // texel-snap's determinism guarantee above.
  void UpdateLightMatrices(const glm::vec3& sun_dir,
                           const glm::vec3& center_world, float coverage_dmax,
                           uint32_t resolution, float backward_extension);

 private:
  void Destroy();

  wgpu::Texture depth_texture_;
  wgpu::TextureView depth_view_;

  glm::mat4 light_view_ = glm::mat4(1.0f);
  glm::mat4 light_proj_ = glm::mat4(1.0f);
  glm::mat4 light_view_proj_ = glm::mat4(1.0f);
  float texel_world_size_ = 0.0f;

  uint32_t resolution_ = 0;
};

}  // namespace badlands
