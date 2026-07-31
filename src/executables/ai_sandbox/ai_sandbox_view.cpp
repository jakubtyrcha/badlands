#include "executables/ai_sandbox/ai_sandbox_view.hpp"

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <string>

#include <SDL3/SDL.h>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>
#include <imgui.h>
#include <spdlog/spdlog.h>

#include "engine/app/fixed_timestep.hpp"          // kTickDt
#include "engine/app/game_camera_controller.hpp"  // ZoomAtCursor
#include "engine/app/sdl_input_util.hpp"
#include "engine/rendering/geometry/building_parts_builder.hpp"
#include "engine/rendering/geometry/primitive_mesh_builders.hpp"
#include "engine/rendering/scene_build.hpp"
#include "engine/rendering/scene_renderer.hpp"
#include "engine/ui/editor_ui.hpp"
#include "game/brain_asset.hpp"
#include "game/building_catalog.h"
#include "game/creature_manifest.h"
#include "game/scene/blockout_materials.hpp"
#include "game/skill_manifest.hpp"

namespace badlands {

namespace {

// Debug materials come from the shared blockout palette (game/scene/
// blockout_materials.hpp) so the game's blockout mode and this view draw from
// one source of truth: blockout::kArenaGray/kArenaRoughness for the floor,
// blockout::kWall/kRoof for the buildings, blockout::kCapsule* for the
// entity capsules.

// Repeat the floor UVs roughly once per 2 world units instead of stretching
// one copy across the whole floor.
constexpr float kFloorUvRepeatSpacing = 2.0f;

// Capsule dimensions (world units).
constexpr float kCapsuleRadius = 0.35f;
constexpr float kCapsuleCylinderHeight = 0.6f;

// Snapshot buffer caps. A mode's world is small; a truncated snapshot would
// only mean fewer rows drawn/listed, never a crash.
constexpr uint32_t kMaxCharacterRows = 64;
constexpr uint32_t kMaxBuildingRows = 64;
constexpr uint32_t kMaxCommandRows = 24;

// Activity names come from badlands::ActivityCatalog() -- the sim's own single
// source of truth. This used to be a hand-mirrored switch, which silently
// returned "-" for every activity added after it was written; the catalog
// cannot rot that way.
const char* behavior_name(int32_t behavior) { return badlands::ActivityName(behavior); }

// Band label, for grouping the statistics panel.
const char* band_name(badlands::ActivityBand band) {
  switch (band) {
    case badlands::ActivityBand::Danger: return "danger";
    case badlands::ActivityBand::Normal: return "normal";
    default: return "?";
  }
}

// Archetype label (badlands::Archetype).
const char* archetype_name(int32_t a) {
  switch (a) {
    case 0: return "hero";
    case 1: return "townfolk";
    case 2: return "critter";
    case 3: return "monster";
    default: return "?";
  }
}

const char* command_name(badlands::CommandKindId kind) {
  switch (kind) {
    case badlands::CommandKindId::PlaceBuilding: return "PlaceBuilding";
    case badlands::CommandKindId::RecruitHero: return "RecruitHero";
    case badlands::CommandKindId::DestroyBuilding: return "DestroyBuilding";
    case badlands::CommandKindId::MoveTo: return "MoveTo";
    case badlands::CommandKindId::EnterBuilding: return "EnterBuilding";
    case badlands::CommandKindId::EnterHome: return "EnterHome";
    case badlands::CommandKindId::Buy: return "Buy";
    case badlands::CommandKindId::Attack: return "Attack";
    case badlands::CommandKindId::SetBehavior: return "SetBehavior";
    case badlands::CommandKindId::CollectTax: return "CollectTax";
    case badlands::CommandKindId::Deposit: return "Deposit";
    case badlands::CommandKindId::AttackBuilding: return "AttackBuilding";
    case badlands::CommandKindId::Chat: return "Chat";
    default: return "?";
  }
}

// Day length, as ONE number driving both clocks: the rendered day/night cycle
// (SimClock::real_seconds_per_day) and the sim's own day
// (WorldConfig::millis_per_day). Kept short here -- this is the AI observation
// tool, where waiting out a hero's need cycle in real time is the whole cost of
// a run. SimClock's own 300 s default would stretch that 2.5x.
constexpr float kRealSecondsPerDay = 120.0f;

}  // namespace

bool AiSandboxView::Initialize(const RenderContext& ctx) {
  device_ = ctx.device;
  queue_ = ctx.queue;
  scene_renderer_ = ctx.scene_renderer;

  if (!matlib_.Initialize(ctx.device, ctx.queue, ctx.pipeline_gen)) {
    spdlog::error("AiSandboxView::Initialize: MaterialLibrary init failed");
    return false;
  }

  ApplyEnvironment();

  StageWorld();  // the mode's world, built and populated
  BuildScene();

  // No volumetric fog over the greybox arena (it hazes the far edges). The
  // engine's config fog defaults on; disable it here as GameView does.
  if (scene_renderer_) {
    scene_renderer_->MutableFogConfig().enabled = false;
  }

  // Frame the camera once, here (the framing is aspect-independent -- see
  // FrameCamera). OnResize only refreshes camera_.aspect afterwards.
  FrameCamera();

  if (!matlib_.ok()) {
    spdlog::error(
        "AiSandboxView::Initialize: material pack(s) failed to load");
    return false;
  }
  return true;
}

void AiSandboxView::ApplyEnvironment() {
  ApplyLightEnvironment(env_, device_, queue_, sky_cube_, scene_context_);
  scene_.SetSunDirection(scene_context_.sun_direction);
  scene_.SetSunColor(scene_context_.sun_color);
  scene_.SetAmbientSH(scene_context_.ambient_sh);
}

void AiSandboxView::StageWorld() {
  // The hero brain, loaded ONCE into a member. Load-bearing that it outlives
  // this call: every restage builds a new Sim, and the wasm module is the only
  // hero decision layer there is (game/src/wasm_brain.h) -- a hero without it
  // issues no intentions and no actions at all, stands still, and loses every
  // fight to anything with a brain.
  if (hero_wasm_.empty()) {
    hero_wasm_ = badlands::LoadBrainWasm(badlands::kHeroBrainPath);
    if (hero_wasm_.empty()) {
      spdlog::error(
          "AiSandboxView: no hero brain at {} -- heroes will idle. Run from the "
          "repo root with git-lfs fetched.",
          badlands::kHeroBrainPath);
    } else {
      spdlog::info("AiSandboxView: {} loaded ({} bytes) -- heroes think via the wasm brain",
                   badlands::kHeroBrainPath, hero_wasm_.size());
    }
  }
  badlands::BrainDesc brain_desc{};
  if (!hero_wasm_.empty()) {
    brain_desc.wasm_bytes = hero_wasm_.data();
    brain_desc.wasm_len = hero_wasm_.size();
  }

  // The two clocks, from the one number: the sky (sim_clock_) and the sim's own
  // day (millis_per_day). Left unset they disagree -- heroes would act on night
  // behaviour while the sky showed something else. MillisPerDayForSimSeconds
  // converts through ticks; seconds * 1000 would drift ~1% (see badlands_sim.hpp).
  sim_clock_.real_seconds_per_day = kRealSecondsPerDay;

  // The mode says what world to build; everything past this point is the game
  // running normally, and nothing in it knows a mode exists.
  badlands::WorldConfig cfg = mode_->Configure();
  cfg.millis_per_day = badlands::MillisPerDayForSimSeconds(kRealSecondsPerDay);
  sim_ = badlands::Sim(cfg, brain_desc);

  // Creature stats and skill templates as data, same as game_view.cpp -- both
  // apps must agree on them (a missing file keeps the compiled defaults). Must
  // happen before staging: they are initial config, not live tuning.
  badlands::CreatureCatalog catalog = sim_.Creatures();
  if (badlands::LoadCreatureCatalog("assets/creatures/creatures.json", catalog)) {
    sim_.SetCreatureCatalog(catalog);
  }
  badlands::SkillCatalog skills = sim_.Skills();
  if (badlands::LoadSkillCatalog("assets/skills/skills.json", skills)) {
    sim_.SetSkillCatalog(skills);
  }

  mode_->Stage(sim_);

  building_rows_.resize(kMaxBuildingRows);
  cmd_rows_.resize(kMaxCommandRows);
  // Both clocks restart with the world, but the SPEED control does not: it is
  // the observer's setting, not the world's.
  sim_ticks_done_ = 0;
  sim_clock_.sim_seconds = 0.0;
}

void AiSandboxView::BuildScene() {
  // Fresh graph: re-mirror scene_context_'s (already-derived-from-env_)
  // lighting right after, same as ApplyEnvironment does for the live-edit
  // path (SceneGraph's constructor resets sun/ambient to its own defaults).
  scene_ = SceneGraph();
  scene_.SetSunDirection(scene_context_.sun_direction);
  scene_.SetSunColor(scene_context_.sun_color);
  scene_.SetAmbientSH(scene_context_.ambient_sh);

  // Floor spans whatever the mode built, with headroom.
  const float floor_size = 2.0f * WorldHalfExtent() + 8.0f;
  AddFloor(scene_, floor_size, matlib_.SolidColor(blockout::kArenaGray, blockout::kArenaRoughness),
           floor_size / kFloorUvRepeatSpacing);

  // No AddWalls(): a wall is a BUILDING now, so it arrives through
  // AddBuildings() with everything else the world contains. The view has no
  // idea an arena is what it is drawing.
  AddBuildings();
}

void AiSandboxView::AddBuildings() {
  

  // Blockout building parts: the same BuildBuildingParts assembly the detailed
  // path uses (building_scene.cpp), but with the flat debug palette instead of
  // PBR packs -- the sandbox is a greybox view of the sim, not a beauty shot.
  const DeferredMaterial wall_mat =
      matlib_.SolidColor(blockout::kWall, blockout::kBuildingRoughness);
  const DeferredMaterial roof_mat =
      matlib_.SolidColor(blockout::kRoof, blockout::kBuildingRoughness);

  building_rows_ = sim_.Buildings();
  const uint32_t count =
      std::min(static_cast<uint32_t>(building_rows_.size()), kMaxBuildingRows);
  int part_index = 0;
  for (uint32_t i = 0; i < count; ++i) {
    const badlands::BuildingState& b = building_rows_[i];
    const BuildingVisual bv = building_visual(static_cast<badlands::BuildingKind>(b.kind));
    const badlands::RenderBox box = badlands::RenderBoxOf(static_cast<badlands::BuildingKind>(b.kind), 0);
    const badlands::RenderBox placed = badlands::RenderBoxOf(static_cast<badlands::BuildingKind>(b.kind), b.rotation_index);

    const glm::mat4 transform =
        glm::translate(glm::mat4(1.0f), glm::vec3(b.center_x, 0.0f, b.center_z)) *
        glm::rotate(glm::mat4(1.0f), placed.yaw_radians, glm::vec3(0.0f, 1.0f, 0.0f));

    for (BuildingPart& part :
         BuildBuildingParts(box.size_x, box.size_z, bv.height, bv.roof)) {
      const std::string name = "building_part_" + std::to_string(part_index++);
      AddMeshEntity(scene_, name.c_str(), std::move(part.mesh),
                    part.kind == BuildingPartKind::Wall ? wall_mat : roof_mat,
                    transform);
    }
  }
}

void AiSandboxView::SyncUnits() {
  // Rebuild the (few) unit capsules each frame from the snapshot, coloured by
  // each entity's own colour so deer (brown) read distinctly from heroes (blue).
  // A fixed recoloured pool would be faster, but the sandbox holds a handful of
  // units and SceneGraph has no cheap per-node material swap -- rebuild is the
  // simple, correct choice for a debug view. Hidden (inside-building) units are
  // skipped, matching the sim's "don't draw; list in the panel" contract.
  for (NodeHandle n : capsule_nodes_) {
    scene_.DestroyNode(n);
  }
  capsule_nodes_.clear();

  char_rows_ = sim_.Characters();
  int index = 0;
  for (const badlands::CharacterState& c : char_rows_) {
    if (c.inside_building_id >= 0) {
      continue;  // hidden
    }
    auto capsule = GenerateCapsule(kCapsuleRadius, kCapsuleCylinderHeight, 16);
    const DeferredMaterial mat = matlib_.SolidColor(
        glm::vec3(c.color_r, c.color_g, c.color_b), blockout::kCapsuleRoughness);
    const glm::mat4 xf = glm::translate(glm::mat4(1.0f), glm::vec3(c.pos_x, 0.0f, c.pos_z));
    const std::string name = "unit_" + std::to_string(index++);
    capsule_nodes_.push_back(
        AddMeshEntity(scene_, name.c_str(), std::move(capsule), mat, xf));
  }

  cmd_rows_ = sim_.CommandLog();
  command_log_total_ = static_cast<uint32_t>(cmd_rows_.size());
}

void AiSandboxView::SyncProjectiles() {
  // Debug-line tracers: a thin yellow box per in-flight shot. Pooled and updated
  // IN PLACE -- the geometry never changes shape, only its transform, so a fixed
  // set of reusable nodes avoids the per-frame mesh upload + node churn. Grow the
  // pool on demand; park unused entries at zero scale rather than destroying them.
  projectile_rows_ = sim_.Projectiles();
  const DeferredMaterial mat =
      matlib_.SolidColor(glm::vec3(1.0f, 0.9f, 0.3f), blockout::kCapsuleRoughness);
  while (projectile_nodes_.size() < projectile_rows_.size()) {
    auto box = GenerateCube(glm::vec3(0.06f, 0.06f, 0.5f));  // unit-length base tracer
    const std::string name = "proj_" + std::to_string(projectile_nodes_.size());
    projectile_nodes_.push_back(
        AddMeshEntity(scene_, name.c_str(), std::move(box), mat, glm::mat4(1.0f)));
  }
  for (size_t i = 0; i < projectile_nodes_.size(); ++i) {
    if (i >= projectile_rows_.size()) {
      scene_.SetScale(projectile_nodes_[i], glm::vec3(0.0f));  // park the unused
      continue;
    }
    const badlands::ProjectileState& p = projectile_rows_[i];
    const glm::vec2 from{p.x, p.z};
    const glm::vec2 to{p.target_x, p.target_z};
    const glm::vec2 d = to - from;
    const float len = glm::length(d);
    if (len < 1e-3f) {
      scene_.SetScale(projectile_nodes_[i], glm::vec3(0.0f));
      continue;
    }
    const glm::vec2 dir = d / len;
    const float half_seg = 0.5f * std::min(len, 0.8f);  // a short tracer, not the whole path
    const glm::vec2 mid = from + dir * half_seg;
    const float yaw = std::atan2(dir.x, dir.y);  // XZ heading
    scene_.SetLocalTransform(
        projectile_nodes_[i],
        Trs{.position = glm::vec3(mid.x, 0.6f, mid.y),
            .rotation = glm::angleAxis(yaw, glm::vec3(0.0f, 1.0f, 0.0f)),
            .scale = glm::vec3(1.0f, 1.0f, half_seg / 0.5f)});
  }
}

float AiSandboxView::WorldHalfExtent() {
  // Derived from the world's own buildings rather than asked of the mode: the
  // host frames whatever is out there, and stays ignorant of what shape the
  // mode thinks it built.
  constexpr float kMinHalfExtent = 12.0f;  // a world with nothing in it still needs a floor
  float half = kMinHalfExtent;
  for (const badlands::BuildingState& b : sim_.Buildings()) {
    const float reach = 0.5f * static_cast<float>(std::max(b.width_tiles, b.depth_tiles));
    half = std::max(half, std::max(std::abs(b.center_x), std::abs(b.center_z)) + reach);
  }
  return half;
}

void AiSandboxView::FrameCamera() {
  gamecam_.focus = glm::vec3(0.0f);
  gamecam_.pitch_deg = 55.0f;

  // Whatever the mode built, walls included.
  const float half_x = WorldHalfExtent();
  const float half_z = half_x;

  // Empirically-derived coefficients (world units of visible ground extent
  // per world unit of camera height) for GameCameraController's fixed
  // pitch_deg=55 frustum at a 16:9-ish aspect: at height=15 the visible
  // ground spans x in [-19.0, 19.0] and z in [-13.0, +7.2] (the near/south
  // edge at +7.2 is the tighter constraint -- the tilted-down view
  // foreshortens it more than the far/north edge). Visible extent scales
  // linearly with height (same eye-ray angles), so height = extent /
  // coefficient inverts them; +25% margin covers narrower aspect ratios and
  // interactive window resizing.
  constexpr float kXCoeff = 19.0f / 15.0f;
  constexpr float kZNearCoeff = 7.2f / 15.0f;  // the tighter (south) edge
  const float height_for_x = half_x / kXCoeff;
  const float height_for_z = half_z / kZNearCoeff;
  gamecam_.height = 1.25f * std::max(height_for_x, height_for_z);

  gamecam_.UpdateCamera(camera_);
}

void AiSandboxView::HandleEvent(const SDL_Event& event, int /*width*/,
                                int /*height*/) {
  // Nav debug: a left click in pick mode drops a path endpoint on the ground.
  if (event.type == SDL_EVENT_MOUSE_BUTTON_DOWN &&
      event.button.button == SDL_BUTTON_LEFT) {
    HandleNavPick(event);
    return;
  }

  // Fixed-angle camera: only zoom is mouse-driven (wheel + trackpad, which SDL
  // reports as the same event with fractional deltas). Key panning is read
  // directly from Update()'s keyboard_state snapshot instead of per-event.
  if (event.type != SDL_EVENT_MOUSE_WHEEL) return;
  if (ImGui::GetIO().WantCaptureMouse) return;
  glm::vec2 screen;
  if (!EventWindowLogicalSize(event.wheel.windowID, screen)) return;
  ZoomAtCursor(gamecam_, camera_, NormalizedWheelY(event.wheel),
               glm::vec2(event.wheel.mouse_x, event.wheel.mouse_y), screen);
}

void AiSandboxView::Update(float dt, const bool* keyboard_state) {
  dt_ = dt;

  // Fixed-interval sim: advance the clock from real dt * speed, then run
  // game_tick(kTickDt) until we catch up to the tick target. The speed control
  // therefore accelerates the day/night loop WITHOUT changing the fixed rate
  // the sim itself sees -- determinism is preserved (the sim never observes a
  // variable dt), only how fast we feed it changes.
  sim_clock_.Advance(static_cast<double>(dt));
  const unsigned long long tick_target = sim_clock_.TickTarget();
  int budget = kMaxSimTicksPerFrame;
  while (sim_ticks_done_ < tick_target && budget-- > 0) {
    sim_.Tick(static_cast<float>(kTickDt));
    ++sim_ticks_done_;
  }
  // Empty the sim's transient event stream (this view does not render a combat
  // log; without draining, game.events would grow unbounded during combat).
  sim_.DrainEvents(events_scratch_);

  // The mode watches the world it asked for and says when it wants a new one.
  // Read AFTER the ticks so it sees this frame's final state, and the scene is
  // rebuilt on a restage because a fresh world has a different arena in it.
  char_rows_ = sim_.Characters();
  if (mode_->Observe(char_rows_, sim_.World().world_millis)) {
    StageWorld();
    BuildScene();
    FrameCamera();
    char_rows_ = sim_.Characters();
  }

  SyncUnits();
  SyncProjectiles();

  // ImGui context guard: Update() runs even in --screenshot mode, where no
  // ImGui context exists (SdlViewerApp only calls InitImGui() for the
  // windowed loop) -- ImGui::GetIO() asserts without a current context.
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
  scene_.SyncToRegistry(registry_, scene_context_);

  // Nav overlay last: it owns scene_context_.debug_lines (SyncToRegistry does
  // not touch that field), so setting it here survives to the render pass. The
  // arena floor is flat at y = 0.
  nav_debug_.Rebuild(sim_, scene_context_, [](float, float) { return 0.0f; });
}

void AiSandboxView::DrawUI() {
  if (!scene_renderer_) return;

  // NOTE(lighting): on any frame the editor changes env_, ApplyEnvironment
  // re-derives the full sky (6 faces x face x face radiance), a 2048-sample SH
  // projection, and a GPU cube rebuild + IBL re-prefilter next frame -- to be
  // debounced / made incremental in the future lighting commit.
  const bool env_changed = EditorUI::DrawDebugPanel(env_, *scene_renderer_, dt_);
  if (env_changed) {
    ApplyEnvironment();
  }

  DrawInspector();
  ImGui::Begin("Nav (debug)");
  nav_debug_.DrawControls();
  ImGui::End();
}

void AiSandboxView::HandleNavPick(const SDL_Event& event) {
  if (!nav_debug_.pick_mode()) return;
  if (ImGui::GetCurrentContext() != nullptr && ImGui::GetIO().WantCaptureMouse) return;
  glm::vec2 screen;
  if (!EventWindowLogicalSize(event.button.windowID, screen)) return;
  const Ray ray = ScreenPointToRay(
      camera_, glm::vec2(event.button.x, event.button.y), screen);
  glm::vec3 hit;
  if (!IntersectGroundPlane(ray, 0.0f, hit)) return;  // cursor on/above the horizon
  nav_debug_.Pick(glm::vec2(hit.x, hit.z));  // flat arena ground (y = 0)
}


void AiSandboxView::DrawInspector() {
  ImGui::Begin("Sim");
  if (false) {
    ImGui::TextUnformatted("no sim");
    ImGui::End();
    return;
  }

  badlands::WorldState world{};
  world = sim_.World();
  badlands::SimStats stats{};
  stats = sim_.GetStats();

  // --- clock -----------------------------------------------------------
  ImGui::Text("Day %u  %02d:%02d  %s", world.day,
              static_cast<int>(world.time_of_day * 24.0f),
              static_cast<int>(world.time_of_day * 24.0f * 60.0f) % 60,
              world.is_night ? "(night)" : "(day)");
  ImGui::Text("t = %lld ms   tick %llu", static_cast<long long>(world.world_millis),
              static_cast<unsigned long long>(stats.ticks));
  ImGui::SliderFloat("speed", &sim_clock_.speed, 0.0f, 60.0f, "%.0fx");
  ImGui::SameLine();
  if (ImGui::SmallButton("1x")) sim_clock_.speed = 1.0f;

  // --- the mode's own readout -------------------------------------------
  ImGui::SeparatorText(mode_->name());
  ImGui::TextUnformatted(mode_->Status().c_str());

  char_rows_ = sim_.Characters();

  // --- heroes ----------------------------------------------------------
  ImGui::SeparatorText("Entities");
  const uint32_t count =
      std::min(static_cast<uint32_t>(char_rows_.size()), kMaxCharacterRows);
  if (ImGui::BeginTable("entities", 6, ImGuiTableFlags_SizingStretchProp)) {
    ImGui::TableSetupColumn("id/name");
    ImGui::TableSetupColumn("type");
    ImGui::TableSetupColumn("behavior");
    ImGui::TableSetupColumn("goal");
    ImGui::TableSetupColumn("needs");
    ImGui::TableSetupColumn("inside");
    ImGui::TableHeadersRow();
    for (uint32_t i = 0; i < count; ++i) {
      const badlands::CharacterState& c = char_rows_[i];
      ImGui::TableNextRow();
      ImGui::TableNextColumn();
      if (c.name[0] != '\0') {
        ImGui::TextUnformatted(c.name);
      } else {
        ImGui::Text("#%u", c.id);
      }
      ImGui::TableNextColumn();
      ImGui::TextUnformatted(archetype_name(c.archetype));
      ImGui::TableNextColumn();
      ImGui::TextUnformatted(behavior_name(c.behavior));
      ImGui::TableNextColumn();
      if (c.goal_kind == 0) {
        ImGui::TextUnformatted("-");
      } else {
        ImGui::Text("(%.0f, %.0f) +%d", c.goal_x, c.goal_z, c.path_waypoints);
      }
      ImGui::TableNextColumn();
      if (c.archetype == 0) {  // hero: show need reserves (1 = satisfied)
        ImGui::Text("L%d %d/%d  f%.2f c%.2f", c.level, c.xp, c.xp_next,
                    c.fatigue, c.content);
        for (int32_t i = 0; i < c.skill_count; ++i) {
          ImGui::SameLine();
          ImGui::TextUnformatted(badlands::SkillName(c.skills[i]));
        }
      } else {
        ImGui::TextUnformatted("-");
      }
      ImGui::TableNextColumn();
      ImGui::Text("%d", c.inside_building_id);
    }
    ImGui::EndTable();
  }

  // --- goal statistics --------------------------------------------------
  // What the sim has SPENT ITS TIME ON, which is the thing a glance at the
  // entity table cannot tell you. Shares rather than raw counts, so the numbers
  // stay comparable as a run gets longer.
  ImGui::SeparatorText("Goal histogram");
  {
    const badlands::ActivityHistogram& stats = sim_.ActivityStats();
    const uint64_t samples = stats.Samples();
    ImGui::Text("%llu entity-ticks", static_cast<unsigned long long>(samples));
    ImGui::SameLine();
    if (ImGui::SmallButton("reset")) {
      sim_.ResetActivityStats();
    }

    if (samples > 0 &&
        ImGui::BeginTable("goals", 3 + badlands::HERO_CLASS_COUNT,
                          ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_RowBg)) {
      ImGui::TableSetupColumn("activity");
      ImGui::TableSetupColumn("band");
      ImGui::TableSetupColumn("all");
      for (int32_t c = 0; c < badlands::HERO_CLASS_COUNT; ++c) {
        ImGui::TableSetupColumn(
            badlands::HeroClassName(static_cast<badlands::HeroClassId>(c)));
      }
      ImGui::TableHeadersRow();

      for (const badlands::ActivityInfo& info : badlands::ActivityCatalog()) {
        const uint64_t total = stats.Total(info.id);
        if (total == 0) {
          continue;  // never once chosen -- omit rather than pad the table
        }
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        ImGui::TextUnformatted(info.name);
        ImGui::TableNextColumn();
        ImGui::TextUnformatted(band_name(info.band));
        ImGui::TableNextColumn();
        ImGui::Text("%5.1f%%", 100.0 * static_cast<double>(total) /
                                   static_cast<double>(samples));
        for (int32_t c = 0; c < badlands::HERO_CLASS_COUNT; ++c) {
          ImGui::TableNextColumn();
          const uint64_t n =
              stats.ForClass(static_cast<badlands::HeroClassId>(c), info.id);
          if (n == 0) {
            ImGui::TextUnformatted("-");
          } else {
            ImGui::Text("%llu", static_cast<unsigned long long>(n));
          }
        }
      }
      ImGui::EndTable();
    }
  }

  // --- the trace of record ---------------------------------------------
  ImGui::SeparatorText("Command log");
  ImGui::Text("%u applied (showing last %u)", command_log_total_,
              std::min(command_log_total_, kMaxCommandRows));
  const uint32_t shown = std::min(command_log_total_, kMaxCommandRows);
  if (ImGui::BeginChild("cmdlog", ImVec2(0.0f, 160.0f))) {
    for (uint32_t i = 0; i < shown; ++i) {
      const badlands::CommandRecord& r = cmd_rows_[i];
      ImGui::Text("%-14s actor=%s (%.1f, %.1f) a=%d", command_name(r.kind),
                  r.actor == UINT32_MAX ? "player" : std::to_string(r.actor).c_str(),
                  r.point_x, r.point_z, r.param_a);
    }
  }
  ImGui::EndChild();

  ImGui::End();
}

void AiSandboxView::OnResize(int width, int height) {
  // Only refresh the aspect. FrameCamera() (run once in Initialize) must NOT
  // be called here: it resets gamecam_.focus to the origin, which would
  // discard any WASD pan on every window resize. The framing is
  // aspect-independent (see FrameCamera's coefficient comment), so nothing
  // needs re-framing on resize.
  camera_.aspect =
      height > 0 ? static_cast<float>(width) / static_cast<float>(height) : 1.0f;
}

}  // namespace badlands
