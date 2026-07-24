#include "executables/mapview/map_view_view.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <string>
#include <vector>

#include <SDL3/SDL.h>
#include <glm/glm.hpp>
#include <imgui.h>
#include <spdlog/spdlog.h>

#include "engine/app/sdl_input_util.hpp"
#include "engine/core/ray.hpp"
#include "engine/rendering/scene_renderer.hpp"  // GetFogSimulation / MutableFogConfig
#include "engine/ui/editor_ui.hpp"
#include "game/geometry/terrain_mesh.hpp"  // RaycastTerrain(MapData)
#include "mapgen/biomes.hpp"
#include "mapgen/fog_generator.hpp"

namespace badlands {

namespace {

// Wrap the generator output in the frozen MapData contract at the raster's own
// texel spacing. Slices are ONE-HOT: the hard per-pixel biome assignment, so
// WeightsAtNode(i,j).Dominant() == the single biome and the cluster terrain's
// per-vertex color is the crisp per-texel biome. Blended slices are the game's
// symbolic generator's business.
MapData MakeOneHotMapData(const mapgen::MapArtifacts& art, float texel_m) {
  const int sw = art.bedrock.width, sh = art.bedrock.height;
  if (sw <= 0 || sh <= 0 || texel_m <= 0.0f) return {};
  // One more node than texels per axis: node i sits at i * texel_m, so the
  // lattice spans exactly the map's size_m; edge nodes clamp to the last texel.
  MapData map(sw + 1, sh + 1, texel_m);
  for (int j = 0; j <= sh; ++j) {
    for (int i = 0; i <= sw; ++i) {
      const int sx = std::min(i, sw - 1), sz = std::min(j, sh - 1);
      map.mutable_height(i, j) = art.heightmap.at(sx, sz);
      map.mutable_slice(art.biome.at(sx, sz), i, j) = 255;
    }
  }
  return map;
}
}  // namespace

bool MapViewView::Initialize(const RenderContext& ctx) {
  device_ = ctx.device;
  queue_ = ctx.queue;
  scene_renderer_ = ctx.scene_renderer;  // shared debug/fog selectors need it

  // Map-load profiling: time each load step and log a per-step + cumulative
  // breakdown once. `log_step` accumulates into cum_ms; the closing TOTAL line is
  // the wall-clock span of the whole load (the tiny untimed bits -- camera
  // framing -- are the only gap between the two).
  using clock = std::chrono::steady_clock;
  auto since = [](clock::time_point t0) {
    return std::chrono::duration<double, std::milli>(clock::now() - t0).count();
  };
  const auto t_load = clock::now();
  double cum_ms = 0.0;
  auto log_step = [&](const char* name, double ms) {
    cum_ms += ms;
    spdlog::info("  {:<14} {:>8.1f} ms   (cum {:>8.1f} ms)", name, ms, cum_ms);
  };
  spdlog::info("map load profile (seed {}, {}x{} texels):", params_.seed,
               params_.resolution.x, params_.resolution.y);

  auto t = clock::now();
  // Start at noon, paused (an inspector holds still until you play/scrub).
  sim_clock_.speed = 0.0f;
  sim_clock_.SeekTimeOfDay(0.5f);
  ApplyDaylight();
  scene_context_.registry = &registry_;
  // Fog renders when enabled; sources come from the biome generator below.
  if (scene_renderer_) scene_renderer_->MutableFogConfig().enabled = true;
  log_step("daylight", since(t));

  // Build the map in-process — the same generator --preview-image-only dumps,
  // so the rendered terrain and the preview PNGs can never disagree.
  t = clock::now();
  map_ = mapgen::generate_map(params_);
  log_step("mg:generate", since(t));
  map_size_m_ = params_.size_m.x;

  // Wrap the generator output in the frozen MapData contract (one-hot biomes) at
  // the raster's own texel spacing -- the input to the cluster terrain and
  // picking. The cluster LOD's job is to decimate from full detail, so the leaf
  // lattice is the finest source data (one node per texel), not a coarser mesh
  // density; LOD selection manages the triangle cost.
  t = clock::now();
  terrain_map_ = MakeOneHotMapData(
      map_, params_.size_m.x / static_cast<float>(params_.resolution.x));
  log_step("map->MapData", since(t));

  // Frame the camera BEFORE building the terrain, so the cluster path's initial
  // LOD selection already runs against the real camera position rather than the
  // origin. Start on the map centre at ground-level framing, matching the game's
  // own camera (game_view.cpp: pitch 50, height 42) rather than a bird's-eye
  // view. Scroll to zoom out; max_height reaches far enough to take in the whole
  // map.
  const float map_depth_m = params_.size_m.y;
  gamecam_.focus = glm::vec3(map_size_m_ * 0.5f, 0.0f, map_depth_m * 0.5f);
  gamecam_.pitch_deg = 50.0f;
  gamecam_.height = 42.0f;
  gamecam_.min_height = 5.0f;
  gamecam_.max_height = std::max(400.0f, map_size_m_);
  // Headless framing override (--camera-height): clamp into the controller's
  // range so a far shot can pull well back without escaping it.
  if (camera_height_override_ > 0.0f) {
    gamecam_.max_height = std::max(gamecam_.max_height, camera_height_override_);
    gamecam_.height = std::clamp(camera_height_override_, gamecam_.min_height,
                                 gamecam_.max_height);
  }
  gamecam_.UpdateCamera(camera_);

  // Build the shared cluster-LOD terrain (identity model -- mapview vertices are
  // absolute world coords). --serial-build forces the single-threaded DAG build
  // for the perf A/B (both produce a bit-identical DAG). Seed the debug tint from
  // --lod-tint so a headless run renders tinted on frame one.
  cluster_terrain_.debug_tint_mode() = initial_tint_;
  TerrainClusterParams cluster_params;
  cluster_params.parallel_build = !serial_build_;
  t = clock::now();
  if (!cluster_terrain_.Build(terrain_map_, ctx, registry_, glm::mat4(1.0f),
                              cluster_params)) {
    spdlog::error("MapViewView: cluster terrain build failed");
    return false;
  }
  // Seed the LOD cut once so the first rendered frame (headless --screenshot
  // renders after a single Update) already draws the selected cut.
  cluster_terrain_.UpdateLod(camera_, screen_h_px_);
  log_step("cluster terrain", since(t));

  // Derive world-static fog emitters from the biome map (forest -> flat
  // elliptical fog, swamp -> granular noise fog) and push them to the fog sim.
  t = clock::now();
  fog_emitters_ = mapgen::GenerateBiomeFog(map_.biome, map_.heightmap, params_.seed);
  SetFogSources();
  log_step("fog", since(t));

  spdlog::info("map load: {:.1f} ms total  ({}x{} texels, {} fog emitters)",
               since(t_load), params_.resolution.x, params_.resolution.y,
               fog_emitters_.size());

  return true;
}

void MapViewView::SetFogSources() {
  if (scene_renderer_ == nullptr) return;
  const glm::vec2 map_min(0.0f, 0.0f);
  const glm::vec2 map_max(params_.size_m.x, params_.size_m.y);

  // Biome emitters (edited/picked) plus the world-static map-border wall,
  // appended after so they never shift the biome indices the editor addresses.
  // The border is a function of the map bounds only -- no camera -- so it stays
  // put as the view moves.
  std::vector<fog::Emitter> all = fog_emitters_;
  if (border_fog_enabled_) {
    const std::vector<fog::Emitter> border =
        mapgen::BuildBorderFog(map_min, map_max, border_fog_);
    all.insert(all.end(), border.begin(), border.end());
  }

  FogSimParams p;
  // The border OBBs straddle the edges, so pad the broadphase by the band so
  // their (slightly off-map) footprint still buckets.
  const float pad = border_fog_.band_m + 1.0f;
  p.map_min = map_min - glm::vec2(pad);
  p.map_max = map_max + glm::vec2(pad);
  p.bp_cell_size = 32.0f;
  scene_renderer_->GetFogSimulation().SetSources(all, p);
}

void MapViewView::ApplyDaylight() {
  const DaylightState state =
      ComputeDaylight(daylight_cfg_, sim_clock_.TimeOfDay());
  ApplyDaylightEnvironment(state, daylight_cfg_, device_, queue_, sky_cube_,
                           scene_context_);
}

void MapViewView::HandleEvent(const SDL_Event& event, int /*width*/,
                              int /*height*/) {
  if (ImGui::GetIO().WantCaptureMouse) return;

  // Mouse coords are logical points; HandleEvent's width/height are physical
  // pixels. EventWindowLogicalSize keeps both in one space (see its docs).
  switch (event.type) {
    case SDL_EVENT_MOUSE_MOTION: {
      glm::vec2 screen;
      if (!EventWindowLogicalSize(event.motion.windowID, screen)) {
        hover_valid_ = false;
        return;
      }
      const Ray ray = ScreenPointToRay(
          camera_, glm::vec2(event.motion.x, event.motion.y), screen);
      hover_valid_ = RaycastTerrain(terrain_map_, ray, hover_point_);
      break;
    }
    case SDL_EVENT_MOUSE_WHEEL: {
      glm::vec2 screen;
      if (!EventWindowLogicalSize(event.wheel.windowID, screen)) return;
      const glm::vec2 pixel(event.wheel.mouse_x, event.wheel.mouse_y);
      ZoomAtCursor(gamecam_, camera_, NormalizedWheelY(event.wheel), pixel,
                   screen);
      // The camera moved under a stationary cursor, so the hover point is stale
      // -- re-pick now rather than waiting for the next motion event (which may
      // never come if the user is only scrolling).
      const Ray ray = ScreenPointToRay(camera_, pixel, screen);
      hover_valid_ = RaycastTerrain(terrain_map_, ray, hover_point_);
      break;
    }
    case SDL_EVENT_MOUSE_BUTTON_DOWN: {
      if (event.button.button != SDL_BUTTON_LEFT) break;
      glm::vec2 screen;
      if (!EventWindowLogicalSize(event.button.windowID, screen)) break;
      const Ray ray = ScreenPointToRay(
          camera_, glm::vec2(event.button.x, event.button.y), screen);
      glm::vec3 hit;
      if (RaycastTerrain(terrain_map_, ray, hit)) selected_emitter_ = PickEmitter(hit);
      break;
    }
    default:
      break;
  }
}

int MapViewView::PickEmitter(const glm::vec3& world) const {
  // Which emitter footprint contains `world` (XZ), in its local frame. Nearest
  // centre wins on overlap. -1 = none (deselect).
  int best = -1;
  float best_d2 = 1e30f;
  for (size_t i = 0; i < fog_emitters_.size(); ++i) {
    const fog::Emitter& e = fog_emitters_[i];
    const glm::vec2 d = glm::vec2(world.x, world.z) - e.center;
    const float cs = std::cos(e.rotation), sn = std::sin(e.rotation);
    const glm::vec2 local(d.x * cs + d.y * sn, -d.x * sn + d.y * cs);  // rotate by -yaw
    const glm::vec2 nd(local.x / std::max(e.half_extent.x, 1e-4f),
                       local.y / std::max(e.half_extent.y, 1e-4f));
    const bool inside = (e.shape == fog::EmitterShape::Obb)
                            ? (std::abs(nd.x) <= 1.0f && std::abs(nd.y) <= 1.0f)
                            : (nd.x * nd.x + nd.y * nd.y <= 1.0f);  // disc/ellipse
    if (!inside) continue;
    const float d2 = d.x * d.x + d.y * d.y;
    if (d2 < best_d2) { best_d2 = d2; best = static_cast<int>(i); }
  }
  return best;
}

void MapViewView::Update(float dt, const bool* keyboard_state) {
  dt_ = dt;

  // Advance the shared clock; when it's running, move the sun and animate the
  // fog by the same sim delta (paused => both hold). The daylight re-bake is
  // throttled implicitly: ApplyDaylight only runs while time actually moves.
  const double sim_dt = sim_clock_.Advance(dt);
  if (sim_dt > 0.0) {
    ApplyDaylight();
    if (scene_renderer_) {
      scene_renderer_->GetFogSimulation().AddTime(static_cast<float>(sim_dt));
    }
  }

  if (keyboard_state != nullptr && ImGui::GetCurrentContext() != nullptr &&
      !ImGui::GetIO().WantCaptureKeyboard) {
    glm::vec2 dir(0.0f);
    if (keyboard_state[SDL_SCANCODE_W] || keyboard_state[SDL_SCANCODE_UP]) dir.y -= 1.0f;
    if (keyboard_state[SDL_SCANCODE_S] || keyboard_state[SDL_SCANCODE_DOWN]) dir.y += 1.0f;
    if (keyboard_state[SDL_SCANCODE_A] || keyboard_state[SDL_SCANCODE_LEFT]) dir.x -= 1.0f;
    if (keyboard_state[SDL_SCANCODE_D] || keyboard_state[SDL_SCANCODE_RIGHT]) dir.x += 1.0f;
    gamecam_.PanKeyboard(dir, dt);  // zoom-scaled; no-op when dir is zero
  }
  gamecam_.UpdateCamera(camera_);

  // Re-select the LOD cluster cut for the new camera and rewrite the draw
  // ranges. Cheap flat pass over the DAG; no buffer re-upload.
  cluster_terrain_.UpdateLod(camera_, screen_h_px_);

  // NB: the fog sources are NOT rebuilt per frame -- both the biome fog and the
  // map-border wall are world-static, so they're uploaded once (Initialize) and
  // only on edit (the editor). Nothing here depends on the camera.

  // Rebuild the debug-line overlay each frame: just the selected fog emitter's
  // OBB now. Small and camera-independent, so rebuilding is cheap.
  overlay_.Clear();
  if (selected_emitter_ >= 0 &&
      selected_emitter_ < static_cast<int>(fog_emitters_.size())) {
    const fog::Emitter& e = fog_emitters_[selected_emitter_];
    overlay_.AddOrientedBox(e.center, e.rotation, e.half_extent, e.base_y,
                            e.base_y + e.height, glm::vec3(1.0f, 0.25f, 0.9f),
                            2.5f);
  }
  scene_context_.debug_lines = overlay_.empty() ? nullptr : &overlay_;
}

void MapViewView::DrawUI() {
  if (ImGui::GetCurrentContext() == nullptr) return;
  ImGui::Begin("Map");
  ImGui::Text("seed %u  %dx%d texels  %.0fx%.0f m", params_.seed,
              params_.resolution.x, params_.resolution.y, params_.size_m.x,
              params_.size_m.y);
  cluster_terrain_.DrawDebugUI();
  ImGui::Text("focus: (%.0f, %.0f)", gamecam_.focus.x, gamecam_.focus.z);
  if (hover_valid_) {
    const std::string_view bn = mapgen::biome_name(
        terrain_map_.DominantBiomeAt(hover_point_.x, hover_point_.z));
    ImGui::Text("hover: (%.1f, %.1f, %.1f)  %.*s", hover_point_.x,
                hover_point_.y, hover_point_.z, static_cast<int>(bn.size()),
                bn.data());
  } else {
    ImGui::TextUnformatted("hover: (off terrain)");
  }
  ImGui::End();

  // Shared sim/daylight/debug controls (same helpers the game uses). Re-bake the
  // sky immediately on a scrub or a config edit so it's visible without waiting
  // for the clock; while playing, Update already re-bakes as time advances.
  ImGui::Begin("Sim / Daylight / Debug");
  if (ImGui::CollapsingHeader("Simulation", ImGuiTreeNodeFlags_DefaultOpen)) {
    if (EditorUI::DrawSimClockControls(sim_clock_)) ApplyDaylight();
  }
  if (ImGui::CollapsingHeader("Directional Light", ImGuiTreeNodeFlags_DefaultOpen)) {
    if (EditorUI::DrawDaylightEditor(daylight_cfg_)) ApplyDaylight();
  }
  if (scene_renderer_ != nullptr) {
    EditorUI::DrawFogEditor(*scene_renderer_);
    if (ImGui::CollapsingHeader("Debug Views")) {
      EditorUI::DrawGBufferDebugSelector(*scene_renderer_);
      EditorUI::DrawShadowDebugSelector(*scene_renderer_);
    }
  }
  EditorUI::DrawStats(dt_);
  ImGui::End();

  DrawFogEmitterEditor();
}

void MapViewView::DrawFogEmitterEditor() {
  ImGui::Begin("Fog Emitters");
  ImGui::Text("%zu emitters   (click the terrain to select)", fog_emitters_.size());
  if (ImGui::Button("Regenerate from biomes")) {
    fog_emitters_ =
        mapgen::GenerateBiomeFog(map_.biome, map_.heightmap, params_.seed);
    selected_emitter_ = -1;
    SetFogSources();
  }
  ImGui::SameLine();
  if (ImGui::Button("Deselect")) selected_emitter_ = -1;

  // World-static fog wall around the map perimeter (does not move with the camera).
  if (ImGui::CollapsingHeader("Border fog (map edge)", ImGuiTreeNodeFlags_DefaultOpen)) {
    bool changed = ImGui::Checkbox("Enabled##border", &border_fog_enabled_);
    changed |= ImGui::SliderFloat("Band (m)", &border_fog_.band_m, 4.0f, 128.0f);
    changed |= ImGui::SliderFloat("Ramp (m)", &border_fog_.ramp_m, 0.0f, 32.0f);
    changed |= ImGui::SliderFloat("Magnitude##border", &border_fog_.magnitude, 0.0f, 0.5f, "%.3f");
    changed |= ImGui::SliderFloat("Height##border", &border_fog_.height_m, 1.0f, 128.0f);
    if (changed) SetFogSources();  // reflect immediately (also removes the wall when off)
  }

  if (selected_emitter_ < 0 ||
      selected_emitter_ >= static_cast<int>(fog_emitters_.size())) {
    ImGui::TextUnformatted("No emitter selected.");
    ImGui::End();
    return;
  }

  fog::Emitter& e = fog_emitters_[selected_emitter_];
  ImGui::SeparatorText("Selected emitter");
  ImGui::Text("#%d  center (%.0f, %.0f)", selected_emitter_, e.center.x, e.center.y);

  bool changed = false;
  const char* kShapes[] = {"Disc", "OBB", "Ellipse"};
  int shape = static_cast<int>(e.shape);
  if (ImGui::Combo("Shape", &shape, kShapes, 3)) {
    e.shape = static_cast<fog::EmitterShape>(shape);
    changed = true;
  }
  const char* kTypes[] = {"Flat (Disc)", "Noise"};
  int type = static_cast<int>(e.type);
  if (ImGui::Combo("Type", &type, kTypes, 2)) {
    e.type = static_cast<fog::EmitterType>(type);
    changed = true;
  }
  changed |= ImGui::DragFloat2("Half extent", &e.half_extent.x, 0.1f, 0.5f, 200.0f);
  changed |= ImGui::SliderAngle("Rotation", &e.rotation);
  changed |= ImGui::DragFloat("Base Y", &e.base_y, 0.1f);
  changed |= ImGui::SliderFloat("Height", &e.height, 1.0f, 128.0f);
  changed |= ImGui::SliderFloat("Magnitude", &e.magnitude, 0.0f, 0.5f, "%.3f");
  changed |= ImGui::SliderFloat("Radial falloff", &e.radial_falloff, 0.0f, 1.0f);
  changed |= ImGui::SliderFloat("Vertical falloff", &e.vertical_falloff, 0.0f, 1.0f);
  if (e.type == fog::EmitterType::Noise) {
    changed |= ImGui::SliderFloat("Noise freq", &e.noise_freq, 0.0f, 1.0f);
    changed |= ImGui::SliderFloat("Noise contrast", &e.noise_contrast, 0.1f, 4.0f);
    changed |= ImGui::DragFloat3("Scroll", &e.scroll.x, 0.05f);
  }
  if (changed) SetFogSources();  // re-upload the whole set
  ImGui::End();
}

void MapViewView::OnResize(int width, int height) {
  camera_.aspect =
      height > 0 ? static_cast<float>(width) / static_cast<float>(height) : 1.0f;
  // The LOD screen-space-error metric is in pixels, so it needs the viewport
  // height in pixels -- exactly what OnResize carries (physical pixels windowed,
  // the capture height headless).
  if (height > 0) screen_h_px_ = static_cast<float>(height);
}

}  // namespace badlands
