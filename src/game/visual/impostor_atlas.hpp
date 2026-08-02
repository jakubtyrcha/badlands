#pragma once

// The baked product of the foliage impostor (LOD4): two texture arrays holding
// every model's octahedral views, plus the arithmetic that says where a view
// lives inside them.
//
// The layout half is free functions with no GPU types, so the baker (which
// writes tiles) and the material (which reads them) share one source of truth
// and can both be tested without a device -- the two halves disagreeing is a
// failure that renders as a plausible-looking wrong tree rather than an error.

#include <cstdint>

#include <dawn/webgpu_cpp.h>
#include <glm/glm.hpp>

#include "engine/rendering/material/material_instance_factory.hpp"
#include "game/visual/octahedral.hpp"

namespace badlands {

// One view's resolution in the atlas. At LOD4 a tree covers a few dozen pixels,
// so 128 is already generous; it is a power of two so the mip chain is exact.
inline constexpr uint32_t kImpostorTilePx = 128;

// Views are laid out as a kImpostorViewsPerAxis square grid within one array
// LAYER, and there is one layer per MODEL.
//
// Layer-per-model, deliberately, and the alternative is worth recording: a
// layer per (model, view) would make tile bleed structurally impossible, since
// filtering and mips never cross array layers. It also caps the forest at
// maxTextureArrayLayers / 16 = 16 models -- which the current pine forest sits
// exactly on, and the earlier mixed forest (28 models) would have blown. A hard
// cliff at the content's current size is not a tradeoff worth taking for a
// problem the mip policy below already solves.
inline constexpr uint32_t kImpostorLayerPx =
    kImpostorTilePx * static_cast<uint32_t>(kImpostorViewsPerAxis);

// The mip chain stops while a tile is still 4x4, rather than running to 1x1.
// Past that a tile carries less information than the blend of three of them
// needs, and every extra level is another 16 renders per model in the bake for
// a level nothing samples: at LOD4 range the impostor is tens of pixels, which
// is mip 1-2 of a 128 tile.
inline constexpr uint32_t kImpostorMinTileMipPx = 4;
inline constexpr uint32_t kImpostorMipLevels = 6;
static_assert(kImpostorTilePx >> (kImpostorMipLevels - 1) ==
                  kImpostorMinTileMipPx,
              "kImpostorMipLevels must be the chain from kImpostorTilePx down "
              "to kImpostorMinTileMipPx");

// The alpha threshold the atlas is built around, and the ONLY one a consumer
// may cut at.
//
// It is not a free parameter. The bake fits every coarser mip's alpha so its
// coverage AT THIS VALUE matches mip 0's (see alpha_coverage.hpp), so a
// material that cuts somewhere else gets a silhouette that drifts with mip
// level -- cutting lower admits texels the fit pushed just under, and the tree
// GROWS with distance, which is the Castano failure inverted. Mip 0 is a binary
// mask, so the mistake is invisible up close and only appears far away.
//
// Deliberately not the model's own `leaves.alpha_cutoff`: that described the
// leaf CARDS, and the bake draws the voxel crown, which is opaque.
inline constexpr float kImpostorAlphaCutoff = 0.5f;
inline constexpr uint8_t kImpostorAlphaCutoffByte = 128;

// A view's pixel square inside a layer, at a given mip.
struct ImpostorTileRect {
  uint32_t x = 0;
  uint32_t y = 0;
  uint32_t size = 0;
};

inline ImpostorTileRect ImpostorTilePixels(int i, int j, uint32_t mip) {
  const uint32_t tile = kImpostorTilePx >> mip;
  return ImpostorTileRect{static_cast<uint32_t>(i) * tile,
                          static_cast<uint32_t>(j) * tile, tile};
}

// Layer-space uv for a coordinate within view (i, j)'s tile, at `mip`.
//
// `local_uv` in [0,1] maps onto the tile's TEXEL CENTRES, not onto its outer
// edges. That is the whole no-bleed policy, and it is a correct mapping rather
// than a margin sacrificed to safety: a tile of T texels has its centres at
// 0.5/T .. (T-0.5)/T, and sampling outside that range is what reaches into the
// neighbouring view. Because the range is computed at the SAMPLED mip, the
// inset is exactly half a texel at every level instead of being sized for the
// coarsest -- which at a 4x4 tile would otherwise have cost 12.5% of the tile
// on every side, at every level.
//
// This is why the material must sample with an EXPLICIT LOD: it needs the mip
// to compute the range. That suits an alpha-cutout impostor anyway, whose
// derivative-driven LOD is unreliable right at the silhouette.
inline glm::vec2 ImpostorTileUv(int i, int j, glm::vec2 local_uv,
                                uint32_t mip) {
  const float layer = static_cast<float>(kImpostorLayerPx >> mip);
  const float tile = static_cast<float>(kImpostorTilePx >> mip);
  const glm::vec2 origin(static_cast<float>(i) * tile,
                         static_cast<float>(j) * tile);
  // Texel centre 0.5 .. tile-0.5, in the mip's own pixel space.
  const glm::vec2 texel = origin + 0.5f + local_uv * (tile - 1.0f);
  return texel / layer;
}

// The uv range ImpostorTileUv can produce for view (i, j) at `mip` -- i.e. the
// tile's texel-centre extent. Exposed so the layout can be checked directly.
struct ImpostorUvRect {
  glm::vec2 min{0.0f};
  glm::vec2 max{0.0f};
};

inline ImpostorUvRect ImpostorTileUvRange(int i, int j, uint32_t mip) {
  return ImpostorUvRect{ImpostorTileUv(i, j, glm::vec2(0.0f), mip),
                        ImpostorTileUv(i, j, glm::vec2(1.0f), mip)};
}

// Everything the runtime needs to place a model's impostor quad, in the tree's
// NATIVE units (the caller scales by native_to_world_scale like any other tree
// geometry).
//
// A single `radius` for all views, not a per-view tight fit: the quad has one
// size, and the blend samples three views at the same local uv, so every view
// has to share a frame. The bounding SPHERE is that frame -- the only extent
// guaranteed not to clip from any direction. A tree is much taller than wide,
// so the tile is well used vertically and loosely across; that is the price of
// one shared frame.
struct ImpostorPlacement {
  glm::vec3 local_center{0.0f};
  float radius = 0.0f;
};

// The two arrays, one layer per model.
//
//   albedo   RGBA8 : rgb = albedo, a = coverage (the alpha-test channel)
//   surface  RGBA8 : rg = octahedral-encoded TREE-LOCAL normal,
//                    b = translucency strength, a = AO
//
// The normal is local, not world: the runtime rotates it by the instance's yaw,
// which is a 2D rotation of n.xz since instances only ever turn about Y.
struct ImpostorAtlas {
  wgpu::Texture albedo;
  wgpu::Texture surface;
  wgpu::TextureView albedo_view;   // 2DArray
  wgpu::TextureView surface_view;  // 2DArray
  wgpu::Sampler sampler;
  uint32_t model_count = 0;

  bool valid() const {
    return albedo_view && surface_view && sampler && model_count > 0;
  }
};

// Allocates an EMPTY atlas for `model_count` models. Returns an invalid atlas
// (after logging) if the model count is zero or exceeds the device's array
// layer limit.
ImpostorAtlas CreateImpostorAtlas(wgpu::Device device, uint32_t model_count);

// Pushes both arrays onto `params` as instance overrides, or returns false
// (after logging) if the atlas is not valid.
//
// Binding is MANDATORY, and this exists so no caller can forget. The factory's
// unbound-slot fallback picks its dimensionality per GEOMETRY type, so it
// cannot supply a texture_2d_array default for a material that also has 2D
// slots -- the same reason ClusterTerrain::Build checks its terrain views up
// front (see standard_material_factory.cpp's note on kTerrainCluster). An
// unbound slot here would not render neutrally; it would reach Dawn as a
// dimension mismatch at draw time.
bool BindImpostorAtlas(const ImpostorAtlas& atlas, InstanceParams& params);

}  // namespace badlands
