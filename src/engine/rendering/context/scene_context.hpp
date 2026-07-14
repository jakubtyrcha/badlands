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
};

}  // namespace badlands
