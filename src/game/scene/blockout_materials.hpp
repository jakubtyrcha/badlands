#pragma once

// Shared blockout / greybox debug colors: one source of truth for the game's
// blockout terrain + buildings and the ai_sandbox arena. Header-only (pure
// color data, no deps beyond glm).
//
// All colors are sRGB in [0,1]. The deferred lighting pass re-linearizes the
// G-buffer albedo, so passing these straight to MaterialLibrary::SolidColor /
// DebugTerrainArrays renders them as the intended sRGB color (the same
// convention the ai_sandbox debug materials already rely on).

#include <vector>

#include <glm/glm.hpp>

namespace badlands::blockout {

// Per-biome debug albedo, ordered by mapgen::Biome value (Lake, Swamp, Forest,
// Plains, Hills, Mountain) so the index doubles as the terrain-array layer. The
// symbolic map uses Lake/Swamp/Forest/Plains (the user-specified colors);
// Hills/Mountain are placeholders so the array covers every biome index.
inline std::vector<glm::vec3> BiomeColors() {
  auto hex = [](int r, int g, int b) {
    return glm::vec3(r / 255.0f, g / 255.0f, b / 255.0f);
  };
  return {
      hex(0xb8, 0xbd, 0xb5),  // Lake   — lake bottom  #b8bdb5
      hex(0x33, 0x3d, 0x29),  // Swamp                 #333d29
      hex(0x58, 0x2f, 0x0e),  // Forest — woodland     #582f0e
      hex(0xa4, 0xac, 0x86),  // Plains                #a4ac86
      hex(0x86, 0x70, 0x58),  // Hills   (placeholder)
      hex(0xa8, 0xa8, 0xae),  // Mountain (placeholder)
  };
}

// Building blockout materials: walls light, roofs darker; matte.
inline constexpr glm::vec3 kWall{0.72f, 0.72f, 0.70f};
inline constexpr glm::vec3 kRoof{0.40f, 0.34f, 0.30f};
inline constexpr float kBuildingRoughness = 0.95f;

// ai_sandbox arena + capsules (lifted here so the debug-material path is shared).
// kArenaGray is pre-encoded so that after deferred_lighting's srgb_to_linear it
// lands at ~0.75 linear reflectance (linear_to_srgb(0.75) ~= 0.881).
inline constexpr glm::vec3 kArenaGray{0.881f};
inline constexpr float kArenaRoughness = 1.0f;
inline constexpr glm::vec3 kCapsuleRed{200.0f / 255.0f, 30.0f / 255.0f,
                                       30.0f / 255.0f};
inline constexpr glm::vec3 kCapsuleBlue{30.0f / 255.0f, 60.0f / 255.0f,
                                        200.0f / 255.0f};
inline constexpr float kCapsuleRoughness = 140.0f / 255.0f;

}  // namespace badlands::blockout
