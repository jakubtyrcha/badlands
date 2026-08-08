#pragma once

// SURFACE COVER: what is on the ground, for shading and for placing vegetation.
//
// Deliberately NOT mapgen::Biome, which is the SIMULATION's vocabulary --
// walkability, movement cost, critter habitat, animal spawning -- and is frozen
// across the C ABI (badlands_sim.hpp's BiomeAt). The two answer different
// questions for different consumers and no code reads both. See
// docs/superpowers/specs/2026-08-07-detailed-patch-rendering-design.md.
//
// This is BADLANDS' vocabulary, not ESA WorldCover's, even though the terrain-net
// provider is the only thing filling it today. Production terrain will not come
// from WorldCover at all, and a contract carrying ESA's class numbering would
// have to be renegotiated the moment it arrives. Providers translate.
//
// Cover does NOT index the terrain shader's weight slots -- ground material is
// derived from slope, curvature and soil at full resolution (see
// mapview/ground_splat.hpp) -- so unlike Biome this enum is under no 8-value
// cap and can grow as the sources get finer.

#include <array>
#include <cstdint>
#include <string_view>

#include "mapgen/rgb.hpp"

namespace badlands::mapgen {

// Unknown is 0 so a default-constructed or zero-filled raster is honestly
// unknown rather than plausibly vegetated. That distinction is load-bearing:
// a sentinel indistinguishable from a legitimate class is one nobody notices.
enum class Cover : uint8_t {
  Unknown = 0,
  Tree,
  Shrub,
  Grass,
  Crop,
  Built,
  Bare,
  Snow,
  Water,
  Wetland,
  Moss,
};

inline constexpr int kCoverCount = 11;

// Debug palette for cover.png and the cluster vertex tint.
inline constexpr std::array<Rgb, kCoverCount> kCoverPalette{{
    {255, 0, 200},   // Unknown - magenta, so it cannot be mistaken for terrain
    {56, 96, 56},    // Tree    - deep green
    {104, 124, 72},  // Shrub   - olive
    {120, 152, 88},  // Grass   - pale green
    {168, 160, 96},  // Crop    - straw
    {176, 92, 92},   // Built   - brick
    {150, 122, 92},  // Bare    - earth
    {238, 240, 245}, // Snow    - near-white
    {48, 96, 160},   // Water   - blue
    {86, 132, 120},  // Wetland - teal
    {132, 148, 108}, // Moss    - grey-green
}};

inline constexpr Rgb cover_color(Cover c) {
  return kCoverPalette[static_cast<int>(c)];
}

inline constexpr std::string_view cover_name(Cover c) {
  switch (c) {
    case Cover::Unknown:
      return "unknown";
    case Cover::Tree:
      return "tree";
    case Cover::Shrub:
      return "shrub";
    case Cover::Grass:
      return "grass";
    case Cover::Crop:
      return "crop";
    case Cover::Built:
      return "built";
    case Cover::Bare:
      return "bare";
    case Cover::Snow:
      return "snow";
    case Cover::Water:
      return "water";
    case Cover::Wetland:
      return "wetland";
    case Cover::Moss:
      return "moss";
  }
  return "unknown";
}

}  // namespace badlands::mapgen
