#include "mapgen/outputs.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <string_view>
#include <utility>

#include "core/util/cpu_image.hpp"
#include "mapgen/biomes.hpp"
#include "mapgen/hillshade.hpp"
#include "mapgen/river_graph.hpp"

namespace badlands::mapgen {

namespace {
// log2(1 + max(0, v)) per texel: drainage area spans orders of magnitude, so
// a linear gray scale is dominated by a handful of bright river-mouth pixels.
Field2D<float> log2_scaled(const Field2D<float>& f) {
  Field2D<float> out(f.width, f.height, 0.0f);
  for (size_t i = 0; i < f.data.size(); ++i)
    out.data[i] = std::log2(1.0f + std::max(0.0f, f.data[i]));
  return out;
}

// The judging composite: biome palette base, standing water recolored Lake
// blue, and river/stream intensity blended toward the same blue where dry
// (v1.3 addendum: "biome palette, lakes, rivers overlaid in water blue").
// Per-class colour ramp for the river preview. Hierarchy has to be legible by
// COLOUR here: honest channel widths at this world scale are sub-texel, so the
// rasterized band is ~1 texel regardless of how much water a reach carries.
// A LUMINANCE ramp, not just a hue ramp: at a 512 m world only classes 1-3 are
// ever reached (measured Qmax ~0.0026 m^3/s across seeds 1-3), so those three
// have to be told apart at a glance on a 1-texel-wide line.
constexpr std::array<Rgb, kRiverClassCount> kRiverClassPalette{{
    {0, 0, 0},        // None
    {45, 70, 105},    // Rill
    {70, 150, 215},   // Brook
    {130, 215, 255},  // Stream
    {195, 240, 255},  // Creek
    {230, 250, 255},  // River
    {255, 255, 255},  // Major
}};

Rgb river_class_color(uint8_t cls) {
  return kRiverClassPalette[std::min<size_t>(cls, kRiverClassCount - 1)];
}

// Rivers as class tiers, NOT autoscaled grey: autoscaling would make a rill on
// one map read the same as a river on another, which is exactly the
// map-relative signal the absolute classification exists to avoid.
void write_river_class_png(const Field2D<uint8_t>& cls, const std::string& path) {
  badlands::CpuImage img(static_cast<uint32_t>(cls.width),
                         static_cast<uint32_t>(cls.height),
                         wgpu::TextureFormat::RGBA8Unorm);
  for (int y = 0; y < cls.height; ++y)
    for (int x = 0; x < cls.width; ++x) {
      const Rgb c = river_class_color(cls.at(x, y));
      img.SetPixel(static_cast<uint32_t>(x), static_cast<uint32_t>(y),
                   {c.r, c.g, c.b, 255});
    }
  img.WritePng(path);
}

void write_map_composite_png(const MapArtifacts& a, const std::string& path) {
  constexpr Rgb kWaterColor = biome_color(Biome::Lake);
  badlands::CpuImage img(static_cast<uint32_t>(a.biome.width),
                         static_cast<uint32_t>(a.biome.height),
                         wgpu::TextureFormat::RGBA8Unorm);
  for (int y = 0; y < a.biome.height; ++y) {
    for (int x = 0; x < a.biome.width; ++x) {
      Rgb c = biome_color(static_cast<Biome>(a.biome.at(x, y)));
      const float water = a.water_depth.at(x, y);
      const uint8_t cls = a.river_class.at(x, y);
      if (water > 0.0f) {
        c = kWaterColor;
      } else if (cls != 0) {
        // Blend strength rises with the tier, so a trunk reads stronger than a
        // headwater rill even though both occupy one texel.
        const float alpha =
            0.35f + 0.65f * static_cast<float>(cls) / static_cast<float>(kRiverClassCount - 1);
        c.r = static_cast<uint8_t>(std::lround(
            static_cast<float>(c.r) + (static_cast<float>(kWaterColor.r) - c.r) * alpha));
        c.g = static_cast<uint8_t>(std::lround(
            static_cast<float>(c.g) + (static_cast<float>(kWaterColor.g) - c.g) * alpha));
        c.b = static_cast<uint8_t>(std::lround(
            static_cast<float>(c.b) + (static_cast<float>(kWaterColor.b) - c.b) * alpha));
      }
      img.SetPixel(static_cast<uint32_t>(x), static_cast<uint32_t>(y),
                   {c.r, c.g, c.b, 255});
    }
  }
  img.WritePng(path);
}
}  // namespace

void write_preview_images(const std::string& out_dir, const MapArtifacts& a,
                          float texel_m) {
  write_gray_png(a.bedrock, out_dir + "/bedrock.png");
  write_biome_png(a.biome, out_dir + "/biome.png");
  write_gray_png(a.heightmap, out_dir + "/heightmap.png");
  write_hillshade_png(a.heightmap, out_dir + "/hillshade.png", texel_m);
  write_gray_png(a.water_depth, out_dir + "/water_depth.png");
  write_gray_png(log2_scaled(a.flow), out_dir + "/flow.png");
  write_gray_png(a.sediment, out_dir + "/sediment.png");
  write_river_class_png(a.river_class, out_dir + "/rivers.png");
  write_gray_png(a.river_depth_m, out_dir + "/river_depth.png");
  write_gray_png(a.river_speed_m_s, out_dir + "/river_speed.png");
  write_map_composite_png(a, out_dir + "/map.png");
}

void write_gray_png(const Field2D<float>& field, const std::string& path,
                    bool normalize) {
  float lo = 0.0f;
  float hi = 1.0f;
  if (normalize) {
    lo = std::numeric_limits<float>::max();
    hi = std::numeric_limits<float>::lowest();
    for (float v : field.data) {
      lo = std::min(lo, v);
      hi = std::max(hi, v);
    }
    if (!(hi > lo)) {  // constant field (or empty) — avoid divide-by-zero
      lo = 0.0f;
      hi = 1.0f;
    }
  }
  const float span = hi - lo;

  badlands::CpuImage img(static_cast<uint32_t>(field.width),
                         static_cast<uint32_t>(field.height),
                         wgpu::TextureFormat::R8Unorm);
  for (int y = 0; y < field.height; ++y) {
    for (int x = 0; x < field.width; ++x) {
      float t = (field.at(x, y) - lo) / span;
      t = std::clamp(t, 0.0f, 1.0f);
      img.SetPixelF32(static_cast<uint32_t>(x), static_cast<uint32_t>(y),
                      {t, t, t, 1.0f});
    }
  }
  img.WritePng(path);
}

void write_gray_png_range(const Field2D<float>& field, const std::string& path,
                          float lo, float hi) {
  const float span = (hi > lo) ? (hi - lo) : 1.0f;
  badlands::CpuImage img(static_cast<uint32_t>(field.width),
                         static_cast<uint32_t>(field.height),
                         wgpu::TextureFormat::R8Unorm);
  for (int y = 0; y < field.height; ++y) {
    for (int x = 0; x < field.width; ++x) {
      const float t = std::clamp((field.at(x, y) - lo) / span, 0.0f, 1.0f);
      img.SetPixelF32(static_cast<uint32_t>(x), static_cast<uint32_t>(y),
                      {t, t, t, 1.0f});
    }
  }
  img.WritePng(path);
}

void write_biome_png(const Field2D<uint8_t>& biome, const std::string& path) {
  badlands::CpuImage img(static_cast<uint32_t>(biome.width),
                         static_cast<uint32_t>(biome.height),
                         wgpu::TextureFormat::RGBA8Unorm);
  for (int y = 0; y < biome.height; ++y) {
    for (int x = 0; x < biome.width; ++x) {
      const Rgb c = biome_color(static_cast<Biome>(biome.at(x, y)));
      img.SetPixel(static_cast<uint32_t>(x), static_cast<uint32_t>(y),
                   {c.r, c.g, c.b, 255});
    }
  }
  img.WritePng(path);
}

PngDebugSink::PngDebugSink(std::string out_dir, float sim_texel_m,
                           float out_texel_m)
    : out_dir_(std::move(out_dir)),
      sim_texel_m_(sim_texel_m),
      out_texel_m_(out_texel_m) {}

namespace {
// "<NN>-<stage>.png" for init/output stages (NN = running sequence);
// "loop-<IIII>-<stage minus its "loop-" prefix>.png" for loop stages
// (IIII = the loop ITERATION the caller passed as `seq`).
std::string dump_path(const std::string& dir, std::string_view stage, int seq) {
  char buf[32];
  if (stage.rfind("loop-", 0) == 0) {
    std::snprintf(buf, sizeof buf, "loop-%04d-", seq);
    return dir + "/" + buf + std::string(stage.substr(5)) + ".png";
  }
  std::snprintf(buf, sizeof buf, "%02d-", seq);
  return dir + "/" + buf + std::string(stage) + ".png";
}
}  // namespace

void PngDebugSink::dump(std::string_view stage, int seq,
                        const Field2D<float>& field) {
  const std::string path = dump_path(out_dir_, stage, seq);
  const bool sim_relief = stage == "cone" || stage == "loop-height" ||
                          stage == "cavities-height" || stage == "canals";
  const bool out_relief = stage == "final-height" || stage == "pre-smooth-height";
  const bool flow = stage == "loop-flow" || stage == "flow";
  if (sim_relief) write_hillshade_png(field, path, sim_texel_m_);
  else if (out_relief) write_hillshade_png(field, path, out_texel_m_);
  else if (flow) write_gray_png(log2_scaled(field), path);
  else write_gray_png(field, path);
}

void PngDebugSink::dump(std::string_view stage, int seq,
                        const Field2D<uint8_t>& mask) {
  const std::string path = dump_path(out_dir_, stage, seq);
  if (stage == "biome" || stage == "biome-sim") {
    write_biome_png(mask, path);
    return;
  }
  if (stage == "river") {  // RiverClass tiers, not a 0/255 mask
    write_river_class_png(mask, path);
    return;
  }
  Field2D<float> f(mask.width, mask.height, 0.0f);
  for (size_t i = 0; i < f.data.size(); ++i) f.data[i] = mask.data[i] ? 1.0f : 0.0f;
  write_gray_png(f, path, /*normalize=*/false);
}

}  // namespace badlands::mapgen
