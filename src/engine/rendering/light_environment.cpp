// LightEnvironment helper (Task S2.B3). See light_environment.hpp.
#include "engine/rendering/light_environment.hpp"

#include <algorithm>
#include <cmath>

#include <spdlog/spdlog.h>

#include "core/math/spherical_harmonics.hpp"
#include "engine/rendering/context/scene_context.hpp"
#include "engine/rendering/cubemap_builder.hpp"

namespace badlands {

namespace {

// Sun-free sky-dome gradient: sky_color overhead -> horizon_color at the
// horizon -> ground_color below, scaled by sky_intensity. Shared by the SH
// projection (stable — no intense sun disc) and the environment-cube radiance.
glm::vec3 GradientRadiance(const LightEnvironment& env, glm::vec3 dir) {
  const float up = glm::normalize(dir).y;  // -1..1
  glm::vec3 c;
  if (up >= 0.0f) {
    const float t = std::pow(1.0f - up, 2.5f);  // bias toward the horizon
    c = glm::mix(env.sky_color, env.horizon_color, glm::clamp(t, 0.0f, 1.0f));
  } else {
    c = glm::mix(env.horizon_color, env.ground_color,
                 glm::clamp(-up * 1.5f, 0.0f, 1.0f));
  }
  return c * env.sky_intensity;
}

}  // namespace

void ApplyLightEnvironment(const LightEnvironment& env, wgpu::Device device,
                           wgpu::Queue queue, CubemapBuilder& sky_cube,
                           SceneContext& out, uint32_t face_size) {
  const glm::vec3 sun_dir = glm::normalize(env.sun_direction);
  const float cos_disc = std::cos(std::max(env.sun_disc_size, 1.0e-4f));
  const glm::vec3 sun_radiance = env.sun_color * env.sun_intensity;

  // Full environment radiance = sky gradient + a bright HDR sun disc (sharp
  // core + a wider halo) toward the sun. HDR so it reads as a sun in the
  // skybox and drives a visible IBL reflection; the tonemap clamps the core.
  auto sky_radiance = [&](glm::vec3 dir) -> glm::vec4 {
    glm::vec3 c = GradientRadiance(env, dir);
    const float cosang = glm::dot(glm::normalize(dir), sun_dir);
    const float disc =
        glm::smoothstep(cos_disc, glm::mix(cos_disc, 1.0f, 0.5f), cosang);
    const float glow = std::pow(glm::max(cosang, 0.0f), 250.0f);
    c += sun_radiance * (disc * 6.0f + glow * 1.5f);
    return glm::vec4(c, 1.0f);
  };

  // Environment cube for the skybox background + IBL prefilter source.
  if (!sky_cube.Build(device, queue, face_size, sky_radiance)) {
    spdlog::error("ApplyLightEnvironment: failed to build sky cubemap");
  } else {
    out.skybox_cubemap = sky_cube.GetView();
    ++out.skybox_generation;
  }

  // Ambient SH from the sun-free gradient (see header rationale).
  out.ambient_sh = sh::ProjectFunctionToSHL2(
      [&](glm::vec3 d) { return GradientRadiance(env, d); }, 2048);

  out.sun_direction = sun_dir;
  out.sun_color = sun_radiance;  // sun_color * sun_intensity
}

}  // namespace badlands
