// leaf_texture.cpp
#include "game/geometry/leaf_texture.hpp"
#include <algorithm>
#include <cmath>
#include <glm/gtc/constants.hpp>  // glm::pi
namespace badlands {

std::vector<uint8_t> BuildLeafRgba8(int size, glm::vec3 leaf_color) {
  std::vector<uint8_t> rgba(static_cast<size_t>(size) * static_cast<size_t>(size) * 4, 0);
  auto to_byte = [](float c) {
    return static_cast<uint8_t>(std::lround(std::clamp(c, 0.0f, 1.0f) * 255.0f));
  };
  const uint8_t r = to_byte(leaf_color.r);
  const uint8_t g = to_byte(leaf_color.g);
  const uint8_t b = to_byte(leaf_color.b);
  const float W = 0.60f;                                  // max half-width (fraction of half-size)
  const float edge = 2.0f / static_cast<float>(size);     // ~2-texel soft edge

  for (int y = 0; y < size; ++y) {
    for (int x = 0; x < size; ++x) {
      const float u = (static_cast<float>(x) + 0.5f) / static_cast<float>(size) * 2.0f - 1.0f;
      const float v = (static_cast<float>(y) + 0.5f) / static_cast<float>(size) * 2.0f - 1.0f;
      const float t = (v + 1.0f) * 0.5f;                  // 0 = base, 1 = tip
      const float half_w = W * std::sin(glm::pi<float>() * t);  // 0 at both ends, W at middle
      const float d = half_w - std::fabs(u);              // >0 inside the silhouette
      const float a = std::clamp(d / edge + 0.5f, 0.0f, 1.0f);
      const size_t idx = (static_cast<size_t>(y) * static_cast<size_t>(size) +
                          static_cast<size_t>(x)) * 4;
      rgba[idx + 0] = r;
      rgba[idx + 1] = g;
      rgba[idx + 2] = b;
      rgba[idx + 3] = static_cast<uint8_t>(std::lround(a * 255.0f));
    }
  }
  return rgba;
}

}  // namespace badlands
