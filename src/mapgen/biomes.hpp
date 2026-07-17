#pragma once

#include <array>
#include <cstdint>
#include <string_view>

namespace badlands::mapgen {

// The map biomes. Ordered low-elevation -> high-elevation, which the
// Whittaker-style assignment (biome_assign.cpp) and the debug palette rely on.
// Mountain is authored-map only: classify_biome never emits it.
enum class Biome : uint8_t {
  Lake = 0,
  Swamp,
  Forest,
  Plains,
  Hills,
  Mountain,
};

inline constexpr int kBiomeCount = 6;

// terrain_blend.wesl carries 8 dense weight slots and uses the slot index AS the
// texture-array layer (`min(i, max_albedo)`), so today the palette is capped at 8
// globally. That is a limit of the BATCH, not of the format: the vertex attribute
// is Uint8x4, i.e. 256 layers. Giving each chunk a local->global layer map would
// turn this into "8 distinct biomes per chunk" and lift the global cap to 256.
// Until then a 9th biome would hit `min(input.layer_indices[k], 7u)` and sample
// the wrong texture with no diagnostic, so fail the build instead.
static_assert(kBiomeCount <= 8,
              "terrain_blend.wesl has only 8 weight slots, indexed by biome; see "
              "the note above before growing the palette past 8");

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
    {168, 168, 174}, // Mountain - rocky gray
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
    case Biome::Mountain:
      return "mountain";
  }
  return "unknown";
}

}  // namespace badlands::mapgen
