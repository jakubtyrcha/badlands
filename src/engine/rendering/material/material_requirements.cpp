// Ported from sampo's src/rendering/material/material_requirements.cpp,
// namespace sampo -> badlands (verbatim otherwise).
#include "engine/rendering/material/material_requirements.hpp"

#include <algorithm>
#include <sstream>

#include "engine/rendering/shader/shader_reflection.hpp"

namespace badlands {

std::string MaterialRequirementsRegistry::MakeKey(const std::string& name,
                                                   GeometryType geo) {
  return name + (geo == GeometryType::kSphericalMesh ? "_sphere" : "_flat");
}

MaterialRequirementsRegistry::MaterialRequirementsRegistry() {
  // Register built-in materials

  // normalmapped.wesl - PBR material with albedo, normal, ARM (AO=R /
  // roughness=G / metallic=B) textures
  RegisterMaterial(
      "normalmapped",
      // textured_mesh mode: 2D textures
      MaterialRequirements{
          .shader_name = "normalmapped",
          .textures = {
              {.slot_name = "albedo",
               .texture_binding = 1,
               .sampler_binding = 2,
               .default_texture = "white"},
              {.slot_name = "normal",
               .texture_binding = 3,
               .sampler_binding = 2,
               .default_texture = "flat_normal"},
              {.slot_name = "arm",
               .texture_binding = 4,
               .sampler_binding = 2,
               .default_texture = "white"},
          }},
      // spherical_mesh mode: cubemap textures
      MaterialRequirements{
          .shader_name = "normalmapped",
          .textures = {
              {.slot_name = "albedo",
               .texture_binding = 1,
               .sampler_binding = 2,
               .default_texture = "white"},
              {.slot_name = "normal",
               .texture_binding = 3,
               .sampler_binding = 2,
               .default_texture = "flat_normal"},
              {.slot_name = "arm",
               .texture_binding = 4,
               .sampler_binding = 2,
               .default_texture = "white"},
          }});

  // textured_mesh.wesl - Simple lit textured mesh
  RegisterMaterial(
      "textured_mesh",
      // textured_mesh mode: single texture
      MaterialRequirements{
          .shader_name = "textured_mesh",
          .textures = {
              {.slot_name = "mesh_texture",
               .texture_binding = 1,
               .sampler_binding = 2,
               .default_texture = "white"},
          }},
      // spherical_mesh mode: not typically used, but provide same bindings for consistency
      MaterialRequirements{
          .shader_name = "textured_mesh",
          .textures = {
              {.slot_name = "mesh_texture",
               .texture_binding = 1,
               .sampler_binding = 2,
               .default_texture = "white"},
          }});

  // terrain_blend.wesl - texture_2d_array of albedo layers, blended per-vertex.
  // Normally the array view is supplied as an instance override (see
  // MaterialLibrary::TerrainBlend); if it is missing, kTerrainBlend geometry
  // resolves the slot to the factory's neutral-gray e2DArray default
  // (GetDefaultTextureForSlot / TextureType::kArray) — a valid array view, so
  // the missing-texture case renders gray rather than failing validation.
  MaterialRequirements terrain_blend_reqs{
      .shader_name = "terrain_blend",
      .textures = {
          {.slot_name = "albedo_array",
           .texture_binding = 1,
           .sampler_binding = 2,
           .default_texture = "white"},
      }};
  RegisterMaterial("terrain_blend", terrain_blend_reqs, terrain_blend_reqs);
}

std::string MaterialRequirementsRegistry::ResolveName(
    const std::string& name) const {
  auto it = aliases_.find(name);
  if (it != aliases_.end()) {
    return it->second;
  }
  return name;
}

MaterialRequirements MaterialRequirementsRegistry::GetRequirements(
    const std::string& material_name, GeometryType geometry_type) const {
  // Resolve aliases first
  std::string resolved_name = ResolveName(material_name);
  auto key = MakeKey(resolved_name, geometry_type);
  auto it = requirements_.find(key);
  if (it != requirements_.end()) {
    return it->second;
  }
  // Return empty requirements for unknown materials
  return MaterialRequirements{};
}

void MaterialRequirementsRegistry::RegisterMaterial(
    const std::string& name, const MaterialRequirements& flat_2d,
    const MaterialRequirements& sphere_mode) {
  requirements_[MakeKey(name, GeometryType::kTexturedMesh)] = flat_2d;
  requirements_[MakeKey(name, GeometryType::kSphericalMesh)] = sphere_mode;
}

void MaterialRequirementsRegistry::RegisterAlias(
    const std::string& alias_name, const std::string& canonical_name) {
  aliases_[alias_name] = canonical_name;
}

bool MaterialRequirementsRegistry::HasMaterial(const std::string& name) const {
  std::string resolved_name = ResolveName(name);
  return requirements_.contains(MakeKey(resolved_name, GeometryType::kTexturedMesh));
}

MaterialRequirements DeriveRequirementsFromReflection(
    const std::string& shader_name,
    const std::vector<ReflectedBinding>& bindings) {
  MaterialRequirements result;
  result.shader_name = shader_name;

  // Collect group 0 texture and sampler bindings
  std::vector<uint32_t> texture_bindings;
  std::vector<uint32_t> sampler_bindings;

  for (const auto& b : bindings) {
    if (b.group != 0) continue;
    if (b.texture_type != wgpu::TextureSampleType::Undefined) {
      texture_bindings.push_back(b.binding);
    }
    if (b.sampler_type != wgpu::SamplerBindingType::Undefined) {
      sampler_bindings.push_back(b.binding);
    }
  }

  std::sort(texture_bindings.begin(), texture_bindings.end());
  std::sort(sampler_bindings.begin(), sampler_bindings.end());

  // Pair each texture with its nearest sampler by binding index
  // Convention: texture at N pairs with sampler at N+1
  for (uint32_t tex_binding : texture_bindings) {
    uint32_t sampler_binding = tex_binding + 1;
    // Verify the sampler actually exists
    bool has_sampler = std::find(sampler_bindings.begin(),
                                 sampler_bindings.end(),
                                 sampler_binding) != sampler_bindings.end();
    if (!has_sampler && !sampler_bindings.empty()) {
      // Fallback: use closest sampler
      sampler_binding = sampler_bindings[0];
      for (uint32_t sb : sampler_bindings) {
        if (sb > tex_binding) {
          sampler_binding = sb;
          break;
        }
      }
    }

    std::ostringstream name;
    name << "tex_" << tex_binding;

    result.textures.push_back(TextureRequirement{
        .slot_name = name.str(),
        .texture_binding = tex_binding,
        .sampler_binding = sampler_binding,
        .default_texture = "white",
    });
  }

  return result;
}

}  // namespace badlands
