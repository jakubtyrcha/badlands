#include "game/visual/impostor_atlas.hpp"

#include <spdlog/spdlog.h>

namespace badlands {

namespace {

// WebGPU's guaranteed floor for maxTextureArrayLayers. Checked against rather
// than queried: the point is to fail on the machine that BUILDS content, not
// only on the one machine whose driver happens to be stingier.
constexpr uint32_t kMinGuaranteedArrayLayers = 256;

wgpu::Texture CreateArray(wgpu::Device device, uint32_t layers,
                          const char* label) {
  wgpu::TextureDescriptor desc;
  desc.label = label;
  desc.dimension = wgpu::TextureDimension::e2D;
  desc.size = {kImpostorLayerPx, kImpostorLayerPx, layers};
  desc.format = wgpu::TextureFormat::RGBA8Unorm;
  desc.mipLevelCount = kImpostorMipLevels;
  // RenderAttachment: the bake draws mip 0 straight into each tile's viewport.
  // CopySrc: mip 0 is read back so the mip chain can be filtered on the CPU
  // (coverage preservation needs a search, see alpha_coverage.hpp).
  // CopyDst: those filtered levels are written back.
  desc.usage = wgpu::TextureUsage::TextureBinding |
               wgpu::TextureUsage::RenderAttachment |
               wgpu::TextureUsage::CopySrc | wgpu::TextureUsage::CopyDst;
  return device.CreateTexture(&desc);
}

wgpu::TextureView CreateArrayView(wgpu::Texture texture, const char* label) {
  wgpu::TextureViewDescriptor desc;
  desc.label = label;
  desc.dimension = wgpu::TextureViewDimension::e2DArray;
  desc.baseMipLevel = 0;
  desc.mipLevelCount = kImpostorMipLevels;
  return texture.CreateView(&desc);
}

}  // namespace

ImpostorAtlas CreateImpostorAtlas(wgpu::Device device, uint32_t model_count) {
  ImpostorAtlas atlas;
  if (model_count == 0) {
    spdlog::error("CreateImpostorAtlas: model_count is 0");
    return atlas;
  }
  if (model_count > kMinGuaranteedArrayLayers) {
    spdlog::error(
        "CreateImpostorAtlas: {} models exceeds the {}-layer array limit -- "
        "the atlas is one layer per model, so a forest cannot carry more",
        model_count, kMinGuaranteedArrayLayers);
    return atlas;
  }

  atlas.albedo = CreateArray(device, model_count, "impostor_albedo");
  atlas.surface = CreateArray(device, model_count, "impostor_surface");
  if (!atlas.albedo || !atlas.surface) {
    spdlog::error("CreateImpostorAtlas: texture creation failed for {} models",
                  model_count);
    return {};
  }

  atlas.albedo_view = CreateArrayView(atlas.albedo, "impostor_albedo_view");
  atlas.surface_view = CreateArrayView(atlas.surface, "impostor_surface_view");

  wgpu::SamplerDescriptor samp;
  samp.label = "impostor_sampler";
  // ClampToEdge is a backstop, not the no-bleed mechanism -- that is the
  // texel-centre uv range (see ImpostorTileUv). Clamping only helps at the
  // LAYER's border; a tile in the middle of a layer has neighbours on every
  // side and clamping would do nothing for it.
  samp.addressModeU = wgpu::AddressMode::ClampToEdge;
  samp.addressModeV = wgpu::AddressMode::ClampToEdge;
  samp.addressModeW = wgpu::AddressMode::ClampToEdge;
  samp.magFilter = wgpu::FilterMode::Linear;
  samp.minFilter = wgpu::FilterMode::Linear;
  // Nearest between mips, not Linear: the material samples an EXPLICIT LOD and
  // insets uv for THAT level, so a trilinear blend would mix in a level whose
  // texel-centre range is different -- reintroducing exactly the cross-tile
  // reach the inset exists to prevent.
  samp.mipmapFilter = wgpu::MipmapFilterMode::Nearest;
  samp.lodMinClamp = 0.0f;
  samp.lodMaxClamp = static_cast<float>(kImpostorMipLevels - 1);
  atlas.sampler = device.CreateSampler(&samp);

  if (!atlas.albedo_view || !atlas.surface_view || !atlas.sampler) {
    spdlog::error("CreateImpostorAtlas: view/sampler creation failed");
    return {};
  }

  atlas.model_count = model_count;
  spdlog::info(
      "impostor atlas: {} models x {} views, {}px tiles ({}px layers, {} mips)",
      model_count, kImpostorViewCount, kImpostorTilePx, kImpostorLayerPx,
      kImpostorMipLevels);
  return atlas;
}

bool BindImpostorAtlas(const ImpostorAtlas& atlas, InstanceParams& params) {
  if (!atlas.valid()) {
    spdlog::error(
        "BindImpostorAtlas: atlas is not valid (albedo={} surface={} "
        "sampler={} models={}) -- both arrays MUST be bound, since the "
        "factory's per-geometry default cannot supply an array view",
        static_cast<bool>(atlas.albedo_view),
        static_cast<bool>(atlas.surface_view),
        static_cast<bool>(atlas.sampler), atlas.model_count);
    return false;
  }

  // TextureType::k2D on an override is correct even for an array view: on the
  // override path the type only filters DEFAULT-view recipes, which are not
  // consulted when an override exists. ClusterTerrain::Build binds its three
  // texture_2d_arrays the same way.
  // Slot names are "tex_<binding>", NOT the shader's own identifiers. A shader
  // with no hand-written entry in the engine's requirements registry falls back
  // to DeriveRequirementsFromReflection, which names every group-0 texture slot
  // by its binding index -- the same deviation tree_field.hpp documents for
  // instanced_gbuffer's bark support textures. Using the literal names here
  // would silently no-op the override and leave a 2D default bound to an array
  // slot, which is a draw-time dimension mismatch rather than a visible one.
  params.texture_overrides.push_back(
      DefaultTextureView{.param_name = "tex_1",
                         .view = atlas.albedo_view,
                         .sampler = atlas.sampler,
                         .type = TextureType::k2D});
  params.texture_overrides.push_back(
      DefaultTextureView{.param_name = "tex_2",
                         .view = atlas.surface_view,
                         .sampler = atlas.sampler,
                         .type = TextureType::k2D});
  return true;
}

}  // namespace badlands
