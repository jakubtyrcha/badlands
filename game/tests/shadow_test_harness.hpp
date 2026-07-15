#pragma once

// Task T4: headless GPU render harness for the directional-shadow Catch2
// suite. Test-only (see shadow_test_geometry.hpp's file comment for why this
// isn't part of badlands_engine).

#include <cstdint>
#include <vector>

#include "engine/core/camera.hpp"
#include "engine/rendering/scene_renderer.hpp"
#include "shadow_test_geometry.hpp"

namespace badlands::shadowtest {

// A width*height row-major image of LINEAR visibility values in [0,1]
// (top-left origin, y=0 = top row -- matches WGSL's @builtin(position).xy /
// screenUV convention, see CameraRayDirectionWorld's doc comment). Read back
// from an RGBA16Float target's R channel (the shadow debug modes write
// vec4(v,v,v,1)).
struct Image {
  uint32_t width = 0;
  uint32_t height = 0;
  std::vector<float> pixels;

  float At(uint32_t x, uint32_t y) const {
    return pixels[static_cast<size_t>(y) * width + x];
  }
};

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
// `config`, and reads back the LINEAR shadow-debug-mode value at every
// pixel. Builds a lazily-initialized, process-lifetime headless GPU context
// (hidden SDL window + Dawn device) on first call -- see
// shadow_test_harness.cpp's GetTestGpu().
Image RenderShadowFrame(const ShadowTestConfig& config, const Scene& world_scene,
                        const Camera& camera);

}  // namespace badlands::shadowtest
