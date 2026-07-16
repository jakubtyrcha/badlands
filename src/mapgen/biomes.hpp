#pragma once

#include <array>
#include <cstdint>
#include <string_view>

namespace badlands::mapgen {

// The five map biomes. Ordered low-elevation -> high-elevation, which the
// Whittaker-style assignment (biomes.cpp) and the debug palette rely on.
enum class Biome : uint8_t {
  Lake = 0,
  Swamp,
  Forest,
  Plains,
  Hills,
};

inline constexpr int kBiomeCount = 5;

struct Rgb {
  uint8_t r, g, b;
};

// Debug palette for biome.png (indexed color per biome).
inline constexpr std::array<Rgb, kBiomeCount> kBiomePalette{{
    {40, 90, 160},   // Lake   - water blue
    {74, 96, 70},    // Swamp  - murky green
    {34, 110, 44},   // Forest - deep green
    {156, 178, 96},  // Plains - pale yellow-green
    {134, 112, 88},  // Hills  - earthy brown
}};

inline constexpr Rgb biome_color(Biome b) {
  return kBiomePalette[static_cast<int>(b)];
}

inline constexpr std::string_view biome_name(Biome b) {
  switch (b) {
    case Biome::Lake:
      return "lake";
    case Biome::Swamp:
      return "swamp";
    case Biome::Forest:
      return "forest";
    case Biome::Plains:
      return "plains";
    case Biome::Hills:
      return "hills";
  }
  return "unknown";
}

}  // namespace badlands::mapgen
