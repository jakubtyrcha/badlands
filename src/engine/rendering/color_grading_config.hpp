#pragma once

// Runtime-configurable color-grading parameters (Task: P3/HDR color grading).
// Plain data owned by C++ and read at the start of each SceneRenderer::Render,
// in the same spirit as FogConfig/ShadowConfig — game-agnostic, lives in
// src/engine/. Consumed by ColorGrading; surfaced live via
// EditorUI::DrawColorGradingEditor.
//
// The grade operates in Oklab on the linear-sRGB HDR working buffer, after
// projected decals and before debug lines: crush blacks below a lightness
// threshold, desaturate midtones while sparing already-saturated colors
// (chroma mask) and HDR highlights (the luminance masks exclude L > 1).
// Defaults are the design-spec starting look; disabled by default so dev
// tools stay neutral — the game view opts in.

namespace badlands {

struct ColorGradingConfig {
  bool enabled = false;

  // Colors below this Oklab lightness are crushed toward black. The shader
  // guards the division with max(threshold, 0.01); the editor slider floors
  // at 0.01 for the same reason.
  float black_crush_threshold = 0.15f;
  // Crush exponent (1 = no crush; useful range ~1.5–3).
  float black_crush_strength = 2.0f;

  // Oklab lightness band treated as midtones (smoothstepped ±0.1 at each
  // edge). Anything above midtone_luminance_end + 0.1 — including HDR
  // highlights with L > 1 — is untouched.
  float midtone_luminance_start = 0.2f;
  float midtone_luminance_end = 0.8f;

  // How much chroma to remove from masked midtones (0 = off, 1 = full —
  // near-monochrome midtones; 0.5 keeps the scene readable while clearly
  // muted).
  float midtone_desat_strength = 0.5f;
  // Chroma level at which desaturation fades out entirely, protecting
  // deliberately saturated accents (blood, magic, team colors).
  float saturation_preservation_mask = 0.15f;
};

}  // namespace badlands
