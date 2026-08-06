#include "game/visual/tree_lod_model.hpp"

#include <algorithm>
#include <cmath>
#include <utility>
#include <vector>

#include <spdlog/spdlog.h>

#include "engine/rendering/geometry/textured_mesh_builders.hpp"
#include "game/geometry/leaf_voxelizer.hpp"
#include "game/geometry/tree_generator.hpp"
#include "game/visual/foliage_voxel_config.hpp"

namespace badlands {

namespace {

// A 1x1 ARM at the matte bark roughness (R=AO, G=roughness, B=metallic).
// Requested as a solid-colour spec rather than created here: this function is
// device-free by contract, and the field builder dedupes the request across
// every model in a forest.
SolidColorTextureSpec BarkArmTexture() {
  const uint8_t rough = static_cast<uint8_t>(
      std::lround(std::clamp(kTreeBarkRoughness, 0.0f, 1.0f) * 255.0f));
  return SolidColorTextureSpec{.slot_name = "arm",
                               .rgba = {255, rough, 0, 255}};
}

InstancedMaterialSpec BarkMaterial() {
  InstancedMaterialSpec spec;
  spec.shader_name = "instanced_gbuffer";
  spec.shader_path = "material/instanced_gbuffer";
  spec.cache_namespace = "tree_bark";
  // None, not Back: bark tubes are thin and the generator does not guarantee
  // consistent winding across grafted junctions.
  spec.cull_mode = wgpu::CullMode::None;
  spec.casts_shadow = true;
  // `albedo` is deliberately left unbound -- its "white" factory default times
  // `tint` reproduces a flat solid bark colour. `normal` is left unbound too,
  // which is now correct rather than merely tolerated: instanced_gbuffer is
  // registered in material_requirements.cpp, so that slot defaults to
  // flat_normal instead of the reflection path's white (which decoded to a
  // ~54 degree tilt).
  spec.solid_textures.push_back(BarkArmTexture());
  spec.uniforms["tint"] = MaterialParameterValue(glm::vec4(kTreeBarkColor, 1.0f));
  return spec;
}

InstancedMaterialSpec CrownMaterial(const TreeOptions& options) {
  InstancedMaterialSpec spec;
  spec.shader_name = "voxel_foliage";
  spec.shader_path = "material/voxel_foliage";
  spec.cache_namespace = "tree_crown";
  // Back: solid tets, not double-sided cards.
  spec.cull_mode = wgpu::CullMode::Back;
  spec.casts_shadow = true;
  // voxel_foliage declares no textures at all (see the .wesl), so no bindings.
  // `params`: x = roughness, y = translucency strength.
  spec.uniforms["tint"] =
      MaterialParameterValue(glm::vec4(options.leaves.tint, 1.0f));
  spec.uniforms["params"] = MaterialParameterValue(glm::vec4(
      kTreeBarkRoughness, options.leaves.transmission_strength, 0.0f, 0.0f));
  return spec;
}

}  // namespace

std::vector<InstancedMaterialSpec> TreeSubmeshMaterials(
    const TreeOptions& options) {
  return {BarkMaterial(), CrownMaterial(options)};
}

InstancedLodModel BuildTreeFieldModel(const TreeOptions& options,
                                      float target_height_m) {
  InstancedLodModel out;

  const std::vector<SkeletonBranch> skeleton = BuildTreeSkeleton(options);
  BarkMeshStats bark_stats;
  const TexturedMeshResult bark_lod0 =
      GenerateTreeMesh(options, skeleton, &bark_stats);
  // One line per model, not per junction -- BuildForestModels runs this under
  // ParallelFor for every model in the forest. A non-zero `fallback` is the
  // signal that some branch could not be merged into its parent and still costs
  // its own mesh component, which is what caps how far bark can decimate.
  if (bark_stats.fallback > 0) {
    spdlog::info(
        "bark graft: seed {} -- {} junctions, {} stitched ({} shrunk), {} FELL BACK",
        options.seed, bark_stats.junctions, bark_stats.stitched,
        bark_stats.shrunk, bark_stats.fallback);
  } else {
    spdlog::debug("bark graft: seed {} -- {} junctions all stitched ({} shrunk)",
                  options.seed, bark_stats.junctions, bark_stats.shrunk);
  }

  const float bark_h =
      bark_lod0.local_bounds.max.y - bark_lod0.local_bounds.min.y;
  const float native_h = std::max(bark_h, 0.001f);
  out.native_to_world_scale = target_height_m / native_h;
  out.target_height_m = target_height_m;

  out.submesh_materials = TreeSubmeshMaterials(options);

  const TexturedMeshResult leaves = GenerateLeafMesh(options, skeleton);
  out.levels.reserve(kFoliageVoxelWorldSizes.size());
  for (size_t lod = 0; lod < kFoliageVoxelWorldSizes.size(); ++lod) {
    // Bark: a COPY of LOD0 decimated by the shared per-level policy. This used
    // to happen inside BuildTreeField, which is exactly what made that builder
    // tree-shaped -- the policy belongs to the producer that knows what bark is.
    TexturedMeshResult bark = bark_lod0;
    SimplifyBarkForVoxelLod(bark.mesh, lod);

    LeafVoxelizeOptions voxel_opts;
    voxel_opts.cell_size = FoliageVoxelCellNativeM(lod, native_h);
    voxel_opts.position_jitter =
        FoliagePositionJitterForLod(lod, voxel_opts.position_jitter);
    TexturedMeshResult crown =
        VoxelizeLeafCards(leaves.mesh, options.leaves.silhouette, voxel_opts);

    out.levels.push_back({std::move(bark), std::move(crown)});
  }

  const auto thresholds = FoliageLodThresholdsForHeight(target_height_m);
  out.thresholds.assign(thresholds.begin(), thresholds.end());

  // The impostor bakes LOD 0's bark and VOXEL CROWN -- not the leaf cards. The
  // field's own LOD0 is the voxel crown, so an impostor baked from cards would
  // be a picture of a tree the chain never draws, and the switch to it would
  // change the tree's appearance rather than only its cost.
  out.impostor.transmission_strength = options.leaves.transmission_strength;
  out.impostor.roughness = kTreeBarkRoughness;
  // Leaves transmit, so the thickness pass earns its cost here.
  out.impostor.opaque = false;
  out.impostor.submeshes.push_back(ImpostorBakeSubmesh{
      .lod = 0,
      .submesh = kTreeBarkSubmesh,
      .tint = kTreeBarkColor,
      .albedo = nullptr,
      .voxel_brightness = 0.0f,
  });
  // The voxel crown is SOLID -- no cutout -- and its albedo is the voxelizer's
  // per-tet brightness times the leaf tint, which is what voxel_foliage.wesl
  // computes. Hence brightness 1.
  if (!out.levels.empty() &&
      out.levels[0][kTreeCrownSubmesh].mesh.vertex_count > 0) {
    out.impostor.submeshes.push_back(ImpostorBakeSubmesh{
        .lod = 0,
        .submesh = kTreeCrownSubmesh,
        .tint = options.leaves.tint,
        .albedo = nullptr,
        .voxel_brightness = 1.0f,
    });
  }

  return out;
}

}  // namespace badlands
