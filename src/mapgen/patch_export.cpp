#include "mapgen/patch_export.hpp"

#include <algorithm>
#include <cmath>

#include <glm/glm.hpp>

#include "mapgen/biomes.hpp"
#include "mapgen/river_network.hpp"

namespace badlands::mapgen {

namespace {

// PRESENTATION constants. Unlike everything else in this directory these are
// picked to be readable, not physical -- they only ever reach an image. The
// depth scales are in metres so they stay meaningful across resolutions.

constexpr Rgb kLandTone{190, 195, 185};   // dry ground, before shading
constexpr Rgb kWaterShallow{72, 132, 172};
constexpr Rgb kWaterDeep{16, 40, 70};
constexpr Rgb kRiverTone{48, 108, 168};   // channel centrelines

// The sun: north-west at 45 degrees elevation, the same convention
// tools/protogen/show.py renders stage-1 dumps with, so a stage-2 hillshade
// reads against them. Unit length by construction: cos(45)/sqrt(2) twice
// horizontally, sin(45) vertically.
constexpr float kSunX = -0.5f, kSunY = -0.5f, kSunZ = 0.70710678f;
// Floor so a slope facing away stays legible instead of going to black.
constexpr float kAmbient = 0.25f;

uint8_t to_code(float unit) {
  return static_cast<uint8_t>(
      std::lround(std::clamp(unit, 0.0f, 1.0f) * 255.0f));
}

void put(std::vector<uint8_t>& rgba, size_t i, Rgb c) {
  rgba[i] = c.r;
  rgba[i + 1] = c.g;
  rgba[i + 2] = c.b;
  rgba[i + 3] = 255;
}

Rgb lerp_rgb(Rgb a, Rgb b, float t) {
  t = std::clamp(t, 0.0f, 1.0f);
  const auto mix = [t](uint8_t x, uint8_t y) {
    return static_cast<uint8_t>(std::lround(x + (y - x) * t));
  };
  return {mix(a.r, b.r), mix(a.g, b.g), mix(a.b, b.b)};
}

// Central difference with clamped borders, in metres per metre.
float slope_x(const Field2D<float>& f, int x, int y, float texel_m) {
  const int lo = std::max(0, x - 1), hi = std::min(f.width - 1, x + 1);
  if (hi == lo) return 0.0f;
  return (f.at(hi, y) - f.at(lo, y)) / ((hi - lo) * texel_m);
}

float slope_y(const Field2D<float>& f, int x, int y, float texel_m) {
  const int lo = std::max(0, y - 1), hi = std::min(f.height - 1, y + 1);
  if (hi == lo) return 0.0f;
  return (f.at(x, hi) - f.at(x, lo)) / ((hi - lo) * texel_m);
}

// One segment of a river polyline, in TEXEL coordinates. Stepped at one texel
// per sample so a diagonal reach stays connected; samples off the patch are
// dropped, not clamped, or a reach passing by outside would smear along an edge.
void draw_segment(std::vector<uint8_t>& rgba, int w, int h, glm::vec2 a,
                  glm::vec2 b, Rgb tone) {
  const glm::vec2 d = b - a;
  const int steps = std::max(
      1, static_cast<int>(std::ceil(std::max(std::abs(d.x), std::abs(d.y)))));
  for (int s = 0; s <= steps; ++s) {
    const glm::vec2 p = a + d * (static_cast<float>(s) / steps);
    const int x = static_cast<int>(std::lround(p.x));
    const int y = static_cast<int>(std::lround(p.y));
    if (x < 0 || y < 0 || x >= w || y >= h) continue;
    put(rgba, (static_cast<size_t>(y) * w + x) * 4, tone);
  }
}

}  // namespace

std::vector<uint8_t> encode_height_water_rgba(const Field2D<float>& height,
                                              const Field2D<float>& water_depth,
                                              ExportRange range,
                                              float water_max_m) {
  const int w = height.width, h = height.height;
  if (w <= 0 || h <= 0) return {};

  const float span = range.hi_m - range.lo_m;
  const bool live_range = span > 0.0f;
  // A water field of the wrong size is a caller mistake, not a reason to read
  // out of bounds: encode the patch as dry and let the image show it.
  const bool live_water = water_max_m > 0.0f &&
                          water_depth.width == w && water_depth.height == h;

  std::vector<uint8_t> rgba(static_cast<size_t>(w) * h * 4);
  for (int y = 0; y < h; ++y) {
    for (int x = 0; x < w; ++x) {
      const uint8_t hc =
          live_range ? to_code((height.at(x, y) - range.lo_m) / span) : 128;
      const int wc =
          live_water ? to_code(water_depth.at(x, y) / water_max_m) : 0;
      const size_t i = (static_cast<size_t>(y) * w + x) * 4;
      rgba[i] = hc;
      rgba[i + 1] = hc;
      rgba[i + 2] = static_cast<uint8_t>(std::min(255, hc + wc));
      rgba[i + 3] = 255;
    }
  }
  return rgba;
}

std::vector<uint8_t> encode_biome_rgba(const Field2D<uint8_t>& biome) {
  const int w = biome.width, h = biome.height;
  if (w <= 0 || h <= 0) return {};

  std::vector<uint8_t> rgba(static_cast<size_t>(w) * h * 4);
  for (int y = 0; y < h; ++y) {
    for (int x = 0; x < w; ++x) {
      const uint8_t id = biome.at(x, y);
      const Rgb tone = id < kBiomeCount ? kBiomePalette[id] : Rgb{0, 0, 0};
      put(rgba, (static_cast<size_t>(y) * w + x) * 4, tone);
    }
  }
  return rgba;
}

std::vector<uint8_t> encode_hillshade_rgba(const PatchData& patch) {
  const Field2D<float>& height = patch.height;
  const int w = height.width, h = height.height;
  if (w <= 0 || h <= 0) return {};
  const float texel_m = patch.texel_m > 0.0f ? patch.texel_m : 1.0f;
  const bool live_water = patch.water_depth.width == w &&
                          patch.water_depth.height == h;

  std::vector<uint8_t> rgba(static_cast<size_t>(w) * h * 4);
  for (int y = 0; y < h; ++y) {
    for (int x = 0; x < w; ++x) {
      const size_t i = (static_cast<size_t>(y) * w + x) * 4;
      const float depth = live_water ? patch.water_depth.at(x, y) : 0.0f;
      if (depth > 0.0f) {
        // Water is drawn FLAT: it is a surface of its own, and relighting it
        // with the bed's normal would print the bed's relief onto a lake.
        put(rgba, i, lerp_rgb(kWaterShallow, kWaterDeep,
                              depth / kExportWaterDepthM));
        continue;
      }
      const glm::vec3 n = glm::normalize(glm::vec3(
          -slope_x(height, x, y, texel_m), -slope_y(height, x, y, texel_m),
          1.0f));
      const float lambert =
          std::max(0.0f, n.x * kSunX + n.y * kSunY + n.z * kSunZ);
      const float lit = kAmbient + (1.0f - kAmbient) * lambert;
      put(rgba, i,
          {to_code(kLandTone.r / 255.0f * lit), to_code(kLandTone.g / 255.0f * lit),
           to_code(kLandTone.b / 255.0f * lit)});
    }
  }

  // Channels last, so a reach stays visible over whatever it crosses. The graph
  // is already clipped to the patch and in PATCH-LOCAL metres, so texel
  // coordinates are just a division by the spacing.
  for (const RiverEdge& e : patch.rivers.edges) {
    for (size_t k = 1; k < e.points_m.size(); ++k) {
      draw_segment(rgba, w, h, e.points_m[k - 1] / texel_m,
                   e.points_m[k] / texel_m, kRiverTone);
    }
  }
  return rgba;
}

}  // namespace badlands::mapgen
