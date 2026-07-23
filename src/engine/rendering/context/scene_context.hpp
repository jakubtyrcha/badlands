#pragma once

// Trimmed port of sampo's src/rendering/context/scene_context.hpp, namespace
// sampo -> badlands.
//
// sampo's `SceneContext` is a large grab-bag of scene-level rendering state:
// lighting, skybox, RenderingParams (shadow/GTAO/tonemap toggles), debug
// line/polygon buffers, text overlays, screen rects, a ray-trace overlay, and
// a glyph-morph text demo request — most of it belonging to rendering
// subsystems (shadow mapping, skybox/IBL, debug draw, text rendering) that
// haven't been ported to badlands yet.
//
// Per the D2 task brief, this port keeps only the fields
// `SceneGraph::SyncToRegistry` actually writes: the ECS registry pointer,
// directional-light (sun) direction/color, ambient SH coefficients, and
// clear color. Everything else (skybox_cubemap, RenderingParams,
// SphereRenderMode, GBufferDebugMode, debug buffers, text/HUD overlays,
// screen rects, ray-trace/glyph-morph overlays) is dropped — bring it back
// alongside whichever future task ports the subsystem that actually consumes
// it.
#include <array>
#include <cstdint>
#include <dawn/webgpu_cpp.h>
#include <entt/entt.hpp>
#include <glm/glm.hpp>

namespace badlands {

class DebugLineBuffer;
class ScenePostPass;
struct ProjectedDecal;

// SceneContext holds scene-level state written by SceneGraph::SyncToRegistry
// and read by (future) rendering passes. See the trim note above — this is
// deliberately a small subset of sampo's SceneContext.
struct SceneContext {
  // Entity data - pointer to allow external or internal ownership
  entt::registry* registry{nullptr};

  // Scene lighting configuration (independent of rendering features)
  glm::vec3 sun_direction{0.0f, 1.0f, 0.0f};  // Directional light direction
  glm::vec3 sun_color{1.0f, 1.0f, 1.0f};      // Directional light color
  // Spherical Harmonics L2 (9 coefficients) for directional ambient lighting
  std::array<glm::vec3, 9> ambient_sh{};

  // Source environment cubemap for IBL specular (a standard RGBA16Float cube,
  // e.g. built by CubemapBuilder from a procedural sky). Null = no environment,
  // in which case SceneRenderer binds a 1x1 black fallback cube (IBL specular
  // contributes nothing). SceneRenderer regenerates its prefiltered cube
  // whenever `skybox_generation` changes — bump it after replacing the view.
  wgpu::TextureView skybox_cubemap;
  uint32_t skybox_generation{0};

  // Clear/background color
  glm::vec4 clear_color{0.1f, 0.1f, 0.1f, 1.0f};

  // Presentation-clock time in seconds, fed by the app each frame. Drives
  // time-animated forward materials (e.g. water waves) via the forward pass.
  // Independent of the sim clock; deterministic under headless SeekToTimeOfDay.
  float time_seconds{0.0f};

  // Wall-clock time in seconds, fed by the app each frame. Unlike
  // `time_seconds` this is NOT scaled by game speed and does NOT stop when the
  // game is paused -- it is the clock for UI-domain animation (e.g. the
  // marching-ants scroll on selection decals), which should keep moving while
  // the world is frozen. Still deterministic in headless capture, where the app
  // feeds a fixed presentation step rather than real elapsed time.
  float real_time_seconds{0.0f};

  // Optional world-space debug lines, drawn (screen-aligned + antialiased,
  // depth-tested against the G-buffer) after deferred lighting and before
  // tonemap. Null = none. Not owned; must outlive the frame.
  const DebugLineBuffer* debug_lines = nullptr;

  // Optional generic post-scene modulation hook (game-agnostic). When set, the
  // renderer snapshots the lit HDR colour after the transparent pass and invokes
  // pass->Execute() with the snapshot + HDR target + G-buffer depth, before
  // debug lines and tonemap, so the modulation affects the whole scene. Not
  // owned; must outlive the frame. Carried here (rather than as a renderer
  // member) so it also applies to the throwaway renderer built for headless
  // --screenshot. See engine/rendering/scene_post_pass.hpp (used by the game's
  // fog-of-war overlay).
  ScenePostPass* post_pass = nullptr;

  // Optional world-space projected decals (selection highlights, VFX marks),
  // composited into the HDR colour after the post-scene hook and before debug
  // lines -- so they sit on top of any scene modulation (e.g. fog-of-war) but
  // under dev overlays. Not owned; must outlive the frame. Null/0 = none.
  // Carried here rather than as a renderer member for the same reason as
  // debug_lines/post_pass: the headless --screenshot renderer then draws them
  // too. See engine/rendering/projected_decal.hpp.
  const ProjectedDecal* decals = nullptr;
  uint32_t decal_count = 0;
};

}  // namespace badlands
