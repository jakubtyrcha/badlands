#include "mapgen/outputs.hpp"

#include <algorithm>
#include <cstdint>
#include <limits>

#include "core/util/cpu_image.hpp"
#include "mapgen/biomes.hpp"

namespace badlands::mapgen {

void write_preview_images(const std::string& out_dir, const MapArtifacts& a) {
  write_gray_png(a.bedrock, out_dir + "/bedrock.png");
  write_biome_png(a.biome, out_dir + "/biome.png");
  write_gray_png(a.heightmap, out_dir + "/heightmap.png");
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

}  // namespace badlands::mapgen
