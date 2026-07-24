#include "engine/rendering/material_library.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <utility>

#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

#include "core/geometry_type.hpp"
#include "engine/rendering/checker_texture.hpp"
#include "engine/rendering/gbuffer.hpp"
#include "engine/rendering/scene_renderer.hpp"  // kAccumulationFormat / kDepthFormat

namespace badlands {

bool MaterialLibrary::Initialize(wgpu::Device device, wgpu::Queue queue,
                                 GpuPipelineGenerator* pipeline_gen) {
  device_ = device;
  queue_ = queue;
  pipeline_gen_ = pipeline_gen;

  // Deferred normalmapped factory: matches the B1 kDeferred descriptor
  // pattern (src/engine/app/placeholder_view.cpp) -- G-buffer MRT color
  // formats + reversed-Z depth so the compiled kGBuffer pipeline variant
  // matches what the G-buffer pass renders into.
  FactoryDescriptor desc;
  desc.shader_name = "normalmapped";
  desc.shader_path = "material/normalmapped.wesl";
  desc.supported_pass_types = {MaterialPassType::kDeferred};
  desc.supported_geometry_types = {GeometryType::kTexturedMesh};
  desc.color_formats = {GBuffer::kNormalsFormat, GBuffer::kAlbedoFormat,
                        GBuffer::kMaterialFormat};
  desc.depth_format = GBuffer::kDepthFormat;

  factory_ = BuildMaterialInstanceFactory(desc, device, queue, pipeline_gen,
                                          /*script_provider=*/nullptr);
  if (!factory_) {
    spdlog::error(
        "MaterialLibrary::Initialize: failed to build normalmapped "
        "material factory");
    return false;
  }

  // Deferred terrain-blend factory: same G-buffer targets, kTerrainBlend
  // geometry, and the fs_gbuffer entry (blends the texture_2d_array layers into
  // one albedo). The array view is supplied per-instance by TerrainBlend().
  FactoryDescriptor terrain_desc;
  terrain_desc.shader_name = "terrain_blend";
  terrain_desc.shader_path = "material/terrain_blend.wesl";
  terrain_desc.fs_entry = "fs_gbuffer";
  terrain_desc.supported_pass_types = {MaterialPassType::kDeferred};
  terrain_desc.supported_geometry_types = {GeometryType::kTerrainBlend};
  terrain_desc.color_formats = {GBuffer::kNormalsFormat, GBuffer::kAlbedoFormat,
                                GBuffer::kMaterialFormat};
  terrain_desc.depth_format = GBuffer::kDepthFormat;
  terrain_factory_ = BuildMaterialInstanceFactory(
      terrain_desc, device, queue, pipeline_gen, /*script_provider=*/nullptr);
  if (!terrain_factory_) {
    spdlog::error(
        "MaterialLibrary::Initialize: failed to build terrain_blend "
        "material factory");
    return false;
  }

  // Shared trilinear + anisotropic sampler: the material factory's default
  // sampler uses mipmapFilter=Nearest, which would defeat every pack's GPU
  // mip chain.
  wgpu::SamplerDescriptor samp_desc = {};
  samp_desc.minFilter = wgpu::FilterMode::Linear;
  samp_desc.magFilter = wgpu::FilterMode::Linear;
  samp_desc.mipmapFilter = wgpu::MipmapFilterMode::Linear;
  samp_desc.addressModeU = wgpu::AddressMode::Repeat;
  samp_desc.addressModeV = wgpu::AddressMode::Repeat;
  samp_desc.maxAnisotropy = 16;
  sampler_ = device.CreateSampler(&samp_desc);
  return true;
}

DeferredMaterial MaterialLibrary::SolidColor(glm::vec3 rgb, float roughness) {
  auto to_byte = [](float c) {
    return static_cast<uint8_t>(
        std::lround(std::clamp(c, 0.0f, 1.0f) * 255.0f));
  };
  const uint8_t r = to_byte(rgb.r);
  const uint8_t g = to_byte(rgb.g);
  const uint8_t b = to_byte(rgb.b);
  const uint8_t rough = to_byte(roughness);
  const uint32_t key = (static_cast<uint32_t>(r) << 24) |
                       (static_cast<uint32_t>(g) << 16) |
                       (static_cast<uint32_t>(b) << 8) |
                       static_cast<uint32_t>(rough);

  auto it = solid_cache_.find(key);
  if (it == solid_cache_.end()) {
    InstanceParams params;
    params.texture_overrides.push_back(DefaultTextureView{
        .param_name = "albedo",
        .view = CreateSolidColorTexture(device_, queue_, r, g, b, 255),
        .sampler = sampler_,
        .type = TextureType::k2D,
    });
    // 1x1 ARM: R=255 (AO=1, no occlusion), G=roughness*255, B=0 (non-metal).
    params.texture_overrides.push_back(DefaultTextureView{
        .param_name = "arm",
        .view = CreateSolidColorTexture(device_, queue_, 255, rough, 0, 255),
        .sampler = sampler_,
        .type = TextureType::k2D,
    });
    it = solid_cache_.emplace(key, std::move(params)).first;
  }

  return DeferredMaterial{.factory = factory_.get(), .params = it->second};
}

DeferredMaterial MaterialLibrary::CheckerAlbedo(glm::vec3 color_a,
                                                glm::vec3 color_b, int tiles,
                                                int texels, float roughness) {
  auto pack = [](glm::vec3 c) {
    auto to_byte = [](float v) {
      return static_cast<uint32_t>(std::lround(std::clamp(v, 0.0f, 1.0f) * 255.0f));
    };
    return (to_byte(c.r) << 16) | (to_byte(c.g) << 8) | to_byte(c.b);
  };
  const uint8_t rough = static_cast<uint8_t>(
      std::lround(std::clamp(roughness, 0.0f, 1.0f) * 255.0f));
  const auto key =
      std::make_tuple(pack(color_a), pack(color_b), tiles, texels, rough);

  auto it = checker_cache_.find(key);
  if (it == checker_cache_.end()) {
    const std::vector<uint8_t> pixels =
        BuildCheckerboardRgba8(color_a, color_b, tiles, texels);
    // The view keeps its texture alive (same contract as CreateSolidColorTexture),
    // so we keep only the view -- the LoadedTexture wrapper can drop here.
    const LoadedTexture albedo = UploadTexture2DWithMips(
        device_, queue_, *pipeline_gen_, static_cast<uint32_t>(texels),
        static_cast<uint32_t>(texels), pixels.data());

    InstanceParams params;
    params.texture_overrides.push_back(DefaultTextureView{
        .param_name = "albedo",
        .view = albedo.view,
        .sampler = sampler_,
        .type = TextureType::k2D,
    });
    // 1x1 ARM: R=255 (AO=1), G=roughness*255, B=0 (non-metal) -- as SolidColor.
    params.texture_overrides.push_back(DefaultTextureView{
        .param_name = "arm",
        .view = CreateSolidColorTexture(device_, queue_, 255, rough, 0, 255),
        .sampler = sampler_,
        .type = TextureType::k2D,
    });
    it = checker_cache_.emplace(key, std::move(params)).first;
  }

  return DeferredMaterial{.factory = factory_.get(), .params = it->second};
}

DeferredMaterial MaterialLibrary::AlphaCutout(wgpu::TextureView albedo,
                                              wgpu::Sampler sampler,
                                              float cutoff, glm::vec3 tint) {
  // Build the shared forward-opaque "leaf" factory once. Its color target is
  // the HDR accumulation format (the forward-opaque pass renders into
  // hdr_color_view_) and its depth is the reversed-Z scene depth; cull None
  // (double-sided foliage) and depth_write true so leaf cards occlude each
  // other. The kForwardOpaque variant compiles both the forward and the
  // (alpha-tested, depth-only) shadow pipeline.
  if (!alpha_cutout_factory_) {
    FactoryDescriptor desc;
    desc.shader_name = "leaf";
    desc.shader_path = "material/leaf.wesl";
    desc.supported_pass_types = {MaterialPassType::kForwardOpaque};
    desc.supported_geometry_types = {GeometryType::kTexturedMesh};
    desc.color_formats = {SceneRenderer::kAccumulationFormat};  // HDR
    desc.depth_format = SceneRenderer::kDepthFormat;
    desc.depth_write = true;
    desc.cull_mode = wgpu::CullMode::None;  // double-sided
    alpha_cutout_factory_ = BuildMaterialInstanceFactory(
        desc, device_, queue_, pipeline_gen_, /*script_provider=*/nullptr);
    if (!alpha_cutout_factory_) {
      spdlog::error(
          "MaterialLibrary::AlphaCutout: failed to build leaf material factory");
      load_failed_ = true;
      return DeferredMaterial{};
    }
  }

  InstanceParams params;
  params.texture_overrides.push_back(DefaultTextureView{
      .param_name = "albedo",
      .view = albedo,
      .sampler = sampler,
      .type = TextureType::k2D,
  });
  // tintCutoff: xyz = tint (RGB multiplier), w = alpha discard threshold. Names
  // the group-1 LeafUniforms field the render_forward pass applies per-object.
  params.uniform_overrides = {
      {"tintCutoff", glm::vec4(tint, cutoff)},
  };

  return DeferredMaterial{.factory = alpha_cutout_factory_.get(),
                          .params = std::move(params)};
}

MaterialLibrary::TerrainArrays MaterialLibrary::LoadTerrainArrays(
    const std::vector<std::string>& pack_dirs) {
  TerrainArrays result;
  if (pack_dirs.empty()) {
    spdlog::error("MaterialLibrary::LoadTerrainArrays: no packs");
    load_failed_ = true;
    return result;
  }

  // Load every pack first (manifest-driven; mips + DX->GL green flip handled by
  // LoadPack/LoadTexture2D), then transpose pack-major -> channel-major.
  //
  // Deliberately NOT via cache_: the per-pack 2D textures are needed only long
  // enough to copy into the arrays, so holding them would double terrain VRAM
  // (~84 MB of sources shadowing ~84 MB of arrays). `packs` drops them on
  // return -- safe right after Submit, because Dawn keeps its own reference to
  // resources used by a submitted command buffer until it retires (nothing here
  // calls Destroy()). Bypassing the cache also means there is no shared entry
  // that a previously-failed Get() could have poisoned with null textures.
  std::vector<PackTextures> packs;
  packs.reserve(pack_dirs.size());
  for (const std::string& dir : pack_dirs) {
    PackTextures textures = LoadPack(dir);
    if (!textures.albedo.texture || !textures.normal.texture ||
        !textures.arm.texture) {
      // LoadPack already logged + set load_failed_.
      return {};
    }
    packs.push_back(std::move(textures));
  }

  std::vector<wgpu::Texture> albedo_layers, normal_layers, arm_layers;
  albedo_layers.reserve(packs.size());
  normal_layers.reserve(packs.size());
  arm_layers.reserve(packs.size());
  for (const PackTextures& p : packs) {
    albedo_layers.push_back(p.albedo.texture);
    normal_layers.push_back(p.normal.texture);
    arm_layers.push_back(p.arm.texture);
  }

  result.albedo = PackTexturesIntoArray(device_, queue_, albedo_layers);
  result.normal = PackTexturesIntoArray(device_, queue_, normal_layers);
  result.arm = PackTexturesIntoArray(device_, queue_, arm_layers);

  if (!result.albedo.view || !result.normal.view || !result.arm.view) {
    spdlog::error(
        "MaterialLibrary::LoadTerrainArrays: failed to pack {} packs into "
        "arrays (do all packs share one texture size?)",
        pack_dirs.size());
    load_failed_ = true;
    return {};
  }
  spdlog::info("MaterialLibrary: terrain arrays built from {} packs",
               pack_dirs.size());
  return result;
}

MaterialLibrary::TerrainArrays MaterialLibrary::DebugTerrainArrays(
    const std::vector<glm::vec3>& srgb_colors) {
  TerrainArrays result;
  const uint32_t n = static_cast<uint32_t>(srgb_colors.size());
  if (n == 0) {
    spdlog::error("MaterialLibrary::DebugTerrainArrays: no colors");
    load_failed_ = true;
    return result;
  }
  auto to_byte = [](float c) {
    return static_cast<uint8_t>(std::lround(std::clamp(c, 0.0f, 1.0f) * 255.0f));
  };
  // One RGBA per layer, per channel array. Normal = flat GL-convention (0,0,1)
  // -> (128,128,255). ARM = AO 1 / roughness 1 (matte) / metallic 0.
  std::vector<uint8_t> albedo(n * 4), normal(n * 4), arm(n * 4);
  for (uint32_t i = 0; i < n; ++i) {
    albedo[i * 4 + 0] = to_byte(srgb_colors[i].r);
    albedo[i * 4 + 1] = to_byte(srgb_colors[i].g);
    albedo[i * 4 + 2] = to_byte(srgb_colors[i].b);
    albedo[i * 4 + 3] = 255;
    normal[i * 4 + 0] = 128; normal[i * 4 + 1] = 128;
    normal[i * 4 + 2] = 255; normal[i * 4 + 3] = 255;
    arm[i * 4 + 0] = 255; arm[i * 4 + 1] = 255;
    arm[i * 4 + 2] = 0;   arm[i * 4 + 3] = 255;
  }
  result.albedo.view = CreateSolidColorArray(device_, queue_, albedo.data(), n);
  result.normal.view = CreateSolidColorArray(device_, queue_, normal.data(), n);
  result.arm.view = CreateSolidColorArray(device_, queue_, arm.data(), n);
  if (!result.albedo.view || !result.normal.view || !result.arm.view) {
    spdlog::error("MaterialLibrary::DebugTerrainArrays: failed to build arrays");
    load_failed_ = true;
    return {};
  }
  return result;
}

DeferredMaterial MaterialLibrary::TerrainBlend(const TerrainArrays& arrays) {
  InstanceParams params;
  // Matched by param_name against the terrain_blend slots; TextureType is only
  // used to filter default-view recipes, so k2D is fine for an override.
  // sampler_ is the shared trilinear + 16x-aniso one -- a bare sampler
  // (mipmapFilter=Nearest) would defeat the arrays' mip chains.
  // A null view is left UNBOUND so the factory resolves that slot's e2DArray
  // default (flat_normal / default_arm) -- binding a null override would
  // instead fail bind-group validation.
  auto bind = [&](const char* slot, wgpu::TextureView view) {
    if (!view) return;
    params.texture_overrides.push_back(DefaultTextureView{
        .param_name = slot,
        .view = view,
        .sampler = sampler_,
        .type = TextureType::k2D,
    });
  };
  bind("albedo_array", arrays.albedo.view);
  bind("normal_array", arrays.normal.view);
  bind("arm_array", arrays.arm.view);

  return DeferredMaterial{.factory = terrain_factory_.get(),
                          .params = std::move(params)};
}

MaterialLibrary::PackTextures MaterialLibrary::LoadPack(
    const std::string& dir) const {
  PackTextures result;

  const std::string manifest_path = dir + "/material.json";
  std::ifstream manifest_file(manifest_path);
  if (!manifest_file) {
    spdlog::error("MaterialLibrary: failed to resolve pack textures -- "
                  "missing manifest '{}'",
                  manifest_path);
    load_failed_ = true;
    return result;
  }

  nlohmann::json manifest;
  std::string albedo_rel, normal_rel, arm_rel, normal_format;
  try {
    manifest_file >> manifest;
    albedo_rel = manifest.at("albedo").get<std::string>();
    normal_rel = manifest.at("normal").get<std::string>();
    arm_rel = manifest.at("arm").get<std::string>();
    normal_format = manifest.value("normal_format", std::string("dx"));
  } catch (const nlohmann::json::exception& e) {
    spdlog::error(
        "MaterialLibrary: failed to resolve pack textures -- unparseable "
        "manifest '{}': {}",
        manifest_path, e.what());
    load_failed_ = true;
    return result;
  }

  // normal_format now drives loader behavior directly (see LoadTexture2D's
  // flip_green_dx param): DirectX-convention packs get their green channel
  // CPU-flipped at load, so every pack's uploaded normal map is GL-convention
  // regardless of source convention -- no footgun, just an info log.
  const bool is_dx_normal = (normal_format == "dx");
  if (!is_dx_normal && normal_format != "gl") {
    spdlog::info(
        "MaterialLibrary: pack '{}' declares normal_format='{}' (expected "
        "'dx' or 'gl') -- treating as already GL-convention (no flip)",
        dir, normal_format);
  }

  const std::string albedo_path = dir + "/" + albedo_rel;
  const std::string normal_path = dir + "/" + normal_rel;
  const std::string arm_path = dir + "/" + arm_rel;

  result.albedo = LoadTexture2D(device_, queue_, *pipeline_gen_, albedo_path);
  result.normal = LoadTexture2D(device_, queue_, *pipeline_gen_, normal_path,
                                /*flip_green_dx=*/is_dx_normal);
  result.arm = LoadTexture2D(device_, queue_, *pipeline_gen_, arm_path);

  if (!result.albedo.texture || !result.normal.texture ||
      !result.arm.texture) {
    spdlog::error("MaterialLibrary: pack '{}' loaded with missing texture(s)",
                  dir);
    load_failed_ = true;
    return result;
  }

  spdlog::info(
      "MaterialLibrary: loaded pack '{}' -- albedo {}x{} ({} mips), "
      "normal {}x{} ({} mips), arm {}x{} ({} mips)",
      dir, result.albedo.texture.GetWidth(),
      result.albedo.texture.GetHeight(),
      result.albedo.texture.GetMipLevelCount(),
      result.normal.texture.GetWidth(), result.normal.texture.GetHeight(),
      result.normal.texture.GetMipLevelCount(),
      result.arm.texture.GetWidth(),
      result.arm.texture.GetHeight(),
      result.arm.texture.GetMipLevelCount());

  return result;
}

DeferredMaterial MaterialLibrary::Get(const std::string& dir) {
  auto params_it = params_cache_.find(dir);
  if (params_it != params_cache_.end()) {
    return DeferredMaterial{.factory = factory_.get(),
                            .params = params_it->second};
  }

  auto it = cache_.find(dir);
  if (it == cache_.end()) {
    it = cache_.emplace(dir, LoadPack(dir)).first;
  }
  const PackTextures& pack = it->second;

  InstanceParams params;
  params.texture_overrides.push_back(DefaultTextureView{
      .param_name = "albedo",
      .view = pack.albedo.view,
      .sampler = sampler_,
      .type = TextureType::k2D,
  });
  params.texture_overrides.push_back(DefaultTextureView{
      .param_name = "normal",
      .view = pack.normal.view,
      .sampler = sampler_,
      .type = TextureType::k2D,
  });
  params.texture_overrides.push_back(DefaultTextureView{
      .param_name = "arm",
      .view = pack.arm.view,
      .sampler = sampler_,
      .type = TextureType::k2D,
  });

  params_it = params_cache_.emplace(dir, std::move(params)).first;
  return DeferredMaterial{.factory = factory_.get(), .params = params_it->second};
}

}  // namespace badlands
