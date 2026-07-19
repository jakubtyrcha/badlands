// Shared EditorUI helpers (Task S2.B4). See editor_ui.hpp.
#include "engine/ui/editor_ui.hpp"

#include <cmath>

#include <glm/glm.hpp>
#include <imgui.h>

#include "engine/app/sim_clock.hpp"
#include "engine/rendering/daylight.hpp"
#include "engine/rendering/light_environment.hpp"
#include "engine/rendering/scene_renderer.hpp"

namespace badlands {

namespace {

// azimuth: angle around Y (0..360deg), elevation: angle above the XZ plane
// (-90..90deg). Matches the convention x=cos(el)*cos(az), y=sin(el),
// z=cos(el)*sin(az) — recomputed from `dir` every frame (stateless: no
// persistent slider state to fight discontinuities at the poles, at the cost
// of the azimuth being momentarily undefined when elevation hits +-90).
void DirectionToAzimuthElevation(const glm::vec3& dir, float& azimuth_deg,
                                 float& elevation_deg) {
  const glm::vec3 d = glm::normalize(dir);
  elevation_deg = glm::degrees(std::asin(glm::clamp(d.y, -1.0f, 1.0f)));
  azimuth_deg = glm::degrees(std::atan2(d.z, d.x));
  if (azimuth_deg < 0.0f) azimuth_deg += 360.0f;
}

glm::vec3 AzimuthElevationToDirection(float azimuth_deg, float elevation_deg) {
  const float az = glm::radians(azimuth_deg);
  const float el = glm::radians(elevation_deg);
  const float cos_el = std::cos(el);
  return glm::normalize(
      glm::vec3(cos_el * std::cos(az), std::sin(el), cos_el * std::sin(az)));
}

}  // namespace

namespace EditorUI {

bool DrawLightEnvironmentEditor(LightEnvironment& env) {
  bool changed = false;

  ImGui::TextUnformatted("Sun");
  float azimuth_deg = 0.0f, elevation_deg = 0.0f;
  DirectionToAzimuthElevation(env.sun_direction, azimuth_deg, elevation_deg);
  bool dir_changed = false;
  dir_changed |=
      ImGui::SliderFloat("Azimuth", &azimuth_deg, 0.0f, 360.0f, "%.1f deg");
  dir_changed |= ImGui::SliderFloat("Elevation", &elevation_deg, -90.0f, 90.0f,
                                    "%.1f deg");
  if (dir_changed) {
    env.sun_direction = AzimuthElevationToDirection(azimuth_deg, elevation_deg);
    changed = true;
  }
  changed |= ImGui::ColorEdit3("Sun Color", &env.sun_color.x);
  changed |= ImGui::SliderFloat("Sun Intensity", &env.sun_intensity, 0.0f, 20.0f);
  changed |= ImGui::SliderFloat("Sun Disc Size", &env.sun_disc_size, 0.001f,
                                0.2f, "%.3f rad");

  ImGui::Spacing();
  ImGui::TextUnformatted("Sky");
  changed |= ImGui::ColorEdit3("Sky Color", &env.sky_color.x);
  changed |= ImGui::ColorEdit3("Horizon Color", &env.horizon_color.x);
  changed |= ImGui::ColorEdit3("Ground Color", &env.ground_color.x);
  changed |= ImGui::SliderFloat("Sky Intensity", &env.sky_intensity, 0.0f, 5.0f);

  return changed;
}

void DrawGBufferDebugSelector(SceneRenderer& renderer) {
  GBufferDebugMode mode = renderer.GetDebugMode();

  ImGui::TextUnformatted("G-Buffer Debug");
  if (ImGui::RadioButton("None", mode == GBufferDebugMode::None)) {
    mode = GBufferDebugMode::None;
  }
  ImGui::SameLine();
  if (ImGui::RadioButton("Depth", mode == GBufferDebugMode::Depth)) {
    mode = GBufferDebugMode::Depth;
  }
  ImGui::SameLine();
  if (ImGui::RadioButton("Normals", mode == GBufferDebugMode::Normals)) {
    mode = GBufferDebugMode::Normals;
  }
  ImGui::SameLine();
  if (ImGui::RadioButton("Albedo", mode == GBufferDebugMode::Albedo)) {
    mode = GBufferDebugMode::Albedo;
  }
  if (ImGui::RadioButton("Roughness", mode == GBufferDebugMode::Roughness)) {
    mode = GBufferDebugMode::Roughness;
  }
  ImGui::SameLine();
  if (ImGui::RadioButton("HDR", mode == GBufferDebugMode::Hdr)) {
    mode = GBufferDebugMode::Hdr;
  }
  ImGui::SameLine();
  if (ImGui::RadioButton("AO", mode == GBufferDebugMode::Ao)) {
    mode = GBufferDebugMode::Ao;
  }

  renderer.SetDebugMode(mode);

  // GTAO screen-space AO toggle (M6). Drives SceneRenderer::SetGtaoEnabled;
  // final AO = min(GTAO, baked AO) when on, baked AO only when off.
  bool gtao_enabled = renderer.GetGtaoEnabled();
  if (ImGui::Checkbox("GTAO", &gtao_enabled)) {
    renderer.SetGtaoEnabled(gtao_enabled);
  }
}

void DrawShadowDebugSelector(SceneRenderer& renderer) {
  ShadowDebugMode mode = renderer.GetShadowDebugMode();

  ImGui::TextUnformatted("Shadow Debug");
  if (ImGui::RadioButton("Off", mode == ShadowDebugMode::Off)) {
    mode = ShadowDebugMode::Off;
  }
  ImGui::SameLine();
  if (ImGui::RadioButton("Combined", mode == ShadowDebugMode::Combined)) {
    mode = ShadowDebugMode::Combined;
  }
  ImGui::SameLine();
  if (ImGui::RadioButton("ShadowMapOnly", mode == ShadowDebugMode::ShadowMapOnly)) {
    mode = ShadowDebugMode::ShadowMapOnly;
  }
  ImGui::SameLine();
  if (ImGui::RadioButton("ContactOnly", mode == ShadowDebugMode::ContactOnly)) {
    mode = ShadowDebugMode::ContactOnly;
  }

  renderer.SetShadowDebugMode(mode);
}

void DrawFogEditor(SceneRenderer& renderer) {
  if (!ImGui::CollapsingHeader("Volumetric Fog", ImGuiTreeNodeFlags_DefaultOpen)) {
    return;
  }
  FogConfig& cfg = renderer.MutableFogConfig();

  ImGui::Checkbox("Enabled##fog", &cfg.enabled);

  // Cascades (changing count/res recreates the media texture next frame; the
  // vertical band [floor, floor+height] is runtime-configurable, default 0..64).
  ImGui::SeparatorText("Cascades");
  ImGui::SliderFloat("Cascade Extent", &cfg.layout.base_half_extent, 8.0f, 512.0f, "%.0f m");
  ImGui::SliderInt("Cascades", &cfg.layout.cascade_count, 1, 4);
  ImGui::SliderInt("Res XZ", &cfg.layout.res_xz, 32, 256);
  ImGui::SliderInt("Res Y", &cfg.layout.res_y, 8, 64);
  ImGui::SliderFloat("Band Floor Y", &cfg.layout.floor_y, -20.0f, 40.0f, "%.1f m");
  ImGui::SliderFloat("Band Height", &cfg.layout.height, 8.0f, 128.0f, "%.1f m");

  // Raymarch / lighting.
  ImGui::SeparatorText("Raymarch");
  ImGui::SliderInt("Steps", &cfg.step_count, 4, 128);
  ImGui::SliderFloat("Max Distance", &cfg.fog_max_distance, 20.0f, 2000.0f, "%.0f m");
  ImGui::SliderFloat("Phase g", &cfg.phase_g, -0.9f, 0.9f);
  ImGui::SliderFloat("Sun Scale", &cfg.sun_scale, 0.0f, 4.0f);
  ImGui::SliderFloat("Ambient Scale", &cfg.ambient_scale, 0.0f, 4.0f);

  // Stochastic / perf.
  ImGui::SeparatorText("Stochastic / perf");
  ImGui::Checkbox("Shafts", &cfg.enable_shafts);
  ImGui::SameLine();
  ImGui::Checkbox("Jitter", &cfg.jitter);
  ImGui::SameLine();
  ImGui::Checkbox("Half-res", &cfg.half_res);
}

bool DrawSimClockControls(SimClock& clock) {
  // Speed (0 = paused, 1/2/4x). Drives the day/night cycle (and, in the game, the
  // sim tick) via the shared clock.
  ImGui::Text("Speed:");
  const float kSpeeds[] = {0.0f, 1.0f, 2.0f, 4.0f};
  const char* kSpeedLabels[] = {"Pause", "1x", "2x", "4x"};
  for (int i = 0; i < 4; ++i) {
    ImGui::SameLine();
    if (ImGui::RadioButton(kSpeedLabels[i], clock.speed == kSpeeds[i])) {
      clock.speed = kSpeeds[i];
    }
  }

  bool seek = false;
  float t01 = clock.TimeOfDay();
  if (ImGui::SliderFloat("Time of day", &t01, 0.0f, 0.9999f)) {
    clock.SeekTimeOfDay(t01);
    seek = true;
  }
  ImGui::Text("Day %d  |  %05.2f h", clock.DayCounter(), t01 * 24.0f);
  ImGui::SliderFloat("Real sec / day", &clock.real_seconds_per_day, 1.0f, 600.0f);
  return seek;
}

bool DrawDaylightEditor(DaylightConfig& cfg) {
  bool changed = false;
  changed |= ImGui::SliderFloat("Turbidity", &cfg.turbidity, 1.0f, 10.0f);
  changed |= ImGui::SliderFloat("Ground albedo", &cfg.ground_albedo, 0.0f, 1.0f);
  changed |= ImGui::SliderFloat("Sky exposure", &cfg.sky_exposure, 0.001f, 0.3f, "%.3f");
  changed |= ImGui::SliderFloat("Sun intensity", &cfg.sun_intensity_max, 0.0f, 10.0f);
  changed |= ImGui::ColorEdit3("Moon color", &cfg.moon_color.x);
  changed |= ImGui::SliderFloat("Moon intensity", &cfg.moon_intensity, 0.0f, 0.2f, "%.3f");
  changed |= ImGui::SliderFloat("Ease minutes", &cfg.ease_ingame_minutes, 1.0f, 120.0f);
  changed |= ImGui::Checkbox("Moon disc", &cfg.moon_disc);
  return changed;
}

void DrawStats(float dt_seconds) {
  ImGui::Text("%.1f FPS (%.2f ms)", dt_seconds > 0.0f ? 1.0f / dt_seconds : 0.0f,
             dt_seconds * 1000.0f);
}

bool DrawDebugPanel(LightEnvironment& env, SceneRenderer& renderer,
                    float dt_seconds) {
  ImGui::Begin("Debug");
  DrawStats(dt_seconds);

  bool changed = false;
  if (ImGui::CollapsingHeader("Directional Light", ImGuiTreeNodeFlags_DefaultOpen)) {
    changed = DrawLightEnvironmentEditor(env);
  }
  DrawFogEditor(renderer);  // self-contained collapsing section
  if (ImGui::CollapsingHeader("Debug Views")) {
    DrawGBufferDebugSelector(renderer);
    ImGui::Separator();
    DrawShadowDebugSelector(renderer);
  }
  ImGui::End();
  return changed;
}

}  // namespace EditorUI

}  // namespace badlands
