// Catch2 suite for the material texture-slot registry
// (engine/rendering/material/material_requirements.{hpp,cpp}).
//
// Pure CPU: the registry's constructor only fills a map, so this needs no
// device, no pipeline and no shader compilation.
//
// WHY THIS EXISTS. A slot-name mismatch between the registry and a caller's
// InstanceParams override is SILENT. StandardMaterialFactory::CreateInstance
// looks the override up by exact string (`dtv.param_name == req.slot_name`);
// a miss falls straight through to the slot's `default_texture` and renders a
// plausible-looking wrong material with nothing logged. That is exactly what
// happened to instanced_gbuffer's bark before it was registered here, and it
// is why the registered names are pinned rather than assumed.

#include <catch_amalgamated.hpp>

#include <algorithm>
#include <string>

#include "engine/rendering/material/material_requirements.hpp"

namespace {

const badlands::TextureRequirement* FindSlot(
    const badlands::MaterialRequirements& reqs, const std::string& name) {
  auto it = std::find_if(
      reqs.textures.begin(), reqs.textures.end(),
      [&](const badlands::TextureRequirement& t) { return t.slot_name == name; });
  return it == reqs.textures.end() ? nullptr : &*it;
}

}  // namespace

TEST_CASE("instanced_gbuffer resolves to named PBR slots, not reflection names",
          "[material_requirements]") {
  const badlands::MaterialRequirementsRegistry registry;

  // kInstancedMesh, not kTexturedMesh: this is the geometry type the instanced
  // field actually draws with, and MakeKey folds everything that is not
  // kSphericalMesh into the same "_flat" entry. If that folding ever changed,
  // the lookup would miss and every override in tree_field.cpp / the prop LOD
  // producer would silently no-op.
  const badlands::MaterialRequirements reqs = registry.GetRequirements(
      "instanced_gbuffer", badlands::GeometryType::kInstancedMesh);

  REQUIRE(reqs.textures.size() == 3);

  // Bindings must match shaders/material/instanced_gbuffer.wesl exactly:
  // albedo@1, sampler@2, normal@3, arm@4.
  const auto* albedo = FindSlot(reqs, "albedo");
  const auto* normal = FindSlot(reqs, "normal");
  const auto* arm = FindSlot(reqs, "arm");
  REQUIRE(albedo != nullptr);
  REQUIRE(normal != nullptr);
  REQUIRE(arm != nullptr);

  CHECK(albedo->texture_binding == 1);
  CHECK(normal->texture_binding == 3);
  CHECK(arm->texture_binding == 4);
  CHECK(albedo->sampler_binding == 2);
  CHECK(normal->sampler_binding == 2);
  CHECK(arm->sampler_binding == 2);

  // The normal slot must NOT default to "white". White decodes to
  // normalize(1,1,1) after the *2-1 unpack -- a ~54 degree tilt, not a flat
  // normal -- so an instanced material that binds no normal map would shade
  // wrong with no error anywhere. This is the half of the registration that is
  // a correctness fix rather than a naming convenience.
  CHECK(normal->default_texture == "flat_normal");
  CHECK(albedo->default_texture == "white");
  CHECK(arm->default_texture == "white");
}

TEST_CASE("instanced_gbuffer's slots match the normalmapped fork it came from",
          "[material_requirements]") {
  const badlands::MaterialRequirementsRegistry registry;

  // instanced_gbuffer.wesl is a fork of normalmapped.wesl that changes only the
  // vertex stage and group layout -- its group-0 textures are byte-identical.
  // Pinning them equal here means a future edit to one that forgets the other
  // fails loudly instead of drifting.
  const badlands::MaterialRequirements instanced = registry.GetRequirements(
      "instanced_gbuffer", badlands::GeometryType::kInstancedMesh);
  const badlands::MaterialRequirements normalmapped = registry.GetRequirements(
      "normalmapped", badlands::GeometryType::kTexturedMesh);

  REQUIRE(instanced.textures.size() == normalmapped.textures.size());
  for (size_t i = 0; i < instanced.textures.size(); ++i) {
    CAPTURE(i, instanced.textures[i].slot_name);
    CHECK(instanced.textures[i].slot_name == normalmapped.textures[i].slot_name);
    CHECK(instanced.textures[i].texture_binding ==
          normalmapped.textures[i].texture_binding);
    CHECK(instanced.textures[i].sampler_binding ==
          normalmapped.textures[i].sampler_binding);
    CHECK(instanced.textures[i].default_texture ==
          normalmapped.textures[i].default_texture);
  }
}

TEST_CASE("an unregistered shader still falls through to the reflection path",
          "[material_requirements]") {
  const badlands::MaterialRequirementsRegistry registry;

  // Registering instanced_gbuffer must not disturb the shaders that
  // deliberately stay unregistered and rely on
  // DeriveRequirementsFromReflection's "tex_<binding>" naming -- notably
  // foliage_impostor (impostor_atlas.cpp binds tex_1/tex_2) and foliage_cutout
  // (model_viewer_view.cpp binds tex_1). CreateInstance only reaches that
  // fallback when GetRequirements returns EMPTY textures, so emptiness is the
  // contract, not merely the current behaviour.
  CHECK_FALSE(registry.HasMaterial("foliage_impostor"));
  CHECK_FALSE(registry.HasMaterial("foliage_cutout"));
  CHECK(registry
            .GetRequirements("foliage_impostor",
                             badlands::GeometryType::kInstancedMesh)
            .textures.empty());
  CHECK(registry
            .GetRequirements("foliage_cutout",
                             badlands::GeometryType::kTexturedMesh)
            .textures.empty());
}
