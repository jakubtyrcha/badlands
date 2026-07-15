// Adapted (not ported verbatim) from sampo's src/rendering/shadow_map.cpp,
// namespace sampo -> badlands. Task T2.
//
// Deviations from sampo:
// - Conventional (non-reversed) Z: plain glm::ortho, no depth-remap matrix
//   (sampo's shadow pass was reversed-Z; badlands' registered `kShadow`
//   pipeline is CompareFunction::Less against a 1.0-cleared depth target --
//   see the header comment and standard_material_factory.cpp).
// - ONE fixed-coverage fit (sampo had a sphere-based UpdateLightMatrices and
//   an AABB-fitting UpdateLightMatricesFromBounds; this keeps only the
//   former's shape, with a FIXED z_range instead of one derived from scene
//   bounds -- required so the fit is a pure function of (sun_dir,
//   center_world, coverage_dmax, resolution, backward_extension), which
//   T4's Test 2 exercises directly).
// - The texel-snap projects center_world onto the light's (right, up,
//   look_dir) axes directly (three dot products against a FIXED,
//   origin-anchored basis) instead of transforming center_world through the
//   view matrix that was *just built from a light_pos derived from that same
//   center_world* (sampo's construction, ported literally from
//   shadow_map.cpp:97-123). That self-referential construction is a no-op:
//   since light_pos is defined as `center_world - look_dir * D`, the vector
//   from light_pos to center_world is *always* exactly `look_dir * D`
//   (a pure identity, independent of center_world's actual value), so its
//   projection onto the perpendicular (right, up) axes is always ~0 --
//   floor-snapping ~0 snaps to the same bin regardless of where the camera
//   actually is, so the "snap" never tracks real sub-texel motion and
//   shadow shimmer would not actually be prevented. Projecting
//   center_world's raw (right, up, look_dir) coordinates first, snapping
//   those, and *then* placing light_pos from the snapped triple avoids the
//   self-cancellation and gives a light position that genuinely locks to
//   world-space texel boundaries (verified against sampo's non-degenerate
//   AABB-fit method, UpdateLightMatricesFromBounds, which sidesteps the same
//   trap by only ever shifting the eye along the look_dir axis before
//   re-projecting).
// - Snaps the light-space Z the same way (see UpdateLightMatrices) so the
//   FULL light_pos -- not just its (right, up) placement -- is a pure
//   function of center_world's snapped grid cell; z_range/light_proj_ are
//   already independent of center_world (fixed constants), so this closes
//   the last path by which center_world's continuous position could leak
//   into the matrix.
#include "engine/rendering/shadow_map.hpp"

#include <cmath>
#include <glm/gtc/matrix_transform.hpp>

namespace badlands {

ShadowMap::ShadowMap(wgpu::Device device, uint32_t resolution) {
  CreateTexture(device, resolution);
}

ShadowMap::ShadowMap(ShadowMap&& other) noexcept
    : depth_texture_(other.depth_texture_),
      depth_view_(other.depth_view_),
      light_view_(other.light_view_),
      light_proj_(other.light_proj_),
      light_view_proj_(other.light_view_proj_),
      texel_world_size_(other.texel_world_size_),
      resolution_(other.resolution_) {
  other.depth_texture_ = nullptr;
  other.depth_view_ = nullptr;
  other.resolution_ = 0;
}

ShadowMap& ShadowMap::operator=(ShadowMap&& other) noexcept {
  if (this != &other) {
    Destroy();
    depth_texture_ = other.depth_texture_;
    depth_view_ = other.depth_view_;
    light_view_ = other.light_view_;
    light_proj_ = other.light_proj_;
    light_view_proj_ = other.light_view_proj_;
    texel_world_size_ = other.texel_world_size_;
    resolution_ = other.resolution_;
    other.depth_texture_ = nullptr;
    other.depth_view_ = nullptr;
    other.resolution_ = 0;
  }
  return *this;
}

ShadowMap::~ShadowMap() { Destroy(); }

void ShadowMap::Destroy() {
  if (depth_texture_) {
    depth_texture_.Destroy();
    depth_texture_ = nullptr;
    depth_view_ = nullptr;
  }
}

void ShadowMap::CreateTexture(wgpu::Device device, uint32_t resolution) {
  resolution_ = resolution;

  wgpu::TextureDescriptor desc;
  desc.size = {resolution, resolution, 1};
  desc.format = wgpu::TextureFormat::Depth32Float;
  desc.usage = wgpu::TextureUsage::RenderAttachment |
              wgpu::TextureUsage::TextureBinding |
              wgpu::TextureUsage::CopySrc;
  desc.mipLevelCount = 1;
  desc.sampleCount = 1;
  desc.dimension = wgpu::TextureDimension::e2D;

  depth_texture_ = device.CreateTexture(&desc);
  depth_view_ = depth_texture_.CreateView();
}

void ShadowMap::UpdateLightMatrices(const glm::vec3& sun_dir,
                                    const glm::vec3& center_world,
                                    float coverage_dmax, uint32_t resolution,
                                    float backward_extension) {
  const float t_size =
      coverage_dmax / static_cast<float>(std::max(resolution, 1u));
  const float half = 0.5f * coverage_dmax;

  // Shadow camera looks FROM the sun TOWARD the scene; sun_dir points
  // TOWARD the sun (SceneContext::sun_direction's convention).
  glm::vec3 look_dir = -glm::normalize(sun_dir);

  glm::vec3 up(0.0f, 1.0f, 0.0f);
  if (std::abs(glm::dot(look_dir, up)) > 0.99f) {
    up = glm::vec3(0.0f, 0.0f, 1.0f);
  }
  glm::vec3 right = glm::normalize(glm::cross(up, look_dir));
  up = glm::normalize(glm::cross(look_dir, right));

  // Texel snap: project center_world onto the (right, up, look_dir) axes
  // directly, floor-snap each to the T_size grid, then place light_pos from
  // the snapped triple (see the file-level comment for why this must NOT
  // transform center_world through a view already translated to a
  // center_world-derived eye position -- that construction self-cancels).
  const float cx = glm::dot(right, center_world);
  const float cy = glm::dot(up, center_world);
  const float cz = glm::dot(look_dir, center_world);
  const float snapped_cx = std::floor(cx / t_size) * t_size;
  const float snapped_cy = std::floor(cy / t_size) * t_size;
  const float snapped_cz = std::floor(cz / t_size) * t_size;

  // Light sits (half + backward_extension) behind the snapped center along
  // look_dir. The whole position is now a pure function of center_world's
  // snapped grid cell (floor(cx/T), floor(cy/T), floor(cz/T)) -- required
  // for the byte-identical-matrix contract under sub-texel camera motion.
  glm::vec3 light_pos = right * snapped_cx + up * snapped_cy +
                        look_dir * (snapped_cz - (half + backward_extension));

  light_view_ = glm::lookAt(light_pos, light_pos + look_dir, up);

  // z_range is a FIXED constant (coverage_dmax + backward_extension only) --
  // it must NOT depend on center_world/scene bounds. Plain glm::ortho: under
  // GLM_FORCE_DEPTH_ZERO_TO_ONE this already produces depth in [0,1] with
  // near->0, far->1 (conventional Z) -- no depth-remap matrix.
  const float z_range = coverage_dmax + backward_extension;
  light_proj_ = glm::ortho(-half, half, -half, half, 0.0f, z_range);

  light_view_proj_ = light_proj_ * light_view_;
  texel_world_size_ = t_size;
}

}  // namespace badlands
