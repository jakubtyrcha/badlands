#pragma once

// LightEnvironment (Task S2.B3) — the shared, game-agnostic description of a
// scene's directional sun + procedural sky, plus a helper that (re)derives all
// the render-facing lighting state from it.
//
// It replaces B2's hardcoded inline sky/SH in PlaceholderView: instead of
// hand-coding a sky radiance function + SH projection at each view, callers own
// a `LightEnvironment` (editable — the B4 ImGui editor mutates it) and call
// `ApplyLightEnvironment` whenever it changes. This lives in src/engine/ and
// intentionally knows nothing about any game.
#include <cstdint>

#include <dawn/webgpu_cpp.h>
#include <glm/glm.hpp>

namespace badlands {

class CubemapBuilder;
struct SceneContext;

// Directional sun + analytic sky dome. Defaults reproduce a clear-day sky with
// the sun high and slightly to the side.
struct LightEnvironment {
  glm::vec3 sun_direction{glm::normalize(glm::vec3(0.3f, 1.0f, 0.4f))};  // toward sun
  glm::vec3 sun_color{1.0f};
  float sun_intensity{3.0f};
  glm::vec3 sky_color{0.45f, 0.62f, 0.95f};      // zenith / overhead
  glm::vec3 horizon_color{0.80f, 0.85f, 0.92f};  // at the horizon
  glm::vec3 ground_color{0.28f, 0.26f, 0.24f};   // below the horizon
  float sky_intensity{1.0f};
  float sun_disc_size{0.02f};  // angular radius of the sun disc, radians
};

// (Re)builds the scene's lighting/environment from `env`:
//   * fills `sky_cube` (a standard RGBA16Float cube, `face_size` texels/face)
//     with a sky->horizon->ground gradient + a bright HDR sun disc along
//     `env.sun_direction`;
//   * projects the SUN-FREE gradient to an SH L2 ambient (avoids Monte-Carlo
//     blow-up from the tiny sun solid angle — the sun's contribution is the
//     directional term + the IBL specular reflection, not the diffuse ambient);
//   * writes into `out`: sun_direction, sun_color * sun_intensity, ambient_sh,
//     skybox_cubemap (= sky_cube's view), and bumps skybox_generation so the
//     SceneRenderer re-prefilters the IBL cube.
//
// `device`/`queue` upload the cube (CubemapBuilder needs them — this parameter
// pair is the one addition to the struct-only signature in the task brief).
void ApplyLightEnvironment(const LightEnvironment& env, wgpu::Device device,
                           wgpu::Queue queue, CubemapBuilder& sky_cube,
                           SceneContext& out, uint32_t face_size = 128);

}  // namespace badlands
