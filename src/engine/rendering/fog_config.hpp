#pragma once

// Runtime-configurable volumetric-fog parameters (Task: fog rendering). Plain
// data owned by C++ and read at the start of each SceneRenderer::Render, in the
// same spirit as ShadowConfig — game-agnostic, lives in src/engine/. Consumed
// by VolumetricFog; surfaced live via EditorUI::DrawFogEditor.
#include <glm/glm.hpp>

#include "engine/rendering/fog_cascade.hpp"

namespace badlands {

struct FogConfig {
  bool enabled = true;

  // --- Cascade layout (height-band clipmap) ---
  // Single source of truth for the clipmap geometry. The vertical band
  // [floor_y, floor_y + height] defaults to 0..64 m and is runtime-configurable
  // (alongside cascade count, resolution, and XZ extent).
  static constexpr float kFogFloorY = 0.0f;
  static constexpr float kFogBandHeight = 64.0f;
  fog::CascadeLayout layout{
      .cascade_count = 3,
      .res_xz = 128,               // voxels per cascade in X and Z
      .res_y = 32,                 // vertical slices in the fixed band
      .base_half_extent = 64.0f,   // cascade 0 XZ half-extent (metres)
      .floor_y = kFogFloorY,       // bottom of the fog band (water line)
      .height = kFogBandHeight,    // band height (metres) — fixed
  };

  // --- Raymarch / lighting (visual) ---
  float fog_max_distance = 400.0f;  // clamp march length (metres)
  int step_count = 48;
  float phase_g = 0.3f;             // Henyey-Greenstein anisotropy
  float ambient_scale = 1.0f;
  float sun_scale = 1.0f;

  // --- Stochastic / perf toggles ---
  bool enable_shafts = true;
  bool jitter = true;
  bool half_res = true;

  const fog::CascadeLayout& Layout() const { return layout; }
};

}  // namespace badlands
