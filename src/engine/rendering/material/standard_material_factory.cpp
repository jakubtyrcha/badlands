// Ported (reconciled) from sampo's
// src/rendering/material/standard_material_factory.cpp, namespace sampo ->
// badlands.
//
// KEY reconciliation: sampo's `BuildMaterialInstanceFactory` built one
// `MeshRenderingMaterial` per `MaterialPassType` variant and *eagerly*
// registered pipelines for every (geometry x pass) combination via the
// async `GpuPipelineGenerator::RegisterMaterialPipelines` (taskflow-backed;
// not ported — see gpu_pipeline_generator.hpp). This port keeps the same
// per-variant `MeshRenderingMaterial` structure (same base name/suffix,
// blend/feature config per `MaterialPassType`) but does NOT eagerly compile
// anything: each `MeshRenderingMaterial` is constructed with its shader
// path/entries/features/blend config and a `TargetConfig` per
// `RenderPassType`, and compilation happens lazily, on demand, the first
// time `CreateInstance` (via `MeshRenderingMaterial::GetPipeline`) is called
// for a given (geometry, pass) — see material.hpp/material.cpp.
//
// `RenderStateProvider`/`RenderStateId` (sampo's indirection for resolving
// target formats + blend state per pass) aren't ported; `FactoryDescriptor`
// now carries `color_formats`/`depth_format` directly, and blend state is a
// fixed per-`MaterialPassType` constant (`kForwardTransparent` blends,
// premultiplied; the others don't) — see `kVariants` below.
#include "engine/rendering/material/standard_material_factory.hpp"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <array>

#include "engine/rendering/material/script_texture_provider.hpp"
#include "engine/rendering/material/standard_rendering_material_instance.hpp"
#include "engine/rendering/shader/gpu_pipeline_generator.hpp"

namespace badlands {

namespace {

wgpu::Sampler CreateNearestSampler(wgpu::Device device) {
  wgpu::SamplerDescriptor desc{};
  desc.magFilter = wgpu::FilterMode::Nearest;
  desc.minFilter = wgpu::FilterMode::Nearest;
  desc.mipmapFilter = wgpu::MipmapFilterMode::Nearest;
  desc.addressModeU = wgpu::AddressMode::ClampToEdge;
  desc.addressModeV = wgpu::AddressMode::ClampToEdge;
  desc.addressModeW = wgpu::AddressMode::ClampToEdge;
  desc.maxAnisotropy = 1;
  return device.CreateSampler(&desc);
}

wgpu::TextureView CreateSolidColor1x1(wgpu::Device device, wgpu::Queue queue,
                                       uint8_t r, uint8_t g, uint8_t b,
                                       uint8_t a) {
  wgpu::TextureDescriptor desc{};
  desc.size = {1, 1, 1};
  desc.format = wgpu::TextureFormat::RGBA8Unorm;
  desc.usage = wgpu::TextureUsage::TextureBinding | wgpu::TextureUsage::CopyDst;
  desc.dimension = wgpu::TextureDimension::e2D;
  desc.mipLevelCount = 1;
  desc.sampleCount = 1;

  auto texture = device.CreateTexture(&desc);
  uint8_t data[] = {r, g, b, a};

  wgpu::TexelCopyBufferLayout layout{};
  layout.bytesPerRow = 4;
  layout.rowsPerImage = 1;

  wgpu::TexelCopyTextureInfo dst{};
  dst.texture = texture;
  dst.mipLevel = 0;
  dst.origin = {0, 0, 0};

  wgpu::Extent3D extent = {1, 1, 1};
  queue.WriteTexture(&dst, data, sizeof(data), &layout, &extent);

  return texture.CreateView();
}

wgpu::TextureView CreateSolidColorCubemap1x1(wgpu::Device device,
                                               wgpu::Queue queue, uint8_t r,
                                               uint8_t g, uint8_t b,
                                               uint8_t a) {
  wgpu::TextureDescriptor desc{};
  desc.size = {1, 1, 6};
  desc.format = wgpu::TextureFormat::RGBA8Unorm;
  desc.usage = wgpu::TextureUsage::TextureBinding | wgpu::TextureUsage::CopyDst;
  desc.dimension = wgpu::TextureDimension::e2D;
  desc.mipLevelCount = 1;
  desc.sampleCount = 1;

  auto texture = device.CreateTexture(&desc);
  uint8_t data[] = {r, g, b, a};

  for (uint32_t face = 0; face < 6; ++face) {
    wgpu::TexelCopyBufferLayout layout{};
    layout.bytesPerRow = 4;
    layout.rowsPerImage = 1;

    wgpu::TexelCopyTextureInfo dst{};
    dst.texture = texture;
    dst.mipLevel = 0;
    dst.origin = {0, 0, face};

    wgpu::Extent3D extent = {1, 1, 1};
    queue.WriteTexture(&dst, data, sizeof(data), &layout, &extent);
  }

  wgpu::TextureViewDescriptor view_desc{};
  view_desc.dimension = wgpu::TextureViewDimension::Cube;
  view_desc.arrayLayerCount = 6;
  view_desc.baseArrayLayer = 0;
  view_desc.mipLevelCount = 1;
  view_desc.baseMipLevel = 0;
  view_desc.format = wgpu::TextureFormat::RGBA8Unorm;
  return texture.CreateView(&view_desc);
}

}  // namespace

StandardMaterialFactory::StandardMaterialFactory(
    std::string requirements_name,
    std::map<MaterialPassType, std::unique_ptr<MeshRenderingMaterial>> materials,
    std::vector<DefaultTextureView> default_view_recipes,
    std::vector<NoiserMaterialScript> script_recipes,
    ScriptTextureProvider* script_provider,
    wgpu::Device device, wgpu::Queue queue,
    const MaterialRequirementsRegistry& requirements_registry,
    std::vector<GeometryType> supported_geometry_types)
    : requirements_name_(std::move(requirements_name)),
      materials_(std::move(materials)),
      default_view_recipes_(std::move(default_view_recipes)),
      script_recipes_(std::move(script_recipes)),
      script_provider_(script_provider),
      device_(device),
      queue_(queue),
      requirements_registry_(requirements_registry),
      supported_geometry_types_(std::move(supported_geometry_types)) {
  default_nearest_sampler_ = CreateNearestSampler(device_);
}

std::unique_ptr<RenderingMaterialInstance>
StandardMaterialFactory::CreateInstance(GeometryType geometry_type,
                                        MaterialPassType material_pass,
                                        RenderPassType render_pass,
                                        const InstanceParams& params) {
  if (!supported_geometry_types_.empty() &&
      std::find(supported_geometry_types_.begin(),
                supported_geometry_types_.end(),
                geometry_type) == supported_geometry_types_.end()) {
    return nullptr;
  }

  // Look up material for this pass type
  auto mat_it = materials_.find(material_pass);
  if (mat_it == materials_.end() || !mat_it->second ||
      !mat_it->second->IsValid()) {
    return nullptr;
  }
  auto* material = mat_it->second.get();

  // Check pipeline exists for this geometry/render_pass combination
  // (compiles it on first use — see the file-level reconciliation note).
  auto pipeline = material->GetPipeline(geometry_type, render_pass);
  if (!pipeline) {
    return nullptr;
  }

  // Create a MaterialInstance wrapping the material
  auto instance = std::make_unique<MaterialInstance>(material, geometry_type, render_pass);

  // Get texture requirements for this shader + geometry
  // Use requirements_name_ (derived from shader path) for canonical lookup
  auto requirements =
      requirements_registry_.GetRequirements(requirements_name_,
                                              geometry_type);

  // Auto-derive from shader reflection if not manually registered
  if (requirements.textures.empty()) {
    const auto& reflected = material->GetReflectedBindings(
        geometry_type, render_pass);
    if (!reflected.empty()) {
      requirements = DeriveRequirementsFromReflection(
          requirements_name_, reflected);
    }
  }

  // Determine expected texture type from geometry
  TextureType expected_type = (geometry_type == GeometryType::kSphericalMesh)
                                  ? TextureType::kCubemap
                                  : TextureType::k2D;

  // Lazily resolve scripts for this geometry type
  const auto& resolved_scripts = GetResolvedScripts(geometry_type);

  // Populate textures: instance overrides → (resolved scripts + filtered
  // default views) → factory defaults
  for (const auto& req : requirements.textures) {
    wgpu::TextureView view;
    wgpu::Sampler sampler;

    // 1. Check instance parameter overrides
    auto override_it = std::find_if(
        params.texture_overrides.begin(), params.texture_overrides.end(),
        [&](const DefaultTextureView& dtv) {
          return dtv.param_name == req.slot_name;
        });

    if (override_it != params.texture_overrides.end()) {
      view = override_it->view;
      sampler = override_it->sampler;
    }

    // 2. Check resolved scripts
    if (!view) {
      auto script_it = std::find_if(
          resolved_scripts.begin(), resolved_scripts.end(),
          [&](const ResolvedRecipeTexture& r) {
            return r.param_name == req.slot_name;
          });

      if (script_it != resolved_scripts.end()) {
        view = script_it->view;
        sampler = script_it->sampler;
      }
    }

    // 3. Check default view recipes (filtered by expected texture type)
    if (!view) {
      auto view_it = std::find_if(
          default_view_recipes_.begin(), default_view_recipes_.end(),
          [&](const DefaultTextureView& dtv) {
            return dtv.param_name == req.slot_name &&
                   dtv.type == expected_type;
          });

      if (view_it != default_view_recipes_.end()) {
        view = view_it->view;
        sampler = view_it->sampler;
      }
    }

    // 4. Fall back to factory defaults (2D or cubemap based on geometry)
    if (!view) {
      view = GetDefaultTextureForSlot(req.default_texture, expected_type);
      sampler = default_nearest_sampler_;
    }

    if (view && sampler) {
      instance->SetTexture(0, req.texture_binding, req.sampler_binding, view,
                           sampler);
    }
  }

  // Apply uniform overrides from params
  for (const auto& [name, value] : params.uniform_overrides) {
    std::visit(
        [&](auto&& v) {
          using T = std::decay_t<decltype(v)>;
          if constexpr (std::is_same_v<T, int32_t>) {
            instance->SetInt(name, v);
          } else if constexpr (std::is_same_v<T, uint32_t>) {
            instance->SetUInt(name, v);
          } else if constexpr (std::is_same_v<T, float>) {
            instance->SetFloat(name, v);
          } else if constexpr (std::is_same_v<T, glm::vec2>) {
            instance->SetVec2(name, v);
          } else if constexpr (std::is_same_v<T, glm::vec3>) {
            instance->SetVec3(name, v);
          } else if constexpr (std::is_same_v<T, glm::vec4>) {
            instance->SetVec4(name, v);
          } else if constexpr (std::is_same_v<T, glm::mat4>) {
            instance->SetMat4(name, v);
          }
        },
        value);
  }

  return std::make_unique<StandardRenderingMaterialInstance>(
      material, std::move(instance), geometry_type, render_pass);
}

StandardMaterialFactory::DefaultTextures&
StandardMaterialFactory::GetDefaultTextures() {
  if (!default_textures_) {
    default_textures_ = DefaultTextures{
        .white = CreateSolidColor1x1(device_, queue_, 255, 255, 255, 255),
        .flat_normal = CreateSolidColor1x1(device_, queue_, 128, 128, 255, 255),
        .full_roughness =
            CreateSolidColor1x1(device_, queue_, 255, 255, 255, 255),
        .gray = CreateSolidColor1x1(device_, queue_, 128, 128, 128, 255),
        .cube_white =
            CreateSolidColorCubemap1x1(device_, queue_, 255, 255, 255, 255),
        .cube_flat_normal =
            CreateSolidColorCubemap1x1(device_, queue_, 128, 128, 255, 255),
        .cube_full_roughness =
            CreateSolidColorCubemap1x1(device_, queue_, 255, 255, 255, 255),
        .cube_gray =
            CreateSolidColorCubemap1x1(device_, queue_, 128, 128, 128, 255),
        .sampler = default_nearest_sampler_,
    };
  }
  return *default_textures_;
}

wgpu::TextureView StandardMaterialFactory::GetDefaultTextureForSlot(
    const std::string& default_name, TextureType type) {
  auto& defaults = GetDefaultTextures();
  if (type == TextureType::kCubemap) {
    if (default_name == "white") return defaults.cube_white;
    if (default_name == "flat_normal") return defaults.cube_flat_normal;
    if (default_name == "full_roughness") return defaults.cube_full_roughness;
    if (default_name == "gray") return defaults.cube_gray;
    return defaults.cube_white;
  }
  if (default_name == "white") return defaults.white;
  if (default_name == "flat_normal") return defaults.flat_normal;
  if (default_name == "full_roughness") return defaults.full_roughness;
  if (default_name == "gray") return defaults.gray;
  return defaults.white;
}

const std::vector<ResolvedRecipeTexture>&
StandardMaterialFactory::GetResolvedScripts(GeometryType geometry_type) const {
  static const std::vector<ResolvedRecipeTexture> empty;
  if (script_recipes_.empty()) {
    return empty;
  }

  auto it = resolved_script_cache_.find(geometry_type);
  if (it != resolved_script_cache_.end()) {
    return it->second;
  }

  // Resolve all scripts for this geometry type
  bool is_cubemap = (geometry_type == GeometryType::kSphericalMesh);
  std::vector<ResolvedRecipeTexture> resolved;

  if (script_provider_) {
    for (const auto& script : script_recipes_) {
      auto result = script_provider_->ExecuteScriptToTexture(
          script.source, script.params, script.resolution, is_cubemap);
      if (result) {
        resolved.push_back(ResolvedRecipeTexture{
            .param_name = script.param_name,
            .view = result->view,
            .sampler = result->sampler,
        });
      } else {
        spdlog::warn(
            "StandardMaterialFactory::GetResolvedScripts: failed to execute "
            "script for '{}': {}",
            script.param_name, result.error());
      }
    }
  }

  auto [ins_it, _] =
      resolved_script_cache_.emplace(geometry_type, std::move(resolved));
  return ins_it->second;
}

// === BuildMaterialInstanceFactory implementation ===

namespace {

struct VariantConfig {
  MaterialPassType pass_type;
  std::string suffix;  // appended to base shader_name (debug name only)
  std::vector<RenderPassType> render_passes;
  bool blend_enabled;
  bool premultiplied_alpha;
  std::vector<std::string> extra_features;
};

const std::array<VariantConfig, 3> kVariants = {{
    {MaterialPassType::kDeferred, "",
     {RenderPassType::kGBuffer, RenderPassType::kShadow}, false, false, {}},
    {MaterialPassType::kForwardOpaque, "_fwd_opaque",
     {RenderPassType::kForward, RenderPassType::kShadow}, false, false, {}},
    {MaterialPassType::kForwardTransparent, "_fwd_transparent",
     {RenderPassType::kForward}, true, true, {"transparent"}},
}};

}  // namespace

std::unique_ptr<MaterialInstanceFactory> BuildMaterialInstanceFactory(
    const FactoryDescriptor& desc, wgpu::Device device, wgpu::Queue queue,
    GpuPipelineGenerator* shader_context,
    ScriptTextureProvider* script_provider) {
  if (!shader_context) {
    spdlog::error("BuildMaterialInstanceFactory: null shader_context");
    return nullptr;
  }

  // Check for script recipes without provider
  for (const auto& recipe : desc.recipes) {
    if (std::holds_alternative<NoiserMaterialScript>(recipe) &&
        !script_provider) {
      spdlog::error(
          "BuildMaterialInstanceFactory: NoiserMaterialScript recipe requires "
          "script_provider");
      return nullptr;
    }
  }

  // Register (lazily-compiled) materials for supported MaterialPassType variants
  std::map<MaterialPassType, std::unique_ptr<MeshRenderingMaterial>> materials;

  for (const auto& variant : kVariants) {
    if (!desc.supported_pass_types.empty() &&
        std::find(desc.supported_pass_types.begin(),
                  desc.supported_pass_types.end(),
                  variant.pass_type) == desc.supported_pass_types.end()) {
      continue;
    }
    std::string name = desc.shader_name + variant.suffix;

    std::map<RenderPassType, MeshRenderingMaterial::TargetConfig> pass_targets;
    for (RenderPassType pass : variant.render_passes) {
      MeshRenderingMaterial::TargetConfig target;
      // Shadow passes are depth-only (no color attachments); see the
      // deviation note at the top of this file re: FactoryDescriptor
      // supplying formats directly instead of a RenderStateId.
      target.color_formats =
          (pass == RenderPassType::kShadow) ? RenderTargetFormats{} : desc.color_formats;
      target.depth_format = desc.depth_format;
      // Placeholder: depth_write/depth_compare are hardcoded uniformly here
      // rather than derived per-pass the way sampo's RenderState presets did
      // (e.g. a forward-transparent pass testing but not writing depth).
      // Revisit once a real RenderState port lands.
      //
      // depth_compare DOES vary by pass: SceneRenderer's kForward/kGBuffer
      // depth attachment uses reversed-Z (cleared to 0.0 = far, 1.0 = near —
      // see scene_renderer.cpp's depthClearValue), so fragments must pass
      // when their NDC z is >= the stored value. kShadow, if/when a shadow
      // pass lands, keeps the conventional (non-reversed, cleared to 1.0)
      // Less test sampo used.
      target.depth_write = true;
      target.depth_compare = (pass == RenderPassType::kShadow)
                                  ? wgpu::CompareFunction::Less
                                  : wgpu::CompareFunction::GreaterEqual;
      pass_targets.emplace(pass, std::move(target));
    }

    materials.emplace(
        variant.pass_type,
        std::make_unique<MeshRenderingMaterial>(
            shader_context, std::move(name), desc.shader_path, desc.vs_entry,
            desc.fs_entry, variant.extra_features, variant.blend_enabled,
            variant.premultiplied_alpha, std::move(pass_targets)));
  }

  // Split recipes into default views and unresolved scripts
  std::vector<DefaultTextureView> default_view_recipes;
  std::vector<NoiserMaterialScript> script_recipes;
  for (const auto& recipe : desc.recipes) {
    std::visit(
        [&](auto&& item) {
          using T = std::decay_t<decltype(item)>;
          if constexpr (std::is_same_v<T, DefaultTextureView>) {
            default_view_recipes.push_back(item);
          } else if constexpr (std::is_same_v<T, NoiserMaterialScript>) {
            script_recipes.push_back(item);
          }
        },
        recipe);
  }

  // Derive canonical requirements name from shader path stem
  // e.g., "material/textured_mesh.wesl" → "textured_mesh"
  std::string requirements_name = desc.shader_path;
  if (auto slash = requirements_name.rfind('/'); slash != std::string::npos) {
    requirements_name = requirements_name.substr(slash + 1);
  }
  if (auto dot = requirements_name.rfind('.'); dot != std::string::npos) {
    requirements_name = requirements_name.substr(0, dot);
  }

  // Built-in registry is immutable after construction — thread-safe static init
  static const MaterialRequirementsRegistry s_registry;

  return std::make_unique<StandardMaterialFactory>(
      std::move(requirements_name), std::move(materials),
      std::move(default_view_recipes), std::move(script_recipes),
      script_provider, device, queue, s_registry,
      desc.supported_geometry_types);
}

}  // namespace badlands
