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

  // standard_forward.wesl - general forward-opaque, standard-lit material (the
  // foliage/alpha-cutout material). One group-0 texture: the albedo silhouette
  // (RGB tint * alpha shape) at binding 1, sampler at binding 2 -- same slot
  // layout as normalmapped's albedo. Registering it here (rather than letting
  // DeriveRequirementsFromReflection name the slot "tex_1") lets callers supply
  // the leaf texture as an "albedo" override. Same reqs for both geometry modes
  // (only kTexturedMesh is used in practice).
  MaterialRequirements standard_forward_reqs{
      .shader_name = "standard_forward",
      .textures = {
          {.slot_name = "albedo",
           .texture_binding = 1,
           .sampler_binding = 2,
           .default_texture = "white"},
      }};
  RegisterMaterial("standard_forward", standard_forward_reqs,
                   standard_forward_reqs);

  // instanced_gbuffer.wesl - the instanced fork of normalmapped (see that
  // shader's header comment), with a byte-identical group-0 texture layout:
  // albedo@1 / sampler@2 / normal@3 / arm@4.
  //
  // Registered rather than left to DeriveRequirementsFromReflection for two
  // reasons. The names: reflection derives "tex_1"/"tex_3"/"tex_4", so every
  // caller wanting to bind a PBR pack had to know binding indices instead of
  // slot names -- a trap that cost tree_field.cpp a 16-line comment explaining
  // why the obvious "albedo"/"normal"/"arm" silently no-op'd. And the
  // DEFAULTS: reflection defaults every slot to "white", but a white normal
  // map decodes to normalize(1,1,1) after *2-1, a ~54 degree tilt rather than
  // a flat normal, so an instanced material that binds no normal map was
  // silently shading wrong. flat_normal below is what that slot has always
  // needed.
  //
  // Same reqs for both geometry modes: kInstancedMesh maps to the _flat
  // variant (see MakeKey), and its slots are all texture_2d.
  MaterialRequirements instanced_gbuffer_reqs{
      .shader_name = "instanced_gbuffer",
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
      }};
  RegisterMaterial("instanced_gbuffer", instanced_gbuffer_reqs,
                   instanced_gbuffer_reqs);

  // terrain_blend.wesl - three texture_2d_arrays (albedo / normal / ARM) whose
  // layers are blended per-vertex. All three share one sampler binding (2), the
  // same way normalmapped's three 2D slots do.
  //
  // Normally the array views are supplied as instance overrides (see
  // MaterialLibrary::TerrainBlend); if one is missing, kTerrainBlend geometry
  // resolves the slot to the factory's e2DArray default for that slot's
  // default_texture name (GetDefaultTextureForSlot / TextureType::kArray) — a
  // valid array view, so a missing texture renders neutrally rather than
  // failing validation. The per-slot default NAME matters: a normal array must
  // fall back to flat_normal (128,128,255), NOT gray — gray decodes to a
  // degenerate (0,0,0) normal after *2-1.
  MaterialRequirements terrain_blend_reqs{
      .shader_name = "terrain_blend",
      .textures = {
          {.slot_name = "albedo_array",
           .texture_binding = 1,
           .sampler_binding = 2,
           .default_texture = "white"},
          {.slot_name = "normal_array",
           .texture_binding = 3,
           .sampler_binding = 2,
           .default_texture = "flat_normal"},
          {.slot_name = "arm_array",
           .texture_binding = 4,
           .sampler_binding = 2,
           .default_texture = "default_arm"},
      }};
  RegisterMaterial("terrain_blend", terrain_blend_reqs, terrain_blend_reqs);

  // terrain_cluster.wesl - the same three layer arrays as terrain_blend (all
  // sharing sampler binding 2), plus two RGBA8 biome-weight splat planes on
  // their own sampler (binding 7). The splat needs a CLAMP sampler where the
  // arrays need REPEAT: it holds normalized weights over the map's own extent,
  // so a repeat sampler would fold the far edge onto the near one.
  //
  // Unlike terrain_blend, this material's slots MIX dimensionalities: the three
  // layer arrays are texture_2d_array, the two splat planes texture_2d. The
  // factory picks its unbound-slot default per GEOMETRY type, so it cannot get
  // both right -- kTerrainCluster stays on k2D, which suits the splat slots and
  // would be a dimension mismatch for the array ones. The array slots are
  // therefore not optional; ClusterTerrain::Build rejects a null view up front
  // rather than letting one reach Dawn. `default_texture` below is the name
  // that WOULD be used, kept accurate for the splat slots.
  MaterialRequirements terrain_cluster_reqs{
      .shader_name = "terrain_cluster",
      .textures = {
          {.slot_name = "albedo_array",
           .texture_binding = 1,
           .sampler_binding = 2,
           .default_texture = "white"},
          {.slot_name = "normal_array",
           .texture_binding = 3,
           .sampler_binding = 2,
           .default_texture = "flat_normal"},
          {.slot_name = "arm_array",
           .texture_binding = 4,
           .sampler_binding = 2,
           .default_texture = "default_arm"},
          {.slot_name = "biome_splat0",
           .texture_binding = 5,
           .sampler_binding = 7,
           .default_texture = "white"},
          {.slot_name = "biome_splat1",
           .texture_binding = 6,
           .sampler_binding = 7,
           .default_texture = "white"},
      }};
  RegisterMaterial("terrain_cluster", terrain_cluster_reqs,
                   terrain_cluster_reqs);
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
