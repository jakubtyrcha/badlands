#include "mapgen/hillshade.hpp"

#include <algorithm>
#include <cmath>

#include <glm/glm.hpp>

#include "core/util/cpu_image.hpp"

namespace badlands::mapgen {

namespace {

// Cartographic convention: sun from the north-west, 45 degrees up. Lighting from the
// top-left is what makes relief read as raised rather than inverted-sunken.
constexpr float kSunAzimuthDeg = 315.0f;
constexpr float kSunAltitudeDeg = 45.0f;
// Floor on the shadowed side so its shape stays readable instead of crushing to black.
constexpr float kAmbient = 0.25f;

}  // namespace

Field2D<float> compute_hillshade(const Field2D<float>& height,
                                 float meters_per_sample) {
  Field2D<float> shade;
  if (height.width <= 0 || height.height <= 0) return shade;
  shade = Field2D<float>(height.width, height.height);
  const float mps = std::max(meters_per_sample, 1e-6f);

  const float az = glm::radians(kSunAzimuthDeg);
  const float alt = glm::radians(kSunAltitudeDeg);
  // Toward the sun. Azimuth measured clockwise from +Z ("north"), matching the map's
  // convention that +Z is south-ish in world space.
  const glm::vec3 light = glm::normalize(
      glm::vec3(std::sin(az) * std::cos(alt), std::sin(alt),
                std::cos(az) * std::cos(alt)));

  for (int z = 0; z < height.height; ++z) {
    for (int x = 0; x < height.width; ++x) {
      // Central differences, clamped at the border (one-sided there). dh/dx is in
      // meters per meter, hence the 2*mps -- this is where meters_per_sample makes the
      // slope real rather than a per-sample fiction.
      const int xm = std::max(x - 1, 0);
      const int xp = std::min(x + 1, height.width - 1);
      const int zm = std::max(z - 1, 0);
      const int zp = std::min(z + 1, height.height - 1);
      const float dhdx =
          (height.at(xp, z) - height.at(xm, z)) / ((xp - xm) * mps);
      const float dhdz =
          (height.at(x, zp) - height.at(x, zm)) / ((zp - zm) * mps);

      // Surface normal of h(x,z): (-dh/dx, 1, -dh/dz), normalized.
      const glm::vec3 n = glm::normalize(glm::vec3(-dhdx, 1.0f, -dhdz));
      const float diffuse = std::max(glm::dot(n, light), 0.0f);
      shade.at(x, z) =
          std::clamp(kAmbient + (1.0f - kAmbient) * diffuse, 0.0f, 1.0f);
    }
  }
  return shade;
}

Field2D<float> upscale_nearest(const Field2D<float>& field, int factor) {
  if (factor <= 1 || field.width <= 0 || field.height <= 0) return field;
  Field2D<float> out(field.width * factor, field.height * factor);
  for (int y = 0; y < out.height; ++y) {
    for (int x = 0; x < out.width; ++x) {
      out.at(x, y) = field.at(x / factor, y / factor);
    }
  }
  return out;
}

void write_hillshade_png(const Field2D<float>& height, const std::string& path,
                         float meters_per_sample) {
  const Field2D<float> shade = compute_hillshade(height, meters_per_sample);
  if (shade.width <= 0) return;
  badlands::CpuImage img(static_cast<uint32_t>(shade.width),
                         static_cast<uint32_t>(shade.height),
                         wgpu::TextureFormat::R8Unorm);
  for (int y = 0; y < shade.height; ++y) {
    for (int x = 0; x < shade.width; ++x) {
      const float s = shade.at(x, y);
      img.SetPixelF32(static_cast<uint32_t>(x), static_cast<uint32_t>(y),
                      {s, s, s, 1.0f});
    }
  }
  img.WritePng(path);
}

}  // namespace badlands::mapgen
