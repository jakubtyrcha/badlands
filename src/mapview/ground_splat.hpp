#pragma once

// THE GROUND-MATERIAL SEAM. One function turns a patch into the 8 blend weights
// the terrain material samples, plus the packs those 8 slots bind to.
//
// WHAT THIS REPLACED, AND WHY. The weights used to be a one-hot of the map's
// biome raster. That pinned every material edge to the classification's own
// lattice -- 10 m, where the cover source is land cover -- against a 1 m
// heightfield, and it meant the ground could only look like as many things as
// there were biomes. Weights derived from slope, curvature and soil resolve at
// the heightfield's resolution and vary continuously across one class.
//
// THE SLOT MEANINGS AND THE PALETTE ARE PROVISIONAL. They exist so there is
// something on screen to judge; the real material-variation work (alternating
// brushes) will replace the derivation wholesale. Nothing here is in the patch
// contract, and no consumer reads it -- that is the point of it being one
// function. shaders/common/terrain_layers.wesl is NOT touched by any of this:
// its 8 weight slots, RGBA8x2 planes and world-XZ sampling are LOD-independent
// and correct, and only what FILLS them changes.

#include <cstdint>
#include <string>
#include <vector>

#include "mapgen/patch_data.hpp"

namespace badlands {

// Blur radius applied to the derived weights, in world metres. Softens the
// per-texel decision into a transition band wide enough to read as a blend.
inline constexpr float kGroundBlendM = 3.0f;

// The 8 slots, in texture-array layer order. Chosen so each is something the
// derivation can actually separate -- a slot no input can distinguish would
// never be reached, and a slot two inputs both claim would flicker.
enum class GroundSlot : uint8_t {
  BareRock = 0,   // steep, thin soil
  Scree,          // steep and convergent -- below a face, where debris collects
  StonyGround,    // moderate slope, thin soil
  Turf,           // gentle, soil, dry, grass or crop cover
  Heath,          // gentle, soil, shrub or moss cover
  Peat,           // flat and wettest, wetland cover
  Silt,           // lake margin
  ForestFloor,    // tree cover
};

inline constexpr int kGroundSlotCount = 8;

// Names, which are also the keys in assets/materials/terrain_ground.json.
const char* ground_slot_name(GroundSlot s);

// Two RGBA8 rasters holding the 8 weight slots, sized to the patch's rasters.
// slots0 = slots 0..3, slots1 = slots 4..7; both tightly packed RGBA8 and
// `width * height * 4` bytes long. The weights at a texel sum to exactly 255.
struct GroundSplat {
  int width = 0;
  int height = 0;
  std::vector<uint8_t> slots0;
  std::vector<uint8_t> slots1;

  bool empty() const { return width <= 0 || height <= 0; }
};

// Derives the weights from `patch`'s height (slope and curvature), soil and
// cover. An empty patch returns an empty splat.
//
// Only the two strongest weights per texel survive: the fragment shader's cost
// is proportional to the number of non-zero layers, and bilinear filtering of a
// 2-weight texel can already light up 4 at a triple point.
GroundSplat BuildGroundSplat(const mapgen::PatchData& patch);

// Resolves the 8 slots to PBR pack directories through `manifest_path`, writing
// them into `out_pack_dirs` in SLOT ORDER -- index i is the pack for slot i,
// which is exactly the terrain array's layer index.
//
// The manifest is keyed by terrain class, then by slot name: a granite tor and
// a limestone dale want different rock. A class with no entry falls back to the
// manifest's "default" block, so an unlabelled patch still renders.
//
// Returns false (after logging) on a missing or unreadable file, unparseable
// JSON, an absent "default" block, or a slot missing from the block that was
// selected. Keyed by NAME rather than position so a reordered or renamed entry
// fails loudly instead of silently binding the wrong texture.
bool ResolveGroundPacks(const std::string& manifest_path,
                        mapgen::TerrainClass terrain_class,
                        std::vector<std::string>& out_pack_dirs);

}  // namespace badlands
