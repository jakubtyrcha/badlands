// Shared EditorUI helpers (Task S2.B4). See editor_ui.hpp.
#include "engine/ui/editor_ui.hpp"

#include <cmath>

#include <glm/glm.hpp>
#include <imgui.h>

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

  renderer.SetDebugMode(mode);
}

void DrawStats(float dt_seconds) {
  ImGui::Text("%.1f FPS (%.2f ms)", dt_seconds > 0.0f ? 1.0f / dt_seconds : 0.0f,
             dt_seconds * 1000.0f);
}

bool DrawDebugPanel(LightEnvironment& env, SceneRenderer& renderer,
                    float dt_seconds) {
  ImGui::Begin("Debug");
  DrawStats(dt_seconds);
  ImGui::Separator();
  const bool changed = DrawLightEnvironmentEditor(env);
  ImGui::Separator();
  DrawGBufferDebugSelector(renderer);
  ImGui::End();
  return changed;
}

}  // namespace EditorUI

}  // namespace badlands
