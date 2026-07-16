// Daylight day/night driver — see daylight.hpp.
#include "engine/rendering/daylight.hpp"

#include <algorithm>
#include <cmath>

#include <glm/gtc/constants.hpp>
#include <spdlog/spdlog.h>

#include "core/math/spherical_harmonics.hpp"
#include "engine/rendering/context/scene_context.hpp"
#include "engine/rendering/cubemap_builder.hpp"
#include "engine/rendering/hosek_wilkie.hpp"

namespace badlands {

namespace {
constexpr float kInGameMinutesPerDay = 1440.0f;
// Keep cos(theta) > 0 for the HW zenith term; clamp the view zenith angle just
// under 90 degrees at/below the horizon.
constexpr float kThetaMax = glm::half_pi<float>() - 1.0e-3f;
}  // namespace

DaylightState ComputeDaylight(const DaylightConfig& cfg, float t01) {
  const float two_pi = glm::two_pi<float>();
  const float e_max = glm::radians(cfg.noon_elevation_deg);

  // Solar arc: t=0.25 sunrise (el=0, rising), 0.5 noon (+e_max), 0.75 sunset
  // (el=0, falling), 0.0 midnight (-e_max). Azimuth sweeps a half-turn across
  // the day (sunrise point -> opposite), passing overhead-south at noon.
  const float phase = two_pi * (t01 - 0.25f);
  const float el = e_max * std::sin(phase);
  const float az = glm::radians(cfg.azimuth_offset_deg) + phase;
  const float cos_el = std::cos(el);

  DaylightState s;
  s.time_of_day = t01;
  s.sun_direction =
      glm::vec3(cos_el * std::cos(az), std::sin(el), cos_el * std::sin(az));
  s.moon_direction = -s.sun_direction;  // anti-solar: up while the sun is down
  s.sun_elevation_rad = el;

  // Sun directional light: strength ramps in with elevation, colour warms
  // toward the horizon. Zero at/below the horizon.
  const float sun_full = glm::radians(cfg.sun_full_elev_deg);
  const float sun_int = cfg.sun_intensity_max * glm::smoothstep(0.0f, sun_full, el);
  const glm::vec3 sun_col = glm::mix(
      cfg.sun_color_horizon, cfg.sun_color_noon,
      glm::smoothstep(0.0f, glm::radians(30.0f), el));

  // Moon eases in over `ease_ingame_minutes` of solar descent below the
  // horizon (band = the elevation change over that window at the crossing).
  const float band = std::max(
      e_max * two_pi * (cfg.ease_ingame_minutes / kInGameMinutesPerDay), 1.0e-4f);
  const float moon_gate = glm::smoothstep(0.0f, band, -el);

  // Sun always wins while above the horizon; below it, swap to the eased moon.
  // The directional term is ~0 at the crossing (sun faded out, moon not yet
  // in), so the direction discontinuity is masked by the bright twilight sky.
  if (el >= 0.0f) {
    s.light_direction = s.sun_direction;
    s.light_color = sun_col;
    s.light_intensity = sun_int;
  } else {
    s.light_direction = s.moon_direction;
    s.light_color = cfg.moon_color;
    s.light_intensity = cfg.moon_intensity * moon_gate;
  }

  // HW sky is undefined below the horizon: clamp elevation and dim/tint toward
  // night as the sun sinks past the twilight band.
  s.hw_solar_elevation_rad = std::max(el, 0.0f);
  const float twilight = glm::radians(cfg.twilight_band_deg);
  const float day_factor = glm::smoothstep(-twilight, 0.0f, el);  // 1 day, 0 deep night
  s.sky_scale = cfg.sky_exposure * glm::mix(cfg.night_sky_factor, 1.0f, day_factor);
  s.sky_tint = glm::mix(cfg.night_tint, glm::vec3(1.0f), day_factor);
  return s;
}

void ApplyDaylightEnvironment(const DaylightState& state,
                              const DaylightConfig& cfg, wgpu::Device device,
                              wgpu::Queue queue, CubemapBuilder& sky_cube,
                              SceneContext& out, uint32_t face_size,
                              int sh_samples) {
  const HosekWilkieState hw = HosekWilkieInit(cfg.turbidity, cfg.ground_albedo,
                                              state.hw_solar_elevation_rad);
  const glm::vec3 sun_dir = glm::normalize(state.sun_direction);

  // Sun-free HW sky radiance for a view direction (shared by the cube and the
  // SH projection). theta measured from the zenith (+Y); below the horizon the
  // HW horizon value fades to a dark ground.
  auto sky_base = [&](glm::vec3 dir) -> glm::vec3 {
    dir = glm::normalize(dir);
    const double gamma =
        std::acos(std::clamp<double>(glm::dot(dir, sun_dir), -1.0, 1.0));
    float ground_fade = 1.0f;
    double theta;
    if (dir.y > 0.0f) {
      theta = std::acos(std::clamp<double>(dir.y, -1.0, 1.0));
      theta = std::min(theta, static_cast<double>(kThetaMax));
    } else {
      theta = kThetaMax;  // evaluate at the horizon
      ground_fade = 1.0f - std::clamp(-dir.y * 1.5f, 0.0f, 1.0f);
    }
    glm::vec3 L = HosekWilkieRadiance(hw, theta, gamma);
    return L * state.sky_scale * state.sky_tint * ground_fade;
  };

  // Full environment radiance = sky + an HDR sun disc/glow (day only), plus an
  // optional faint moon disc. The disc is NOT included in the SH projection
  // (avoids Monte-Carlo blow-up from the tiny solid angle — same rationale as
  // ApplyLightEnvironment).
  const float sun_up = glm::smoothstep(0.0f, glm::radians(3.0f), state.sun_elevation_rad);
  const glm::vec3 disc_col = cfg.sun_color_noon * cfg.sun_intensity_max;
  const float cos_disc = std::cos(0.02f);  // ~sun angular radius
  auto sky_radiance = [&](glm::vec3 dir) -> glm::vec4 {
    const glm::vec3 dn = glm::normalize(dir);
    glm::vec3 c = sky_base(dir);
    const float cosang = glm::dot(dn, sun_dir);
    const float disc =
        glm::smoothstep(cos_disc, glm::mix(cos_disc, 1.0f, 0.5f), cosang);
    const float glow = std::pow(std::max(cosang, 0.0f), 250.0f);
    c += disc_col * (disc * 6.0f + glow * 1.5f) * sun_up;
    if (cfg.moon_disc) {
      const float cosm = glm::dot(dn, glm::normalize(state.moon_direction));
      const float md =
          glm::smoothstep(cos_disc, glm::mix(cos_disc, 1.0f, 0.5f), cosm);
      c += cfg.moon_color * cfg.moon_intensity * 6.0f * md * (1.0f - sun_up);
    }
    return glm::vec4(c, 1.0f);
  };

  if (!sky_cube.Build(device, queue, face_size, sky_radiance)) {
    spdlog::error("ApplyDaylightEnvironment: failed to build sky cubemap");
  } else {
    out.skybox_cubemap = sky_cube.GetView();
    ++out.skybox_generation;
  }

  out.ambient_sh = sh::ProjectFunctionToSHL2(
      [&](glm::vec3 d) { return sky_base(d); }, sh_samples);

  out.sun_direction = glm::normalize(state.light_direction);
  out.sun_color = state.light_color * state.light_intensity;
}

}  // namespace badlands
