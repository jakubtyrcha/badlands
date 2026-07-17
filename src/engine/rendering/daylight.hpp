#pragma once

// Daylight — day/night cycle driver (Task: daylight system).
//
// Game-agnostic engine code. Works purely in NORMALIZED time-of-day
// `t01 in [0,1)` (t=0 midnight, 0.25 sunrise, 0.5 noon, 0.75 sunset) — it knows
// nothing about "16 seconds"; the game layer owns that policy and the clock.
//
// Two pieces:
//   * ComputeDaylight(cfg, t01) -> DaylightState : pure celestial + handover
//     math (sun/moon direction, the effective directional light, and the
//     Hosek-Wilkie sky inputs). No GPU, no side effects — unit-testable.
//   * ApplyDaylightEnvironment(...) : bakes the HW sky into the source cube +
//     projects SH ambient + writes the directional light into a SceneContext.
//     Mirrors ApplyLightEnvironment (light_environment.hpp).
#include <cstdint>

#include <dawn/webgpu_cpp.h>
#include <glm/glm.hpp>

namespace badlands {

class CubemapBuilder;
struct SceneContext;

// Tunable configuration for the day/night cycle. Colours are linear RGB.
struct DaylightConfig {
  // Solar arc (aesthetic).
  float noon_elevation_deg = 80.0f;   // peak sun elevation above the horizon
  float azimuth_offset_deg = 0.0f;    // yaw of the sunrise point

  // Sun directional light.
  float sun_intensity_max = 4.0f;
  float sun_full_elev_deg = 6.0f;     // elevation at which the sun is at full strength
  glm::vec3 sun_color_noon{1.0f};
  glm::vec3 sun_color_horizon{1.0f, 0.6f, 0.35f};  // golden hour

  // Hosek-Wilkie sky.
  float turbidity = 3.0f;             // [1,10], clear sky ~2-3
  float ground_albedo = 0.15f;        // [0,1]
  float sky_exposure = 0.05f;         // HW radiance -> engine HDR scale

  // Moon. Physical sun:moon illuminance is ~6 orders of magnitude, which is
  // unrepresentable without banding/tonemap collapse — moon_intensity is
  // artificially bumped so the ratio is ~1.5 orders (0.1 vs 4.0 = 40x), the
  // bright end of the intended 2-3 order compression, so night reads as a dim
  // moonlit scene rather than black.
  glm::vec3 moon_color{0.5f, 0.62f, 1.0f};
  float moon_intensity = 0.1f;

  // Sun<->moon handover + night. The sun always takes precedence while above
  // the horizon; the moon eases in over `ease_ingame_minutes` of the 1440-min
  // day once the sun drops below the horizon (and eases out before dawn).
  float ease_ingame_minutes = 15.0f;
  float twilight_band_deg = 8.0f;     // sky darkening ramp below the horizon
  float night_sky_factor = 0.08f;     // min sky scale at deep night (dim ambient fill)
  glm::vec3 night_tint{0.6f, 0.7f, 1.0f};
  bool moon_disc = false;             // draw a faint moon disc in the sky (stretch)
};

// Everything a renderer needs for one instant of the cycle.
struct DaylightState {
  float time_of_day = 0.0f;                 // the input t01, for reference
  glm::vec3 sun_direction{0, 1, 0};         // toward the sun (may be below horizon)
  glm::vec3 moon_direction{0, -1, 0};       // toward the moon (anti-solar)
  float sun_elevation_rad = 0.0f;           // signed; <0 = below horizon

  // The single effective directional light (sun while up, else the eased moon).
  glm::vec3 light_direction{0, 1, 0};
  glm::vec3 light_color{1};
  float light_intensity = 0.0f;

  // Hosek-Wilkie sky inputs, consumed by ApplyDaylightEnvironment.
  float hw_solar_elevation_rad = 0.0f;      // clamped >= 0 (HW is undefined below horizon)
  float sky_scale = 1.0f;                    // sky_exposure folded with the night dimming
  glm::vec3 sky_tint{1};                     // white by day -> night_tint at deep night
};

// Pure. Maps a normalized time-of-day to sun/moon geometry, the blended
// directional light, and the HW sky inputs.
DaylightState ComputeDaylight(const DaylightConfig& cfg, float t01);

// (Re)builds the scene's sky/IBL/ambient/directional-light from `state`:
//   * bakes the HW sky (+ optional sun/moon disc) into `sky_cube` and sets
//     out.skybox_cubemap + bumps out.skybox_generation (forces IBL re-prefilter);
//   * projects the sun-free HW sky to SH L2 ambient (out.ambient_sh);
//   * writes out.sun_direction / out.sun_color = the effective directional light.
// Not cheap (CPU per-texel HW eval + SH projection); the game layer throttles
// how often it is called. `face_size` keeps the dynamic IBL at 128 by default.
void ApplyDaylightEnvironment(const DaylightState& state,
                              const DaylightConfig& cfg, wgpu::Device device,
                              wgpu::Queue queue, CubemapBuilder& sky_cube,
                              SceneContext& out, uint32_t face_size = 128,
                              int sh_samples = 512);

}  // namespace badlands
