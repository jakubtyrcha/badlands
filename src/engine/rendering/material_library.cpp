#include "engine/rendering/material_library.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <utility>

#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

#include "core/geometry_type.hpp"
#include "engine/rendering/gbuffer.hpp"

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

MaterialLibrary::PackTextures MaterialLibrary::LoadPack(
    const std::string& dir) const {
  PackTextures result;

  const std::string manifest_path = dir + "/material.json";
  std::ifstream manifest_file(manifest_path);
  if (!manifest_file) {
    spdlog::error("MaterialLibrary: failed to resolve pack textures -- "
                  "missing manifest '{}'",
                  manifest_path);
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
    return result;
  }

  // normalmapped.wesl hardcodes an unconditional DirectX->GL green-channel
  // flip; it has no uniform to toggle this. Packs whose normal maps are not
  // DirectX-convention will render with inverted normal-map Y and need
  // either a re-export or a shader-side toggle -- flag it loudly rather than
  // silently shading wrong.
  if (normal_format != "dx") {
    spdlog::warn(
        "MaterialLibrary: pack '{}' declares normal_format='{}', but "
        "normalmapped.wesl assumes DirectX-convention normals ('dx') and "
        "always flips green -- this pack's normal maps will render "
        "inverted",
        dir, normal_format);
  }

  const std::string albedo_path = dir + "/" + albedo_rel;
  const std::string normal_path = dir + "/" + normal_rel;
  const std::string arm_path = dir + "/" + arm_rel;

  result.albedo = LoadTexture2D(device_, queue_, *pipeline_gen_, albedo_path);
  result.normal = LoadTexture2D(device_, queue_, *pipeline_gen_, normal_path);
  result.arm = LoadTexture2D(device_, queue_, *pipeline_gen_, arm_path);

  if (!result.albedo.texture || !result.normal.texture ||
      !result.arm.texture) {
    spdlog::error("MaterialLibrary: pack '{}' loaded with missing texture(s)",
                  dir);
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

  return DeferredMaterial{.factory = factory_.get(), .params = std::move(params)};
}

}  // namespace badlands
