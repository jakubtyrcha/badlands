#pragma once

// Core of the noise-texgen tool: compile a noiser script, evaluate it over a
// windowed 2D domain into a texture, and encode pixels. Kept separate from the
// CLI (noise_texgen.cpp) so it can be driven directly from tests.
//
// The script is dispatched as a warp grid (@warpId = (px, py, 0),
// @warpSize = (width, height, 1)); the domain window is bound as the four
// scalar uniforms win_min_x/win_min_y/win_max_x/win_max_y that assets/noiser/
// texgen/texgen.noiser declares. The script's top-level return value is the
// linear pixel: f32 for grayscale, vec3 for rgb, vec4 for rgba.

#include <cstdint>
#include <expected>
#include <string>
#include <vector>

namespace badlands::texgen {

enum class Mode { kGrayscale, kRgb, kRgba };

struct Options {
  std::string script_path;
  uint32_t width = 0;
  uint32_t height = 0;
  // Domain window [min, max] mapped across the texture (uniforms).
  float win_min_x = 0.0f;
  float win_min_y = 0.0f;
  float win_max_x = 1.0f;
  float win_max_y = 1.0f;
  Mode mode = Mode::kGrayscale;
  // True when the window was set explicitly (e.g. via --window). When set,
  // Generate() errors if the script declares none of the window uniforms,
  // rather than silently ignoring the requested window.
  bool window_explicit = false;
  // Extra module search paths for `import`. The directory containing the
  // script is always searched (so `import ... from texgen` resolves when
  // texgen.noiser sits next to the script).
  std::vector<std::string> include_paths;
};

// Evaluates the script and returns tightly-packed RGBA8 pixels
// (width*height*4, no row padding), or an error message. Grayscale is written
// linear (value -> gray, no gamma); rgb/rgba clamp to [0,1], sRGB-encode the
// color channels, and keep alpha linear. Work is split into 8x8 patches across
// the shared thread pool (badlands::ParallelFor).
std::expected<std::vector<uint8_t>, std::string> Generate(const Options& opts);

// Generate() then write the pixels to a PNG via the assets crate.
std::expected<void, std::string> GenerateToPng(const Options& opts,
                                               const std::string& out_path);

}  // namespace badlands::texgen
