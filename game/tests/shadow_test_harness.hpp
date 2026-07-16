#pragma once

// Task T4: headless GPU render harness for the directional-shadow Catch2
// suite. Test-only (see shadow_test_geometry.hpp's file comment for why this
// isn't part of badlands_engine).
//
// Task T8: re-plumbed onto the sampo-ported GPU-test infra
// (src/engine/tests/gpu_test_helpers.hpp + badlands::ColorRenderTarget +
// badlands::TextureReadback) instead of a home-grown hidden-SDL-window
// device + hand-rolled RGBA16Float readback. See task-8-brief.md /
// task-8-report.md.

#include <cstdint>

#include "core/util/cpu_image.hpp"
#include "engine/core/camera.hpp"
#include "engine/rendering/scene_renderer.hpp"
#include "shadow_test_geometry.hpp"

namespace badlands::shadowtest {

struct ShadowTestConfig {
  uint32_t r_sm = 2048;
  float d_max = 128.0f;
  ShadowDebugMode mode = ShadowDebugMode::ShadowMapOnly;
  bool enable_shadow_map = true;
  bool enable_contact_shadows = true;
  // Task T3-fix-2: forwarded to ShadowConfig::hard_shadow_debug. Test 1
  // (edge-leak) sets this true to get the raw, unfiltered shadow signal;
  // Test 5 (RPDB slope-acne) leaves it false (needs the PCF path to
  // exercise the per-tap biasUV gradient).
  bool hard_shadow_debug = false;
};

// Fixed offscreen capture size (512x512) for every RenderShadowFrame call.
inline constexpr uint32_t kFrameWidth = 512;
inline constexpr uint32_t kFrameHeight = 512;

// Renders `world_scene` (already posed into world space -- see ApplyPose)
// from `camera` through a fresh, throwaway SceneRenderer configured per
// `config`, into an R32Float offscreen ColorRenderTarget, and reads it back
// as a CpuImage of LINEAR shadow-debug-mode values (top-left origin, y=0 =
// top row -- matches WGSL's @builtin(position).xy / screenUV convention, see
// CameraRayDirectionWorld's doc comment). Sample a pixel via
// `img.GetFloat(x, y)`.
//
// Builds a lazily-initialized, process-lifetime headless GPU context (no
// SDL window -- see badlands::test::RequestAdapter/RequestDevice in
// gpu_test_helpers.hpp) on first call -- see shadow_test_harness.cpp's
// GetTestGpu().
CpuImage RenderShadowFrame(const ShadowTestConfig& config, const Scene& world_scene,
                           const Camera& camera);

}  // namespace badlands::shadowtest
