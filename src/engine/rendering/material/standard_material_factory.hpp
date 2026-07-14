#pragma once

// Ported from sampo's src/rendering/material/standard_material_factory.hpp,
// namespace sampo -> badlands.
//
// Deviation: adds `supported_geometry_types_` (from
// `FactoryDescriptor::supported_geometry_types`), checked in `CreateInstance`.
// sampo enforced this at async-registration time (only registering pipelines
// for the configured geometry types); since this port compiles lazily on
// first `CreateInstance` call (see material.hpp), the check has to happen in
// `CreateInstance` itself instead.
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include <dawn/webgpu_cpp.h>

#include "engine/rendering/material/material.hpp"
#include "engine/rendering/material/material_instance_factory.hpp"
#include "engine/rendering/material/material_requirements.hpp"

namespace badlands {

class MeshRenderingMaterial;
class ScriptTextureProvider;

// Resolved texture from recipe (either DefaultTextureView or script execution)
struct ResolvedRecipeTexture {
  std::string param_name;
  wgpu::TextureView view;
  wgpu::Sampler sampler;
};

// StandardMaterialFactory is the ownership and caching layer for material resources:
// - Compiled shader pipelines (via MeshRenderingMaterial → GpuPipelineGenerator)
// - Default textures and samplers (1x1 fallback textures, nearest sampler)
// - Resolved script textures (lazily cached per geometry type in CreateInstance)
// - Recipe data: DefaultTextureView items (geometry-specific via TextureType)
//   and NoiserMaterialScript items (geometry-independent; resolved on demand)
//
// Geometry type determines texture format: kSphericalMesh → cubemap, kTexturedMesh → 2D.
// Scripts are geometry-agnostic in definition but produce geometry-specific textures.
class StandardMaterialFactory : public MaterialInstanceFactory {
 public:
  StandardMaterialFactory(
      std::string requirements_name,
      std::map<MaterialPassType, std::unique_ptr<MeshRenderingMaterial>> materials,
      std::vector<DefaultTextureView> default_view_recipes,
      std::vector<NoiserMaterialScript> script_recipes,
      ScriptTextureProvider* script_provider,
      wgpu::Device device, wgpu::Queue queue,
      const MaterialRequirementsRegistry& requirements_registry,
      std::vector<GeometryType> supported_geometry_types = {});

  std::unique_ptr<RenderingMaterialInstance> CreateInstance(
      GeometryType geometry_type, MaterialPassType material_pass,
      RenderPassType render_pass,
      const InstanceParams& params = {}) override;

 private:
  std::string requirements_name_;
  std::map<MaterialPassType, std::unique_ptr<MeshRenderingMaterial>> materials_;
  std::vector<DefaultTextureView> default_view_recipes_;
  std::vector<NoiserMaterialScript> script_recipes_;
  ScriptTextureProvider* script_provider_;
  wgpu::Device device_;
  wgpu::Queue queue_;
  const MaterialRequirementsRegistry& requirements_registry_;
  std::vector<GeometryType> supported_geometry_types_;
  mutable std::map<GeometryType, std::vector<ResolvedRecipeTexture>> resolved_script_cache_;

  // Default samplers (Nearest for 1x1 fallback textures)
  wgpu::Sampler default_nearest_sampler_;

  // Lazily created default 1x1 textures per slot type
  struct DefaultTextures {
    wgpu::TextureView white;
    wgpu::TextureView flat_normal;
    wgpu::TextureView full_roughness;
    wgpu::TextureView gray;
    // Cubemap variants for spherical geometry
    wgpu::TextureView cube_white;
    wgpu::TextureView cube_flat_normal;
    wgpu::TextureView cube_full_roughness;
    wgpu::TextureView cube_gray;
    wgpu::Sampler sampler;
  };
  std::optional<DefaultTextures> default_textures_;

  DefaultTextures& GetDefaultTextures();
  wgpu::TextureView GetDefaultTextureForSlot(const std::string& default_name,
                                              TextureType type);
  const std::vector<ResolvedRecipeTexture>& GetResolvedScripts(
      GeometryType geometry_type) const;
};

}  // namespace badlands
