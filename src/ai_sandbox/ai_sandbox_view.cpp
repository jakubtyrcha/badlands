#include "ai_sandbox/ai_sandbox_view.hpp"

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>

#include <SDL3/SDL.h>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
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
#include "game/building_catalog.h"
#include "game/factors_manifest.hpp"
#include "game/scene/blockout_materials.hpp"
#include "mapgen/biomes.hpp"

namespace badlands {

namespace {

// Debug materials come from the shared blockout palette (game/scene/
// blockout_materials.hpp) so the game's blockout mode and this arena draw from
// one source of truth: blockout::kArenaGray/kArenaRoughness for the floor +
// walls, blockout::kWall/kRoof for the buildings, blockout::kCapsule* for the
// hero capsules.

// Wall block footprint/height (world units; tile = 1.0 world unit).
constexpr float kWallHalfFootprint = 0.5f;
constexpr float kWallHalfHeight = 0.6f;  // 1.2 tall

// Repeat the floor UVs roughly once per 2 world units instead of stretching
// one copy across the whole floor.
constexpr float kFloorUvRepeatSpacing = 2.0f;

// Capsule dimensions (world units).
constexpr float kCapsuleRadius = 0.35f;
constexpr float kCapsuleCylinderHeight = 0.6f;

// The sandbox arena: origin-centred greybox. Elongated in z so it also contains
// the prebuilt colony Castle, which make_world now places on the plains at
// kCastleSpawn (world z ~ +54) rather than the origin -- the floor/wall ring
// reach it so nothing floats off the pit. The town + deer still cluster near the
// origin; the tax collector walks north to the Castle to deposit. Well inside
// the sim's 256 u placement grid.
constexpr glm::ivec2 kSandboxArena{90, 130};

// Snapshot buffer caps. The sandbox town is tiny; a truncated snapshot would
// only mean fewer rows drawn/listed, never a crash.
constexpr uint32_t kMaxCharacterRows = 64;
constexpr uint32_t kMaxBuildingRows = 64;
constexpr uint32_t kMaxCommandRows = 24;

// Where the seeded town goes. Kept clear of the origin Castle that game_create
// prebuilds (4x4 + margin) and spaced so no footprint+margin overlaps another.
struct TownBuilding {
  badlands::BuildingKind kind;
  float x, z;
  int32_t rotation_index;
};
constexpr TownBuilding kTown[] = {
    {badlands::BuildingKind::FreeCompanyQuarters, -14.0f, -8.0f, 0},
    {badlands::BuildingKind::FreeCompanyQuarters, -14.0f, 8.0f, 0},
    {badlands::BuildingKind::HuntersCamp, -24.0f, 0.0f, 0},  // recruits a hunter
    {badlands::BuildingKind::Tavern, 14.0f, -8.0f, 0},
    {badlands::BuildingKind::Apothecary, 14.0f, 8.0f, 0},
    {badlands::BuildingKind::House, 6.0f, 16.0f, 0},   // accrue tax for the collector
    {badlands::BuildingKind::House, -6.0f, 16.0f, 0},
    {badlands::BuildingKind::Watchtower, 0.0f, -16.0f, 0},  // second deposit point
    {badlands::BuildingKind::Sewer, 18.0f, -14.0f, 0},     // rats crawl out here
};

// Heroes recruited per guild at seed time. Below kGuildRosterCap so the panel
// can show a recruit that fails on a full roster if the cap ever shrinks.
constexpr int kSeedHeroesPerGuild = 3;
constexpr int kSeedDeer = 6;

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
    case badlands::CommandKindId::Shoot: return "Shoot";
    case badlands::CommandKindId::Chat: return "Chat";
    default: return "?";
  }
}

// noiser is PARKED by default: heroes think via the wasm brain
// (assets/brains/hero.wasm) when it loads, the C++ town brain otherwise.
// Explicitly setting BADLANDS_BRAIN_SCRIPT (see load_brain_script below)
// opts back into the noiser path for a session -- dev tooling, not a shipped
// path.
//
// Upstream noiser has seven open bugs (docs/noiser-bugs-upstream/), two of
// which block composing brains at all -- sub-generators cannot be resumed
// from the entry generator, and a file with 2+ `gen fn` runs an arbitrary
// one. The scripts and the whole host-call surface stay in the tree and
// compiling (game/src/brain.cpp, game/tests/noiser_smoke_tests.cpp keep
// exercising them, dormant), so re-adopting it is a switch flip once those
// land -- see docs/superpowers/specs/2026-07-23-wasm-brain-contract-design.md's
// Scope note (the noiser path stays "unused by the apps").
//
// Missing/unreadable file here is packaging, not a brain bug (the file
// simply was not built/shipped) -- log and fall back to mock (via an empty
// BrainDesc), the same as no wasm bytes provided at all. A wasm module that
// DOES load but fails to compile/instantiate is a different case entirely --
// fatal, per WasmBrainRuntime::create's policy (wasm_brain.h) -- because
// bytes were actually handed to bh_load/bh_instantiate.
std::vector<uint8_t> load_hero_wasm() {
  constexpr const char* kPath = "assets/brains/hero.wasm";  // cwd-relative, like shaders/assets
  std::ifstream file(kPath, std::ios::binary | std::ios::ate);
  if (!file.good()) {
    spdlog::warn("AiSandboxView: '{}' unreadable -- heroes fall back to the C++ brain", kPath);
    return {};
  }
  const std::streamsize size = file.tellg();
  file.seekg(0, std::ios::beg);
  std::vector<uint8_t> bytes(static_cast<size_t>(size));
  if (!file.read(reinterpret_cast<char*>(bytes.data()), size)) {
    spdlog::warn("AiSandboxView: '{}' failed to read -- heroes fall back to the C++ brain", kPath);
    return {};
  }
  return bytes;
}

// Explicitly setting BADLANDS_BRAIN_SCRIPT opts back in to the noiser brain
// for a session; unset (the default) drives every hero via the wasm brain
// (or the C++ town brain, if that fails to load -- see load_hero_wasm). A
// read failure here is a config error at the app level, not fatal (this is
// opt-in dev tooling mirroring the old pre-wasm behaviour, not a shipped
// path) -- log and fall back to the wasm path.
std::string load_brain_script() {
  const char* env = std::getenv("BADLANDS_BRAIN_SCRIPT");
  if (env == nullptr) {
    return {};  // parked
  }
  std::ifstream file(env);
  if (!file.good()) {
    spdlog::warn("AiSandboxView: BADLANDS_BRAIN_SCRIPT='{}' unreadable -- using the wasm hero brain",
                 env);
    return {};
  }
  std::ostringstream buffer;
  buffer << file.rdbuf();
  return buffer.str();
}

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

  arena_ = build_arena(kSandboxArena);
  SeedTown();
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

void AiSandboxView::SeedTown() {
  badlands::BrainDesc brain_desc{};

  // brain_script/wasm's backing storage only needs to outlive the
  // Sim(brain_desc) construction below: BrainRuntime::create copies the
  // noiser source into the compiled program, and bh_load compiles the wasm
  // module synchronously and keeps no reference to the input bytes (see
  // brainhost's bh_load -- wasmtime::Module::new copies/compiles eagerly) --
  // so both are declared at this scope, alongside the single Sim() call that
  // consumes whichever one applies.
  const std::string brain_script = load_brain_script();
  std::vector<uint8_t> wasm;
  if (!brain_script.empty()) {
    brain_desc.noiser_source = brain_script.c_str();
    spdlog::info("AiSandboxView: BADLANDS_BRAIN_SCRIPT loaded ({} bytes) -- heroes think via the "
                 "noiser brain",
                 brain_script.size());
  } else {
    wasm = load_hero_wasm();
    if (!wasm.empty()) {
      brain_desc.wasm_bytes = wasm.data();
      brain_desc.wasm_len = wasm.size();
      spdlog::info("AiSandboxView: assets/brains/hero.wasm loaded ({} bytes) -- heroes think via "
                   "the wasm brain",
                   wasm.size());
    }
  }
  sim_ = badlands::Sim(brain_desc);

  // Behaviour tuning as data: load over the compiled defaults (a missing file
  // just keeps them). Must happen before ticking -- factors are initial config.
  badlands::SimFactors factors = sim_.Factors();
  if (badlands::LoadSimFactors("assets/creatures/factors.json", factors)) {
    sim_.SetFactors(factors);
  }

  // Everything goes through game_dispatch, so the seed is itself a logged
  // command sequence -- (initial config, seed, command log) still reproduces
  // this world.
  for (const TownBuilding& b : kTown) {
    badlands::Action place{badlands::ActionKind::PlaceBuilding, 0, b.x, b.z,
                     static_cast<int32_t>(b.kind), b.rotation_index};
    const int64_t id = sim_.Dispatch(place);
    if (id < 0) {
      spdlog::warn("AiSandboxView::SeedTown: placing kind {} at ({}, {}) failed",
                   static_cast<int32_t>(b.kind), b.x, b.z);
      continue;
    }
    if (b.kind != badlands::BuildingKind::FreeCompanyQuarters &&
        b.kind != badlands::BuildingKind::HuntersCamp) {
      continue;
    }
    for (int i = 0; i < kSeedHeroesPerGuild; ++i) {
      badlands::Action recruit{badlands::ActionKind::RecruitHero, static_cast<uint32_t>(id),
                         0.0f, 0.0f, 0, 0};
      if (sim_.Dispatch(recruit) < 0) {
        spdlog::warn("AiSandboxView::SeedTown: recruit {} from guild {} failed", i,
                     id);
      }
    }
  }

  // A small herd of deer on good terrain NEAR the town, so they are visible
  // wandering/grazing/fleeing inside the arena. (Forest proper is a ring far
  // outside this greybox pit; deer roaming real woods is covered by the tests.
  // The sim reasons about the biome map even though the arena does not draw it.)
  int deer_placed = 0;
  for (int i = 0; i < 600 && deer_placed < kSeedDeer; ++i) {
    const float ang = static_cast<float>(i) * 2.399963f;  // golden-angle spread
    const float rad = 22.0f + static_cast<float>(i % 20) * 1.0f;  // the good-biome ring
    const glm::vec2 p{std::cos(ang) * rad, std::sin(ang) * rad};
    const int32_t b = sim_.BiomeAt(p.x, p.y);
    if (b != static_cast<int32_t>(mapgen::Biome::Forest) &&
        b != static_cast<int32_t>(mapgen::Biome::Plains)) {
      continue;  // keep them off the central Lake
    }
    CharacterDesc d{};
    d.archetype = badlands::Archetype::Critter;
    d.pos_x = p.x;
    d.pos_z = p.y;
    d.team = 2;  // neutral wildlife
    d.hp = 8.0f;
    d.move_speed = 3.0f;
    d.attack_range = 0.0f;
    d.attack_damage = 0.0f;
    d.attack_cooldown = 1.0f;
    d.size_x = d.size_z = 0.7f;
    d.size_y = 1.0f;
    d.color_r = 0.62f;  // deer brown, distinct from the heroes' blue capsules
    d.color_g = 0.42f;
    d.color_b = 0.20f;
    sim_.Spawn(d);
    ++deer_placed;
  }

  building_rows_.resize(kMaxBuildingRows);
  cmd_rows_.resize(kMaxCommandRows);
}

void AiSandboxView::BuildScene() {
  // Fresh graph: re-mirror scene_context_'s (already-derived-from-env_)
  // lighting right after, same as ApplyEnvironment does for the live-edit
  // path (SceneGraph's constructor resets sun/ambient to its own defaults).
  scene_ = SceneGraph();
  scene_.SetSunDirection(scene_context_.sun_direction);
  scene_.SetSunColor(scene_context_.sun_color);
  scene_.SetAmbientSH(scene_context_.ambient_sh);

  // Floor covers the whole arena footprint (interior + wall ring) with
  // headroom; scales with the configured arena size instead of a fixed
  // constant.
  const float full_x = static_cast<float>(arena_.accessible.x + 2);
  const float full_z = static_cast<float>(arena_.accessible.y + 2);
  const float floor_size = std::max(full_x, full_z) + 4.0f;
  AddFloor(scene_, floor_size, matlib_.SolidColor(blockout::kArenaGray, blockout::kArenaRoughness),
           floor_size / kFloorUvRepeatSpacing);

  AddWalls();
  AddBuildings();
}

void AiSandboxView::AddWalls() {
  const DeferredMaterial wall_mat =
      matlib_.SolidColor(blockout::kArenaGray, blockout::kArenaRoughness);

  int index = 0;
  for (const glm::ivec2& tile : arena_.wall_tiles) {
    const glm::vec2 center = arena_tile_center(tile);
    // GenerateCube is centered at the origin -- translate up by half the
    // height so the block's base sits on the y=0 floor.
    const glm::mat4 transform = glm::translate(
        glm::mat4(1.0f), glm::vec3(center.x, kWallHalfHeight, center.y));

    auto cube = GenerateCube(
        glm::vec3(kWallHalfFootprint, kWallHalfHeight, kWallHalfFootprint));

    const std::string name = "wall_" + std::to_string(index++);
    AddMeshEntity(scene_, name.c_str(), std::move(cube), wall_mat, transform);
  }
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

void AiSandboxView::FrameCamera() {
  gamecam_.focus = glm::vec3(0.0f);
  gamecam_.pitch_deg = 55.0f;

  // Full arena footprint including the 1-tile wall ring on every side.
  const float half_x = 0.5f * static_cast<float>(arena_.accessible.x + 2);
  const float half_z = 0.5f * static_cast<float>(arena_.accessible.y + 2);

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

  SyncUnits();

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
  stats = stats;

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
  ImGui::Text("noiser bugs: %u   script intents: %llu", stats.noiser_bugs,
              static_cast<unsigned long long>(stats.script_intents));

  // --- heroes ----------------------------------------------------------
  char_rows_ = sim_.Characters();
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
        ImGui::Text("f%.2f c%.2f", c.fatigue, c.content);
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
